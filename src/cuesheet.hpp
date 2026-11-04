// cuesheet.hpp - CUE sheet parser (UTF-8 / UTF-16 / Shift-JIS CP932 / ANSI) for splitting
// single-file or multi-file album images and populating per-track metadata and cover.jpg.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "id3v2_builder.hpp"

namespace deltarec {

struct CueTrackItem {
    std::string audioPath;       // Resolved path to the backing WAV/FLAC file
    int trackNumber = 0;         // 1..99
    uint64_t startFrame44k = 0;  // Start position in 1/75s CD sectors (frames_75 = mm*60*75 + ss*75 + ff)
    bool hasEndSector = false;
    uint64_t endFrame44k = 0;    // End position in 1/75s CD sectors (0 = until EOF)
    TrackMetadata metadata;      // Combined album + track metadata + cover.jpg from the CUE sheet

    // Converts 1/75-second CD sector index to exact PCM sample frame at `sampleRate`.
    static uint64_t sectorToSampleFrame(uint64_t sector75, uint32_t sampleRate) {
        uint64_t wholeSeconds = sector75 / 75;
        uint64_t remSectors   = sector75 % 75;
        return wholeSeconds * sampleRate + (remSectors * sampleRate + 37) / 75;
    }
};

namespace cuedetail {

inline std::filesystem::path utf8ToPath(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::filesystem::path(utf8);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), len);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(utf8);
#endif
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    std::wstring w = p.wstring();
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

inline bool isValidUtf8(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c < 0x80) {
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80 || c < 0xC2) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) return false;
            uint32_t cp = ((c & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len || (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80) return false;
            uint32_t cp = ((c & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12) |
                          ((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
inline bool tryDecodeCodePageToUtf8(UINT codePage, DWORD flags, const uint8_t* data, size_t len, std::string& outUtf8) {
    if (len == 0) {
        outUtf8.clear();
        return true;
    }
    int wlen = MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(data),
                                   static_cast<int>(len), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(data),
                        static_cast<int>(len), w.data(), wlen);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return false;
    outUtf8.assign(static_cast<size_t>(u8len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, outUtf8.data(), u8len, nullptr, nullptr);
    return true;
}
#endif

// Reads a CUE file of any common encoding (UTF-8 with/without BOM, UTF-16LE/BE, Shift-JIS CP932, or system ANSI)
// and always returns a normalized UTF-8 string.
inline std::string readFileAsUtf8(const std::string& cuePath) {
    std::FILE* f = nullptr;
#ifdef _WIN32
    std::wstring wpath = utf8ToPath(cuePath).wstring();
    if (!wpath.empty()) f = _wfopen(wpath.c_str(), L"rb");
#else
    f = std::fopen(cuePath.c_str(), "rb");
#endif
    if (!f) throw std::runtime_error("cannot open CUE file: " + cuePath);

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return {};
    }
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    raw.resize(got);

    // 1. Check UTF-8 BOM
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(raw.data() + 3), raw.size() - 3);
    }

    std::string utf8Content;
    bool wasUtf8 = false;

    // 2. Check UTF-16LE / UTF-16BE BOM
    if (raw.size() >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        utf8Content = id3detail::decodeId3String(1, raw.data(), raw.size());
    } else if (isValidUtf8(raw.data(), raw.size())) {
        // 3. Check valid UTF-8 without BOM
        utf8Content.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        wasUtf8 = true;
    } else {
#ifdef _WIN32
        // 4. Explicitly try Shift-JIS (Code Page 932) with strict validation first
        //    so Shift-JIS CUE files decode cleanly even when Windows "Beta: Use Unicode UTF-8
        //    for worldwide language support" sets CP_ACP to 65001 (or on non-Japanese locales).
        if (!tryDecodeCodePageToUtf8(932, MB_ERR_INVALID_CHARS, raw.data(), raw.size(), utf8Content)) {
            // 5. Try system ANSI code page (if not UTF-8) with strict validation
            UINT acp = GetACP();
            if (acp == 65001 || acp == 932 || !tryDecodeCodePageToUtf8(acp, MB_ERR_INVALID_CHARS, raw.data(), raw.size(), utf8Content)) {
                // 6. Permissive Shift-JIS CP932 fallback
                if (!tryDecodeCodePageToUtf8(932, 0, raw.data(), raw.size(), utf8Content)) {
                    // 7. Permissive Windows-1252 fallback
                    if (!tryDecodeCodePageToUtf8(1252, 0, raw.data(), raw.size(), utf8Content)) {
                        utf8Content.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
                    }
                }
            }
        }
#else
        utf8Content.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
#endif
    }

    // When the CUE file was not already UTF-8, convert and save the file back as UTF-8
    // so the CUE sheet text itself is no longer garbled.
    if (!wasUtf8 && !utf8Content.empty()) {
        std::FILE* wf = nullptr;
#ifdef _WIN32
        if (!wpath.empty()) wf = _wfopen(wpath.c_str(), L"wb");
#else
        wf = std::fopen(cuePath.c_str(), "wb");
#endif
        if (wf) {
            static const uint8_t kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
            std::fwrite(kUtf8Bom, 1, 3, wf);
            std::fwrite(utf8Content.data(), 1, utf8Content.size(), wf);
            std::fclose(wf);
        }
    }

    return utf8Content;
}

// Loads `cover.jpg` (case-insensitive) from the same directory as `cuePath` if present.
inline std::vector<uint8_t> loadCueFolderCoverJpg(const std::string& cuePath) {
    namespace fs = std::filesystem;
    fs::path cueP = utf8ToPath(cuePath);
    fs::path cueDir = cueP.parent_path();
    if (cueDir.empty()) cueDir = fs::path(".");

    std::error_code ec;
    fs::path targetPath;

    // Direct lookup first (case-insensitive on Windows)
    fs::path direct = cueDir / fs::path("cover.jpg");
    if (fs::exists(direct, ec) && fs::is_regular_file(direct, ec)) {
        targetPath = direct;
    } else if (fs::exists(cueDir, ec) && fs::is_directory(cueDir, ec)) {
        // Case-insensitive directory scan for case-sensitive filesystems (macOS / Linux)
        for (const auto& entry : fs::directory_iterator(cueDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string fname = TrackMetadata::toUpperAscii(pathToUtf8(entry.path().filename()));
            if (fname == "COVER.JPG") {
                targetPath = entry.path();
                break;
            }
        }
    }

    if (targetPath.empty()) return {};

    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(targetPath.wstring().c_str(), L"rb");
#else
    f = std::fopen(targetPath.string().c_str(), "rb");
#endif
    if (!f) return {};

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 4 || sz > 32 * 1024 * 1024) {
        std::fclose(f);
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    data.resize(got);
    return data;
}

inline std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string unquote(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

inline std::string firstWordUpper(const std::string& line, std::string& restOut) {
    std::string t = trim(line);
    size_t i = 0;
    while (i < t.size() && t[i] != ' ' && t[i] != '\t') ++i;
    std::string cmd = TrackMetadata::toUpperAscii(t.substr(0, i));
    restOut = (i < t.size()) ? trim(t.substr(i)) : std::string();
    return cmd;
}

// Parses "mm:ss:ff" (where ff is 0..74 CD frames, 75 frames = 1 second)
inline bool parseCueTime75(const std::string& s, uint64_t& sector75Out) {
    int mm = 0, ss = 0, ff = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &mm, &ss, &ff) != 3) return false;
    if (mm < 0 || ss < 0 || ss >= 60 || ff < 0 || ff >= 75) return false;
    sector75Out = static_cast<uint64_t>(mm) * 60 * 75 +
                  static_cast<uint64_t>(ss) * 75 +
                  static_cast<uint64_t>(ff);
    return true;
}

// Resolves the audio file referenced by a CUE `FILE "..."` directive.
// Falls back to .flac / .wav with the same stem or the CUE file's own stem if the original extension was changed.
inline std::string resolveCueAudioFile(const std::string& cuePath, const std::string& refFile) {
    namespace fs = std::filesystem;
    fs::path cueP = utf8ToPath(cuePath);
    fs::path cueDir = cueP.parent_path();
    if (cueDir.empty()) cueDir = fs::path(".");

    fs::path refP = utf8ToPath(refFile);
    fs::path candidate = refP.is_absolute() ? refP : (cueDir / refP);
    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
        return pathToUtf8(candidate);
    }

    // Try alternate lossless extensions (.flac, .wav) on the referenced filename stem
    fs::path refStem = candidate.parent_path() / candidate.stem();
    for (const char* ext : { ".flac", ".FLAC", ".wav", ".WAV" }) {
        fs::path alt = utf8ToPath(pathToUtf8(refStem) + ext);
        if (fs::exists(alt, ec) && fs::is_regular_file(alt, ec)) {
            return pathToUtf8(alt);
        }
    }

    // Try the CUE file's own stem (e.g. album.cue -> album.flac or album.wav)
    fs::path cueStem = cueDir / cueP.stem();
    for (const char* ext : { ".flac", ".FLAC", ".wav", ".WAV" }) {
        fs::path alt = utf8ToPath(pathToUtf8(cueStem) + ext);
        if (fs::exists(alt, ec) && fs::is_regular_file(alt, ec)) {
            return pathToUtf8(alt);
        }
    }

    // Also handle double extensions like "album.wav.cue" -> "album.flac" / "album.wav"
    if (cueP.stem().has_extension()) {
        fs::path innerStem = cueDir / cueP.stem().stem();
        for (const char* ext : { ".flac", ".FLAC", ".wav", ".WAV" }) {
            fs::path alt = utf8ToPath(pathToUtf8(innerStem) + ext);
            if (fs::exists(alt, ec) && fs::is_regular_file(alt, ec)) {
                return pathToUtf8(alt);
            }
        }
    }

    return pathToUtf8(candidate);
}

}  // namespace cuedetail

inline std::vector<CueTrackItem> parseCueSheet(const std::string& cuePath) {
    using namespace cuedetail;
    std::string content = readFileAsUtf8(cuePath);
    std::vector<uint8_t> folderCoverArt = loadCueFolderCoverJpg(cuePath);

    std::string albumTitle;
    std::string albumPerformer;
    std::string albumSongwriter;
    std::vector<std::pair<std::string, std::string>> albumRemTags;

    struct RawTrack {
        std::string fileRef;
        int number = 0;
        bool isAudio = false;
        std::string title;
        std::string performer;
        std::string songwriter;
        std::string isrc;
        std::vector<std::pair<std::string, std::string>> remTags;
        bool hasIndex00 = false;
        uint64_t index00 = 0;
        bool hasIndex01 = false;
        uint64_t index01 = 0;
    };

    std::string currentFile;
    std::vector<RawTrack> rawTracks;
    RawTrack* curTrack = nullptr;

    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find_first_of("\r\n", pos);
        std::string line = (eol == std::string::npos) ? content.substr(pos) : content.substr(pos, eol - pos);
        if (eol == std::string::npos) {
            pos = content.size();
        } else {
            pos = eol + 1;
            if (pos < content.size() && content[eol] == '\r' && content[pos] == '\n') ++pos;
        }

        std::string rest;
        std::string cmd = firstWordUpper(line, rest);
        if (cmd.empty()) continue;

        if (cmd == "FILE") {
            // FILE "filename.flac" WAVE  or  FILE filename.flac WAVE
            if (!rest.empty() && rest.front() == '"') {
                size_t closeQuote = rest.find('"', 1);
                if (closeQuote != std::string::npos) {
                    currentFile = rest.substr(1, closeQuote - 1);
                }
            } else {
                size_t lastSpace = rest.find_last_of(" \t");
                currentFile = (lastSpace != std::string::npos) ? trim(rest.substr(0, lastSpace)) : rest;
            }
        } else if (cmd == "TRACK") {
            std::string typeStr;
            std::string numStr = firstWordUpper(rest, typeStr);
            int tnum = std::atoi(numStr.c_str());
            std::string mode = TrackMetadata::toUpperAscii(trim(typeStr));
            rawTracks.push_back({});
            curTrack = &rawTracks.back();
            curTrack->fileRef = currentFile;
            curTrack->number = tnum;
            curTrack->isAudio = (mode == "AUDIO");
        } else if (cmd == "TITLE") {
            std::string val = unquote(rest);
            if (curTrack) curTrack->title = val;
            else albumTitle = val;
        } else if (cmd == "PERFORMER") {
            std::string val = unquote(rest);
            if (curTrack) curTrack->performer = val;
            else albumPerformer = val;
        } else if (cmd == "SONGWRITER") {
            std::string val = unquote(rest);
            if (curTrack) curTrack->songwriter = val;
            else albumSongwriter = val;
        } else if (cmd == "ISRC") {
            if (curTrack) curTrack->isrc = unquote(rest);
        } else if (cmd == "CATALOG") {
            std::string val = unquote(rest);
            if (!val.empty()) albumRemTags.emplace_back("CATALOG", val);
        } else if (cmd == "REM") {
            std::string remVal;
            std::string remKey = firstWordUpper(rest, remVal);
            remVal = unquote(remVal);
            if (!remKey.empty() && !remVal.empty()) {
                if (curTrack) curTrack->remTags.emplace_back(remKey, remVal);
                else albumRemTags.emplace_back(remKey, remVal);
            }
        } else if (cmd == "INDEX" && curTrack) {
            std::string timeStr;
            std::string idxStr = firstWordUpper(rest, timeStr);
            int idxNum = std::atoi(idxStr.c_str());
            uint64_t sec75 = 0;
            if (parseCueTime75(unquote(timeStr), sec75)) {
                if (idxNum == 0) {
                    curTrack->hasIndex00 = true;
                    curTrack->index00 = sec75;
                } else if (idxNum == 1) {
                    curTrack->hasIndex01 = true;
                    curTrack->index01 = sec75;
                }
            }
        }
    }

    // Count total audio tracks
    int totalAudioTracks = 0;
    for (const auto& rt : rawTracks) {
        if (rt.isAudio && (rt.hasIndex01 || rt.hasIndex00)) ++totalAudioTracks;
    }
    if (totalAudioTracks == 0) {
        throw std::runtime_error("no playable AUDIO tracks found in CUE file: " + cuePath);
    }

    std::vector<CueTrackItem> items;
    items.reserve(static_cast<size_t>(totalAudioTracks));

    for (size_t i = 0; i < rawTracks.size(); ++i) {
        const RawTrack& rt = rawTracks[i];
        if (!rt.isAudio || (!rt.hasIndex01 && !rt.hasIndex00)) continue;

        CueTrackItem item;
        item.audioPath = resolveCueAudioFile(cuePath, rt.fileRef);
        item.trackNumber = rt.number > 0 ? rt.number : static_cast<int>(items.size() + 1);
        item.startFrame44k = rt.hasIndex01 ? rt.index01 : rt.index00;

        // Determine end sector if the next track in the CUE sheet uses the same backing file
        for (size_t j = i + 1; j < rawTracks.size(); ++j) {
            if (rawTracks[j].fileRef == rt.fileRef && (rawTracks[j].hasIndex01 || rawTracks[j].hasIndex00)) {
                uint64_t nextStart = rawTracks[j].hasIndex01 ? rawTracks[j].index01 : rawTracks[j].index00;
                if (nextStart > item.startFrame44k) {
                    item.hasEndSector = true;
                    item.endFrame44k = nextStart;
                }
                break;
            }
        }

        // Populate metadata from CUE sheet
        char trkBuf[16], totBuf[16];
        std::snprintf(trkBuf, sizeof(trkBuf), "%02d", item.trackNumber);
        std::snprintf(totBuf, sizeof(totBuf), "%02d", totalAudioTracks);

        if (!rt.title.empty()) item.metadata.addField("TITLE", rt.title);
        if (!rt.performer.empty()) {
            item.metadata.addField("ARTIST", rt.performer);
        } else if (!albumPerformer.empty()) {
            item.metadata.addField("ARTIST", albumPerformer);
        }
        if (!albumPerformer.empty()) {
            item.metadata.addField("ALBUM ARTIST", albumPerformer);
        }
        if (!albumTitle.empty()) {
            item.metadata.addField("ALBUM", albumTitle);
        }
        item.metadata.addField("TRACKNUMBER", trkBuf);
        item.metadata.addField("TOTALTRACKS", totBuf);

        if (!rt.songwriter.empty()) {
            item.metadata.addField("COMPOSER", rt.songwriter);
        } else if (!albumSongwriter.empty()) {
            item.metadata.addField("COMPOSER", albumSongwriter);
        }
        if (!rt.isrc.empty()) {
            item.metadata.addField("ISRC", rt.isrc);
        }

        // Track-level REM tags first, then album-level REM tags (if not already set)
        for (const auto& kv : rt.remTags) {
            item.metadata.setOrAddFieldIfEmpty(kv.first, kv.second);
        }
        for (const auto& kv : albumRemTags) {
            item.metadata.setOrAddFieldIfEmpty(kv.first, kv.second);
        }

        if (!folderCoverArt.empty()) {
            item.metadata.coverArt = folderCoverArt;
        }

        items.push_back(std::move(item));
    }

    return items;
}

}  // namespace deltarec
