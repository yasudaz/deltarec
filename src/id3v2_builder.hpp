// id3v2_builder.hpp - Builds an ID3v2.3 tag (UTF-16LE + BOM) from source track
// metadata and optional cover art for embedding into DSF and DFF files.
// Also parses existing ID3v2 chunks from WAV files so tag-based folder naming
// and tag preservation work across both FLAC (Vorbis comments) and WAV (LIST INFO / ID3v2).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deltarec {

struct TrackMetadata {
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<uint8_t> coverArt;
    std::vector<uint8_t> rawId3v2;

    static std::string toUpperAscii(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            out.push_back(static_cast<char>(std::toupper(c)));
        }
        return out;
    }

    void addField(const std::string& key, const std::string& val) {
        if (key.empty() || val.empty()) return;
        fields.emplace_back(key, val);
    }

    void setOrAddFieldIfEmpty(const std::string& key, const std::string& val) {
        if (key.empty() || val.empty()) return;
        if (getField(key.c_str()).empty()) {
            fields.emplace_back(key, val);
        }
    }

    std::string getField(const char* name) const {
        if (!name || !*name) return {};
        std::string target = toUpperAscii(name);
        std::string out;
        for (const auto& kv : fields) {
            if (toUpperAscii(kv.first) == target && !kv.second.empty()) {
                if (!out.empty()) out += "; ";
                out += kv.second;
            }
        }
        return out;
    }

    bool empty() const {
        return fields.empty() && coverArt.empty() && rawId3v2.empty();
    }
};

namespace id3detail {

inline void appendU32BE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

inline uint32_t readSynchsafe32(const uint8_t* p) {
    return ((static_cast<uint32_t>(p[0]) & 0x7F) << 21) |
           ((static_cast<uint32_t>(p[1]) & 0x7F) << 14) |
           ((static_cast<uint32_t>(p[2]) & 0x7F) << 7)  |
           (static_cast<uint32_t>(p[3]) & 0x7F);
}

inline void appendSynchsafe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
    buf.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
    buf.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
    buf.push_back(static_cast<uint8_t>(v & 0x7F));
}

inline void appendCodepointUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Appends UTF-16LE with BOM (0xFF 0xFE) for a UTF-8 string (without trailing null).
inline void appendUtf16WithBom(std::vector<uint8_t>& buf, const std::string& utf8) {
    buf.push_back(0xFF);
    buf.push_back(0xFE);
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        uint32_t cp = 0;
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        if (c < 0x80) {
            cp = c;
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0F) << 12) |
                 ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((c & 0x07) << 18) |
                 ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<uint8_t>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            ++i;
        }

        if (cp <= 0xFFFF) {
            buf.push_back(static_cast<uint8_t>(cp & 0xFF));
            buf.push_back(static_cast<uint8_t>((cp >> 8) & 0xFF));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            uint16_t hi = static_cast<uint16_t>(0xD800 + ((cp >> 10) & 0x3FF));
            uint16_t lo = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
            buf.push_back(static_cast<uint8_t>(hi & 0xFF));
            buf.push_back(static_cast<uint8_t>((hi >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(lo & 0xFF));
            buf.push_back(static_cast<uint8_t>((lo >> 8) & 0xFF));
        }
    }
}

inline bool isValidUtf8Bytes(const uint8_t* data, size_t len) {
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

inline std::string ensureUtf8String(const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(data + 3), len - 3);
    }
    if (isValidUtf8Bytes(data, len)) {
        return std::string(reinterpret_cast<const char*>(data), len);
    }
#ifdef _WIN32
    auto tryCp = [&](UINT cp, DWORD flags, std::string& out) -> bool {
        int wlen = MultiByteToWideChar(cp, flags, reinterpret_cast<const char*>(data), static_cast<int>(len), nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(cp, flags, reinterpret_cast<const char*>(data), static_cast<int>(len), w.data(), wlen);
        int u8len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return false;
        out.assign(static_cast<size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), u8len, nullptr, nullptr);
        return true;
    };
    std::string u8;
    if (tryCp(932, MB_ERR_INVALID_CHARS, u8)) return u8;
    UINT acp = GetACP();
    if (acp != 65001 && acp != 932 && tryCp(acp, MB_ERR_INVALID_CHARS, u8)) return u8;
    if (tryCp(932, 0, u8)) return u8;
#endif
    std::string out;
    for (size_t i = 0; i < len && data[i] != 0; ++i) {
        appendCodepointUtf8(out, data[i]);
    }
    return out;
}

inline std::string decodeId3String(uint8_t encoding, const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};
    std::string out;
    if (encoding == 0) {
        // ISO-8859-1 or legacy Shift-JIS / ANSI -> UTF-8
        size_t end = 0;
        while (end < len && data[end] != 0) ++end;
        return ensureUtf8String(data, end);
    } else if (encoding == 3) {
        // UTF-8
        size_t end = 0;
        while (end < len && data[end] != 0) ++end;
        out.assign(reinterpret_cast<const char*>(data), end);
    } else if (encoding == 1 || encoding == 2) {
        // UTF-16 with BOM (1) or UTF-16BE without BOM (2)
        bool bigEndian = (encoding == 2);
        size_t pos = 0;
        if (encoding == 1 && len >= 2) {
            if (data[0] == 0xFE && data[1] == 0xFF) {
                bigEndian = true;
                pos = 2;
            } else if (data[0] == 0xFF && data[1] == 0xFE) {
                bigEndian = false;
                pos = 2;
            }
        }
        auto readU16 = [&](size_t idx) -> uint16_t {
            return bigEndian
                ? static_cast<uint16_t>((static_cast<uint16_t>(data[idx]) << 8) | data[idx + 1])
                : static_cast<uint16_t>(data[idx] | (static_cast<uint16_t>(data[idx + 1]) << 8));
        };
        while (pos + 1 < len) {
            uint16_t w1 = readU16(pos);
            pos += 2;
            if (w1 == 0) break;
            if (w1 >= 0xD800 && w1 <= 0xDBFF && pos + 1 < len) {
                uint16_t w2 = readU16(pos);
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                    pos += 2;
                    uint32_t cp = 0x10000 + (((static_cast<uint32_t>(w1) - 0xD800) << 10) |
                                             (static_cast<uint32_t>(w2) - 0xDC00));
                    appendCodepointUtf8(out, cp);
                    continue;
                }
            }
            appendCodepointUtf8(out, w1);
        }
    }
    return out;
}

inline void appendFrame(std::vector<uint8_t>& frames, const char id[4], const std::vector<uint8_t>& payload) {
    if (payload.empty()) return;
    frames.insert(frames.end(), id, id + 4);
    appendU32BE(frames, static_cast<uint32_t>(payload.size()));
    frames.push_back(0x00);
    frames.push_back(0x00);
    frames.insert(frames.end(), payload.begin(), payload.end());
}

inline void addTextFrame(std::vector<uint8_t>& frames, const char id[4], const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);  // UTF-16 with BOM
    appendUtf16WithBom(payload, text);
    appendFrame(frames, id, payload);
}

inline void addTxxxFrame(std::vector<uint8_t>& frames, const std::string& desc, const std::string& val) {
    if (desc.empty() || val.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);  // UTF-16 with BOM
    appendUtf16WithBom(payload, desc);
    payload.push_back(0x00);
    payload.push_back(0x00);
    appendUtf16WithBom(payload, val);
    appendFrame(frames, "TXXX", payload);
}

inline void addCommOrUsltFrame(std::vector<uint8_t>& frames, const char id[4], const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);  // UTF-16 with BOM
    payload.push_back('e');
    payload.push_back('n');
    payload.push_back('g');
    // Empty content descriptor with BOM + UTF-16 null terminator
    payload.push_back(0xFF);
    payload.push_back(0xFE);
    payload.push_back(0x00);
    payload.push_back(0x00);
    appendUtf16WithBom(payload, text);
    appendFrame(frames, id, payload);
}

inline void addApicFrame(std::vector<uint8_t>& frames, const void* imgData, size_t imgSize) {
    if (!imgData || imgSize < 4) return;
    const auto* b = static_cast<const uint8_t*>(imgData);
    const char* mime = "image/jpeg";
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') mime = "image/png";
    else if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') mime = "image/gif";
    else if (b[0] == 'B' && b[1] == 'M') mime = "image/bmp";

    std::vector<uint8_t> payload;
    payload.reserve(32 + imgSize);
    payload.push_back(0x00);  // ISO-8859-1 description encoding
    payload.insert(payload.end(), mime, mime + std::strlen(mime) + 1);  // null-terminated MIME
    payload.push_back(0x03);  // Picture type: Cover (front)
    payload.push_back(0x00);  // Empty description (null-terminated)
    payload.insert(payload.end(), b, b + imgSize);
    appendFrame(frames, "APIC", payload);
}

}  // namespace id3detail

// Parses an ID3v2.3 or ID3v2.4 tag byte block (e.g., from a WAV "id3 " chunk) into TrackMetadata.
inline void parseId3v2IntoMetadata(const uint8_t* data, size_t size, TrackMetadata& meta) {
    using namespace id3detail;
    if (!data || size < 10) return;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return;
    uint8_t ver = data[3];
    if (ver != 3 && ver != 4) return;
    uint8_t flags = data[5];
    uint32_t tagBodySize = readSynchsafe32(data + 6);
    size_t limit = std::min(size, static_cast<size_t>(10) + tagBodySize);

    size_t pos = 10;
    if ((flags & 0x40) != 0 && pos + 4 <= limit) {
        uint32_t extSize = (ver == 4) ? readSynchsafe32(data + pos) : readU32BE(data + pos);
        pos += (ver == 4) ? extSize : (4 + extSize);
    }

    while (pos + 10 <= limit) {
        if (data[pos] == 0) break;  // padding
        char fid[5] = {
            static_cast<char>(data[pos]),
            static_cast<char>(data[pos + 1]),
            static_cast<char>(data[pos + 2]),
            static_cast<char>(data[pos + 3]),
            '\0'
        };
        uint32_t fsize = (ver == 4) ? readSynchsafe32(data + pos + 4) : readU32BE(data + pos + 4);
        pos += 10;
        if (fsize == 0 || pos + fsize > limit) break;

        const uint8_t* fp = data + pos;
        std::string id(fid);
        if (id == "TXXX" && fsize >= 2) {
            uint8_t enc = fp[0];
            const uint8_t* p = fp + 1;
            size_t rem = fsize - 1;
            size_t descLen = 0;
            size_t step = (enc == 1 || enc == 2) ? 2 : 1;
            while (descLen + step <= rem) {
                if (step == 1 && p[descLen] == 0) break;
                if (step == 2 && p[descLen] == 0 && p[descLen + 1] == 0) break;
                descLen += step;
            }
            std::string desc = decodeId3String(enc, p, descLen);
            size_t valOffset = std::min(rem, descLen + step);
            std::string val = decodeId3String(enc, p + valOffset, rem - valOffset);
            meta.addField(desc, val);
        } else if (id[0] == 'T' && fsize >= 2) {
            std::string val = decodeId3String(fp[0], fp + 1, fsize - 1);
            if (id == "TIT2") meta.addField("TITLE", val);
            else if (id == "TPE1") meta.addField("ARTIST", val);
            else if (id == "TALB") meta.addField("ALBUM", val);
            else if (id == "TPE2") meta.addField("ALBUM ARTIST", val);
            else if (id == "TYER" || id == "TDRC") meta.addField("DATE", val);
            else if (id == "TRCK") meta.addField("TRACKNUMBER", val);
            else if (id == "TPOS") meta.addField("DISCNUMBER", val);
            else if (id == "TCON") meta.addField("GENRE", val);
            else if (id == "TCOM") meta.addField("COMPOSER", val);
            else if (id == "TPE3") meta.addField("CONDUCTOR", val);
            else if (id == "TPE4") meta.addField("PERFORMER", val);
            else if (id == "TCOP") meta.addField("COPYRIGHT", val);
            else if (id == "TPUB") meta.addField("PUBLISHER", val);
            else if (id == "TSRC") meta.addField("ISRC", val);
            else if (id == "TBPM") meta.addField("BPM", val);
        } else if ((id == "COMM" || id == "USLT") && fsize >= 5) {
            uint8_t enc = fp[0];
            const uint8_t* p = fp + 4;
            size_t rem = fsize - 4;
            size_t descLen = 0;
            size_t step = (enc == 1 || enc == 2) ? 2 : 1;
            while (descLen + step <= rem) {
                if (step == 1 && p[descLen] == 0) break;
                if (step == 2 && p[descLen] == 0 && p[descLen + 1] == 0) break;
                descLen += step;
            }
            size_t valOffset = std::min(rem, descLen + step);
            std::string val = decodeId3String(enc, p + valOffset, rem - valOffset);
            meta.addField(id == "COMM" ? "COMMENT" : "LYRICS", val);
        } else if (id == "APIC" && fsize >= 6 && meta.coverArt.empty()) {
            uint8_t enc = fp[0];
            size_t i = 1;
            while (i < fsize && fp[i] != 0) ++i;  // skip MIME
            if (i + 2 < fsize) {
                ++i;  // null after MIME
                ++i;  // picture type
                size_t step = (enc == 1 || enc == 2) ? 2 : 1;
                while (i + step <= fsize) {
                    if (step == 1 && fp[i] == 0) { i += 1; break; }
                    if (step == 2 && fp[i] == 0 && fp[i + 1] == 0) { i += 2; break; }
                    i += step;
                }
                if (i < fsize) {
                    meta.coverArt.assign(fp + i, fp + fsize);
                }
            }
        }
        pos += fsize;
    }
}

// Builds an ID3v2.3 byte array from `meta`.
inline std::vector<uint8_t> buildId3v2Tag(const TrackMetadata& meta) {
    using namespace id3detail;
    std::vector<uint8_t> frames;

    // Combined TRACKNUMBER/TOTALTRACKS and DISCNUMBER/TOTALDISCS
    std::string trk = meta.getField("TRACKNUMBER");
    if (trk.empty()) trk = meta.getField("TRACK");
    std::string totTrk = meta.getField("TOTALTRACKS");
    if (totTrk.empty()) totTrk = meta.getField("TRACKTOTAL");
    if (!trk.empty()) {
        if (!totTrk.empty() && trk.find('/') == std::string::npos) {
            addTextFrame(frames, "TRCK", trk + "/" + totTrk);
        } else {
            addTextFrame(frames, "TRCK", trk);
        }
    }

    std::string dsc = meta.getField("DISCNUMBER");
    if (dsc.empty()) dsc = meta.getField("DISC");
    std::string totDsc = meta.getField("TOTALDISCS");
    if (totDsc.empty()) totDsc = meta.getField("DISCTOTAL");
    if (totDsc.empty()) totDsc = meta.getField("TOTALDISC");
    if (!dsc.empty()) {
        if (!totDsc.empty() && dsc.find('/') == std::string::npos) {
            addTextFrame(frames, "TPOS", dsc + "/" + totDsc);
        } else {
            addTextFrame(frames, "TPOS", dsc);
        }
    }

    // Deduplicate keys so multi-value fields are emitted as a single joined frame
    std::vector<std::string> emittedKeys;
    auto wasEmitted = [&](const std::string& upperKey) {
        for (const auto& k : emittedKeys) {
            if (k == upperKey) return true;
        }
        emittedKeys.push_back(upperKey);
        return false;
    };

    for (const auto& kv : meta.fields) {
        std::string name = TrackMetadata::toUpperAscii(kv.first);
        if (wasEmitted(name)) continue;
        std::string val = meta.getField(kv.first.c_str());
        if (val.empty()) continue;

        if (name == "TRACKNUMBER" || name == "TRACK" || name == "TOTALTRACKS" || name == "TRACKTOTAL" ||
            name == "DISCNUMBER" || name == "DISC" || name == "TOTALDISCS" || name == "DISCTOTAL" || name == "TOTALDISC") {
            continue;  // already handled above
        } else if (name == "TITLE") {
            addTextFrame(frames, "TIT2", val);
        } else if (name == "ARTIST") {
            addTextFrame(frames, "TPE1", val);
        } else if (name == "ALBUM") {
            addTextFrame(frames, "TALB", val);
        } else if (name == "ALBUM ARTIST" || name == "ALBUMARTIST") {
            addTextFrame(frames, "TPE2", val);
        } else if (name == "DATE" || name == "YEAR") {
            addTextFrame(frames, "TYER", val);
        } else if (name == "GENRE") {
            addTextFrame(frames, "TCON", val);
        } else if (name == "COMPOSER") {
            addTextFrame(frames, "TCOM", val);
        } else if (name == "CONDUCTOR") {
            addTextFrame(frames, "TPE3", val);
        } else if (name == "PERFORMER" || name == "REMIXER") {
            addTextFrame(frames, "TPE4", val);
        } else if (name == "COPYRIGHT") {
            addTextFrame(frames, "TCOP", val);
        } else if (name == "PUBLISHER" || name == "LABEL" || name == "ORGANIZATION") {
            addTextFrame(frames, "TPUB", val);
        } else if (name == "ISRC") {
            addTextFrame(frames, "TSRC", val);
        } else if (name == "BPM" || name == "TEMPO") {
            addTextFrame(frames, "TBPM", val);
        } else if (name == "COMMENT") {
            addCommOrUsltFrame(frames, "COMM", val);
        } else if (name == "LYRICS" || name == "UNSYNCED LYRICS" || name == "UNSYNCEDLYRICS") {
            addCommOrUsltFrame(frames, "USLT", val);
        } else {
            addTxxxFrame(frames, kv.first, val);
        }
    }

    if (!meta.coverArt.empty()) {
        addApicFrame(frames, meta.coverArt.data(), meta.coverArt.size());
    }

    if (frames.empty()) {
        return meta.rawId3v2;
    }

    std::vector<uint8_t> tag;
    tag.reserve(10 + frames.size());
    tag.push_back('I');
    tag.push_back('D');
    tag.push_back('3');
    tag.push_back(0x03);  // ID3v2.3
    tag.push_back(0x00);  // revision 0
    tag.push_back(0x00);  // flags
    appendSynchsafe32(tag, static_cast<uint32_t>(frames.size()));
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

}  // namespace deltarec

