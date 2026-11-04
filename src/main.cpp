// deltarec - offline PCM (WAV/FLAC) to 1-bit DSD (DSF/DFF) converter.
//
// Signal chain per channel:
//   PCM -> polyphase FIR (stage 1, sharp) -> polyphase FIR (stage 2, relaxed)
//       -> delta-sigma modulator (1-bit, noise shaped) -> DSF/DFF packer
// i.e. exactly what a DSD recorder's ADC does, performed numerically.
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "cuesheet.hpp"
#include "decoder.hpp"
#include "dsdwriter.hpp"
#include "id3v2_builder.hpp"
#include "modulator.hpp"
#include "ntf.hpp"
#include "resampler.hpp"

using namespace deltarec;
namespace fs = std::filesystem;

namespace {

const char* kVersion = "0.3.0";

constexpr int defaultOrderForRate(int rate) {
    if (rate == 64) return 7;
    if (rate == 128) return 5;
    return 4;  // DSD256, DSD512
}

struct Options {
    std::vector<std::string> inputs;
    std::string output;          // single-file output path (or treated as outDir if directory / multi-input)
    std::string outDir;          // output folder (supports %tag% tokens)
    std::string subfolderPattern;// optional subfolder pattern appended to outDir
    int rate = 256;              // DSD64 / 128 / 256 / 512
    uint32_t base = 44100;       // 44100 or 48000 family
    bool baseExplicit = false;
    std::string format;          // dsf | dff (default from extension)
    int order = 0;               // 0 = auto per rate (DSD64=7, DSD128=5, DSD256/512=4)
    double hinf = 1.4;
    double gainDb = 0.0;         // extra gain; 0 dB => PCM FS -> 50% modulation
    double limit = 0.6;          // modulator input hard limit (modulation units)
    double dither = 0.05;        // TPDF dither amplitude at quantiser
    double passband = 0.0;       // Hz, 0 = auto (0.4535 * fs_in)
    double attenuation = 160.0;  // dB, anti-image filter stopband
    bool noTags = false;         // disable ID3v2 tag embedding
    bool noDiscSubfolder = false;// disable automatic discN subfolder on multi-disc albums
    bool info = false;
    bool quiet = false;
};

void usage() {
    std::fprintf(stderr,
        "deltarec %s - PCM (WAV/FLAC/CUE) -> DSD (DSF/DFF) converter\n"
        "\n"
        "usage: deltarec [options] input1.(wav|flac|cue) [input2.(wav|flac|cue) ...] [-d outdir] [-o output.dsf]\n"
        "\n"
        "  -d, --outdir DIR      output directory; supports %%tag%% placeholders such as\n"
        "                        \"D:\\DSD\\[%%album artist%%] %%album%%-(%%encoder%%)\"\n"
        "                        (creates disc1, disc2... subfolders automatically for multi-disc albums)\n"
        "  -S, --subfolder PAT   optional subfolder pattern under --outdir (e.g. \"[%%artist%%] %%album%%\")\n"
        "  -o, --output FILE     output file path (or output directory when converting multiple files)\n"
        "  -r, --rate N          DSD rate multiple: 64, 128, 256 (default), 512\n"
        "  -b, --base HZ         rate family: 44100 (default) or 48000\n"
        "  -f, --format FMT      dsf (default) or dff\n"
        "      --order N         modulator order 2..8 (default by rate: DSD64=7, DSD128=5, DSD256/512=4)\n"
        "      --hinf X          NTF out-of-band gain, Lee's rule (default 1.4, or preset:\n"
        "                        standard=1.45, gentle=1.35, aggressive=1.55)\n"
        "  -g, --gain DB         extra gain in dB (default 0: PCM 0 dBFS -> 50%% modulation,\n"
        "                        the SACD convention; use -3 to leave more headroom)\n"
        "      --limit X         hard limit on modulator input, 0..1 (default 0.6)\n"
        "      --dither X        TPDF dither at quantiser, 0 disables (default 0.05)\n"
        "      --passband HZ     anti-image filter passband edge (default 0.4535*fs_in,\n"
        "                        i.e. 20 kHz for 44.1 kHz material)\n"
        "      --attenuation DB  anti-image filter stopband attenuation (default 160)\n"
        "      --no-tags         do not copy source metadata tags / cover art into output\n"
        "      --no-disc-folder  do not create discN subfolders for multi-disc albums\n"
        "      --info            print the DSP design for these settings and exit\n"
        "  -q, --quiet           no progress output\n"
        "  -h, --help            this text\n"
        "  -V, --version         print version\n",
        kVersion);
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

fs::path utf8ToPath(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return fs::path(utf8);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), len);
    return fs::path(w);
#else
    return fs::path(utf8);
#endif
}

std::string pathToUtf8(const fs::path& p) {
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

bool isSupportedAudioExt(const fs::path& p) {
    std::string ext = toLowerAscii(pathToUtf8(p.extension()));
    return ext == ".flac" || ext == ".wav";
}

bool wildcardMatch(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0, starIdx = std::string::npos, matchIdx = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::tolower(static_cast<unsigned char>(pat[p])) ==
                                                std::tolower(static_cast<unsigned char>(str[s])))) {
            ++p;
            ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            starIdx = p++;
            matchIdx = s;
        } else if (starIdx != std::string::npos) {
            p = starIdx + 1;
            s = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

void expandInputArg(const std::string& arg, std::vector<std::string>& outFiles) {
    if (arg.find_first_of("*?") != std::string::npos) {
        fs::path p = utf8ToPath(arg);
        fs::path dir = p.parent_path();
        std::string filePat = pathToUtf8(p.filename());
        if (dir.empty()) dir = fs::path(".");
        std::error_code ec;
        std::vector<std::string> matched;
        if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string fname = pathToUtf8(entry.path().filename());
                if (wildcardMatch(filePat, fname)) {
                    matched.push_back(pathToUtf8(entry.path()));
                }
            }
        }
        std::sort(matched.begin(), matched.end());
        if (!matched.empty()) {
            outFiles.insert(outFiles.end(), matched.begin(), matched.end());
            return;
        }
    } else {
        fs::path p = utf8ToPath(arg);
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            std::vector<std::string> matched;
            for (const auto& entry : fs::directory_iterator(p, ec)) {
                if (entry.is_regular_file(ec) && isSupportedAudioExt(entry.path())) {
                    matched.push_back(pathToUtf8(entry.path()));
                }
            }
            std::sort(matched.begin(), matched.end());
            if (!matched.empty()) {
                outFiles.insert(outFiles.end(), matched.begin(), matched.end());
                return;
            }
        }
    }
    outFiles.push_back(arg);
}

bool parseArgs(const std::vector<std::string>& args, Options& o) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* name) -> const std::string& {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else if (a == "-V" || a == "--version") { std::printf("deltarec %s\n", kVersion); std::exit(0); }
        else if (a == "-o" || a == "--output") o.output = need("-o");
        else if (a == "-d" || a == "--outdir" || a == "--output-dir") o.outDir = need("-d");
        else if (a == "-S" || a == "--subfolder") o.subfolderPattern = need("--subfolder");
        else if (a == "-r" || a == "--rate") o.rate = std::atoi(need("-r").c_str());
        else if (a == "-b" || a == "--base") {
            o.base = static_cast<uint32_t>(std::atoi(need("-b").c_str()));
            o.baseExplicit = true;
        }
        else if (a == "-f" || a == "--format") o.format = toLowerAscii(need("-f"));
        else if (a == "--order") o.order = std::atoi(need("--order").c_str());
        else if (a == "--hinf") {
            std::string hval = toLowerAscii(need("--hinf"));
            if (hval == "standard") o.hinf = 1.45;
            else if (hval == "gentle") o.hinf = 1.35;
            else if (hval == "aggressive") o.hinf = 1.55;
            else o.hinf = std::atof(hval.c_str());
        }
        else if (a == "-g" || a == "--gain") o.gainDb = std::atof(need("-g").c_str());
        else if (a == "--limit") o.limit = std::atof(need("--limit").c_str());
        else if (a == "--dither") o.dither = std::atof(need("--dither").c_str());
        else if (a == "--passband") o.passband = std::atof(need("--passband").c_str());
        else if (a == "--attenuation") o.attenuation = std::atof(need("--attenuation").c_str());
        else if (a == "--no-tags") o.noTags = true;
        else if (a == "--no-disc-folder" || a == "--no-disc-subfolder") o.noDiscSubfolder = true;
        else if (a == "--info") o.info = true;
        else if (a == "-q" || a == "--quiet") o.quiet = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        } else {
            expandInputArg(a, o.inputs);
        }
    }

    if (o.inputs.empty()) return false;
    if (!(o.rate == 64 || o.rate == 128 || o.rate == 256 || o.rate == 512)) {
        std::fprintf(stderr, "--rate must be 64, 128, 256 or 512\n");
        return false;
    }
    if (!(o.base == 44100 || o.base == 48000)) {
        std::fprintf(stderr, "--base must be 44100 or 48000\n");
        return false;
    }
    // Set default modulator order based on DSD rate when --order is not explicitly given
    if (o.order == 0) {
        o.order = defaultOrderForRate(o.rate);
    }
    if (o.order < 2 || o.order > 8) {
        std::fprintf(stderr, "--order must be 2..8\n");
        return false;
    }
    if (o.limit <= 0.0 || o.limit >= 1.0) {
        std::fprintf(stderr, "--limit must be in (0,1)\n");
        return false;
    }

    // If -o was given, check whether it refers to a directory (or multiple inputs are present)
    if (!o.output.empty() && o.outDir.empty()) {
        std::string lowerOut = toLowerAscii(o.output);
        bool hasDsdExt = (lowerOut.size() >= 4 &&
                         (lowerOut.compare(lowerOut.size() - 4, 4, ".dsf") == 0 ||
                          lowerOut.compare(lowerOut.size() - 4, 4, ".dff") == 0));
        std::error_code ec;
        bool isExistingDir = fs::is_directory(utf8ToPath(o.output), ec);
        bool endsWithSlash = (o.output.back() == '/' || o.output.back() == '\\');
        if (o.inputs.size() > 1 || isExistingDir || endsWithSlash || (!hasDsdExt && o.output.find('%') != std::string::npos)) {
            o.outDir = o.output;
            o.output.clear();
        }
    }

    if (o.format.empty()) {
        std::string ext = o.output.size() >= 4 ? toLowerAscii(o.output.substr(o.output.size() - 4)) : "";
        o.format = (ext == ".dff") ? "dff" : "dsf";
    }
    if (o.format != "dsf" && o.format != "dff") {
        std::fprintf(stderr, "--format must be dsf or dff\n");
        return false;
    }
    return true;
}

// ---- Path / Tag Formatting & Multi-Disc Helpers --------------------------
std::string sanitizeFilename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            static_cast<unsigned char>(c) < 32) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "output";
    return out;
}

void getSourceFolderAndStem(const std::string& inputPath, std::string& folderOut, std::string& stemOut) {
    size_t slash = inputPath.find_last_of("/\\");
    std::string fname = (slash != std::string::npos) ? inputPath.substr(slash + 1) : inputPath;
    folderOut = (slash != std::string::npos) ? inputPath.substr(0, slash) : ".";
    size_t dot = fname.find_last_of('.');
    stemOut = (dot != std::string::npos && dot > 0) ? fname.substr(0, dot) : fname;
}

std::string resolvePropertyToken(
    const std::string& rawToken,
    const TrackMetadata& meta,
    const Options& o,
    const std::string& srcStem
) {
    std::string lower = toLowerAscii(rawToken);
    if (lower == "encoder" || lower == "dsd_rate" || lower == "rate") {
        return "DSD" + std::to_string(o.rate);
    }
    if (lower == "format") {
        return (o.format == "dff") ? "DFF" : "DSF";
    }
    if (lower == "filename") {
        return sanitizeFilename(srcStem.empty() ? "Unknown" : srcStem);
    }

    // Support both "%artist%" and "%arthist%" (common spelling variant), and fallbacks
    std::string val;
    if (lower == "artist" || lower == "arthist") {
        val = meta.getField("ARTIST");
        if (val.empty()) val = meta.getField("ALBUM ARTIST");
        if (val.empty()) val = meta.getField("ALBUMARTIST");
        if (val.empty()) val = "Unknown Artist";
    } else if (lower == "album artist" || lower == "albumartist") {
        val = meta.getField("ALBUM ARTIST");
        if (val.empty()) val = meta.getField("ALBUMARTIST");
        if (val.empty()) val = meta.getField("ARTIST");
        if (val.empty()) val = "Unknown Artist";
    } else if (lower == "album") {
        val = meta.getField("ALBUM");
        if (val.empty()) val = "Unknown Album";
    } else if (lower == "title") {
        val = meta.getField("TITLE");
        if (val.empty()) val = srcStem.empty() ? "Unknown Title" : srcStem;
    } else if (lower == "date" || lower == "year") {
        val = meta.getField("DATE");
        if (val.empty()) val = meta.getField("YEAR");
        if (val.empty()) val = "Unknown";
    } else if (lower == "tracknumber" || lower == "track") {
        val = meta.getField("TRACKNUMBER");
        if (val.empty()) val = meta.getField("TRACK");
        size_t slash = val.find('/');
        if (slash != std::string::npos) val = val.substr(0, slash);
        if (val.empty()) val = "00";
    } else if (lower == "discnumber" || lower == "disc") {
        val = meta.getField("DISCNUMBER");
        if (val.empty()) val = meta.getField("DISC");
        size_t slash = val.find('/');
        if (slash != std::string::npos) val = val.substr(0, slash);
        if (val.empty()) val = "1";
    } else {
        val = meta.getField(rawToken.c_str());
        if (val.empty()) val = "Unknown";
    }

    return sanitizeFilename(val);
}

// Expands %...% tokens in a path or subfolder pattern while preserving directory structure.
std::string expandPathPattern(
    const std::string& pattern,
    const TrackMetadata& meta,
    const Options& o,
    const std::string& srcStem
) {
    std::string expanded;
    expanded.reserve(pattern.size() + 64);

    size_t i = 0;
    const size_t n = pattern.size();
    while (i < n) {
        if (pattern[i] == '%') {
            size_t end = pattern.find('%', i + 1);
            if (end != std::string::npos) {
                if (end == i + 1) {
                    expanded.push_back('%');
                } else {
                    std::string token = pattern.substr(i + 1, end - i - 1);
                    expanded.append(resolvePropertyToken(token, meta, o, srcStem));
                }
                i = end + 1;
                continue;
            }
        }
        expanded.push_back(pattern[i]);
        ++i;
    }

    // Trim trailing spaces/dots on each path segment while preserving leading drive roots / UNC paths
    const char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    std::string norm = expanded;
    for (char& c : norm) {
        if (c == '/' || c == '\\') c = sep;
    }

    std::string prefix;
    size_t pos = 0;
#ifdef _WIN32
    if (norm.size() >= 2 && norm[1] == ':') {
        prefix = norm.substr(0, 2);
        pos = 2;
        if (pos < norm.size() && norm[pos] == sep) {
            prefix.push_back(sep);
            ++pos;
        }
    } else if (norm.size() >= 2 && norm[0] == sep && norm[1] == sep) {
        prefix = std::string(2, sep);
        pos = 2;
    }
#else
    if (!norm.empty() && norm[0] == sep) {
        prefix = std::string(1, sep);
        pos = 1;
    }
#endif

    std::string result = prefix;
    bool firstSeg = true;
    while (pos < norm.size()) {
        while (pos < norm.size() && norm[pos] == sep) ++pos;
        if (pos >= norm.size()) break;
        size_t nextSlash = norm.find(sep, pos);
        std::string seg = (nextSlash == std::string::npos)
            ? norm.substr(pos)
            : norm.substr(pos, nextSlash - pos);
        while (!seg.empty() && (seg.back() == ' ' || (seg.back() == '.' && seg != "." && seg != ".."))) {
            seg.pop_back();
        }
        if (!seg.empty()) {
            if (!firstSeg || (!result.empty() && result.back() != sep)) {
                result.push_back(sep);
            }
            result.append(seg);
            firstSeg = false;
        }
        if (nextSlash == std::string::npos) break;
        pos = nextSlash + 1;
    }
    return result;
}

std::string joinPath(const std::string& baseDir, const std::string& sub) {
    if (sub.empty()) return baseDir;
    if (baseDir.empty()) return sub;
    const char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    if (baseDir.back() == '/' || baseDir.back() == '\\') return baseDir + sub;
    return baseDir + sep + sub;
}

void parseDiscNumbersFromString(const std::string& str, int& discOut, int& totalOut) {
    if (str.empty()) return;
    const char* p = str.c_str();
    while (*p && (*p < '0' || *p > '9')) ++p;
    if (*p >= '0' && *p <= '9') {
        int d = std::atoi(p);
        if (d > 0 && discOut <= 0) discOut = d;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '/') {
            ++p;
            while (*p && (*p < '0' || *p > '9')) ++p;
            if (*p >= '0' && *p <= '9') {
                int t = std::atoi(p);
                if (t > totalOut) totalOut = t;
            }
        }
    }
}

void getTrackDiscInfo(const TrackMetadata& meta, int& discNumOut, int& totalDiscsOut) {
    discNumOut = 0;
    totalDiscsOut = 0;

    parseDiscNumbersFromString(meta.getField("DISCNUMBER"), discNumOut, totalDiscsOut);
    parseDiscNumbersFromString(meta.getField("DISC"), discNumOut, totalDiscsOut);

    const char* totalKeys[] = { "TOTALDISCS", "DISCTOTAL", "TOTALDISC" };
    for (const char* k : totalKeys) {
        std::string v = meta.getField(k);
        if (!v.empty()) {
            int dDummy = 0, tVal = 0;
            parseDiscNumbersFromString(v, tVal, dDummy);
            if (tVal > totalDiscsOut) totalDiscsOut = tVal;
        }
    }

    if (discNumOut > totalDiscsOut) {
        totalDiscsOut = discNumOut;
    }
}

// ---- DSP Plan & Pipeline -------------------------------------------------
uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

struct Plan {
    uint32_t fsIn = 0, fsDsd = 0, fi = 0;
    uint32_t L1 = 1, M1 = 1, L2 = 1;
    double passband = 0, stop1 = 0, stop2 = 0;
    double osr = 0;
    PolyphaseResampler::Spec s1, s2;
    NtfDesign ntf;
};

Plan makePlan(const Options& o, uint32_t fsIn) {
    Plan p;
    p.fsIn = fsIn;
    p.fsDsd = o.base * static_cast<uint32_t>(o.rate);
    // Stage-2 factor: largest power of two with intermediate rate >= 8 * fs_in.
    p.L2 = 1;
    while ((p.L2 * 2) <= 64 && p.fsDsd % (p.L2 * 2) == 0 && p.fsDsd / (p.L2 * 2) >= 8 * fsIn) p.L2 *= 2;
    p.fi = p.fsDsd / p.L2;
    uint32_t g = gcd32(p.fi, fsIn);
    p.L1 = p.fi / g;
    p.M1 = fsIn / g;
    p.passband = o.passband > 0 ? o.passband : 0.4535 * fsIn;
    if (p.passband >= 0.5 * fsIn) p.passband = 0.49 * fsIn;
    p.stop1 = std::min(0.5 * fsIn, p.fi - p.passband);
    p.stop2 = p.fi - p.passband;
    p.s1 = {p.L1, p.M1, p.passband, p.stop1, double(fsIn), o.attenuation};
    p.s2 = {p.L2, 1, p.passband, p.stop2, double(p.fi), o.attenuation};
    // OSR for the NTF: half the DSD rate over the audio band edge.  We fix the
    // band at 20 kHz (or the passband if wider) so hi-res sources keep their
    // ultrasonic content below the noise shelf.
    double band = std::max(20000.0, p.passband);
    p.osr = p.fsDsd / (2.0 * band);
    p.ntf = designNtf(o.order, p.osr, o.hinf);
    return p;
}

void printPlan(const Plan& p, const Options& o, uint32_t channels) {
    PolyphaseResampler r1(p.s1);
    std::fprintf(stderr, "  input      : %u Hz, %u ch\n", p.fsIn, channels);
    std::fprintf(stderr, "  output     : %u Hz 1-bit (DSD%d, %s family), %s\n", p.fsDsd, o.rate,
                 o.base == 44100 ? "44.1k" : "48k", o.format.c_str());
    std::fprintf(stderr, "  stage 1    : x%u/%u -> %u Hz, %u taps/phase (%u total), pass %.0f Hz, stop %.0f Hz\n",
                 p.L1, p.M1, p.fi, r1.tapsPerPhase(), r1.totalTaps(), p.passband, p.stop1);
    if (p.L2 > 1) {
        PolyphaseResampler r2(p.s2);
        std::fprintf(stderr, "  stage 2    : x%u -> %u Hz, %u taps/phase (%u total), stop %.0f Hz\n",
                     p.L2, p.fsDsd, r2.tapsPerPhase(), r2.totalTaps(), p.stop2);
    }
    std::fprintf(stderr, "  modulator  : order %d, H_inf %.2f, OSR %.1f, theoretical in-band SNR %.0f dB @ -6 dBFS\n",
                 o.order, o.hinf, p.osr, theoreticalSnrDb(p.ntf, 0.5));
    std::fprintf(stderr, "  level      : PCM 0 dBFS -> %.1f%% modulation (gain %+.1f dB), limiter at %.0f%%, dither %.3f\n",
                 50.0 * std::pow(10.0, o.gainDb / 20.0), o.gainDb, 100.0 * o.limit, o.dither);
    std::fprintf(stderr, "  NTF b      :");
    for (double v : p.ntf.b) std::fprintf(stderr, " %.9g", v);
    std::fprintf(stderr, "\n  NTF a      :");
    for (double v : p.ntf.a) std::fprintf(stderr, " %.9g", v);
    std::fprintf(stderr, "\n");
}

struct ChannelPipe {
    PolyphaseResampler s1, s2;
    bool twoStage = true;
    DeltaSigmaModulator mod;
    std::vector<double> buf1, buf2;
    std::vector<uint8_t> bits;

    static constexpr size_t kRingSlots = 8;
    static constexpr size_t kRingMask = kRingSlots - 1;
    struct TileSlot {
        std::vector<double> x;
        std::vector<double> d;
        size_t count = 0;
        bool isEnd = false;
    };
    TileSlot ring[kRingSlots];
    std::atomic<uint64_t> writeIdx{0};
    std::atomic<uint64_t> readIdx{0};

    std::thread prepThread;
    std::mutex prepMu;
    std::condition_variable prepCv;
    const double* prepIn = nullptr;
    size_t prepN = 0;
    bool prepFlush = false;
    bool prepHasWork = false;
    bool prepStop = false;

    ChannelPipe() {
        prepThread = std::thread([this] { prepLoop(); });
    }

    ~ChannelPipe() {
        {
            std::lock_guard<std::mutex> lk(prepMu);
            prepStop = true;
        }
        prepCv.notify_one();
        if (prepThread.joinable()) prepThread.join();
    }

    ChannelPipe(const ChannelPipe&) = delete;
    ChannelPipe& operator=(const ChannelPipe&) = delete;

    static inline void spinPause(uint32_t& spins) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64)
        if (spins < 1024) {
            _mm_pause();
            ++spins;
            return;
        }
#endif
        std::this_thread::yield();
    }

    void pushSlot(const double* src, size_t count, bool isEnd) {
        uint64_t w = writeIdx.load(std::memory_order_relaxed);
        uint32_t spins = 0;
        while (w - readIdx.load(std::memory_order_acquire) >= kRingSlots) {
            spinPause(spins);
        }
        TileSlot& slot = ring[w & kRingMask];
        slot.count = count;
        slot.isEnd = isEnd;
        if (count > 0) {
            if (slot.x.size() < count) {
                slot.x.resize(count);
                slot.d.resize(count);
            }
            mod.prepare(src, slot.x.data(), slot.d.data(), count);
        }
        writeIdx.store(w + 1, std::memory_order_release);
    }

    void prepLoop() {
        while (true) {
            const double* in = nullptr;
            size_t n = 0;
            bool flush = false;
            {
                std::unique_lock<std::mutex> lk(prepMu);
                prepCv.wait(lk, [this] { return prepStop || prepHasWork; });
                if (prepStop) return;
                in = prepIn;
                n = prepN;
                flush = prepFlush;
                prepHasWork = false;
            }

            constexpr size_t kInTile = 512;
            constexpr size_t kTile = 256;
            for (size_t inPos = 0; inPos < n; inPos += kInTile) {
                const size_t inChunk = std::min(kInTile, n - inPos);
                const bool lastIn = (inPos + inChunk == n);
                buf1.clear();
                s1.process(in + inPos, inChunk, buf1);
                if (flush && lastIn) s1.flush(buf1);

                const double* p1 = buf1.data();
                const size_t n1 = buf1.size();
                if (!twoStage) {
                    constexpr size_t kChunk = 4096;
                    for (size_t pos = 0; pos < n1; pos += kChunk) {
                        pushSlot(p1 + pos, std::min(kChunk, n1 - pos), false);
                    }
                } else {
                    for (size_t pos = 0; pos < n1; pos += kTile) {
                        const size_t chunk = std::min(kTile, n1 - pos);
                        buf2.clear();
                        s2.process(p1 + pos, chunk, buf2);
                        pushSlot(buf2.data(), buf2.size(), false);
                    }
                    if (flush && lastIn) {
                        buf2.clear();
                        s2.flush(buf2);
                        if (!buf2.empty()) pushSlot(buf2.data(), buf2.size(), false);
                    }
                }
            }
            if (n == 0 && flush) {
                buf1.clear();
                s1.flush(buf1);
                if (!twoStage) {
                    if (!buf1.empty()) pushSlot(buf1.data(), buf1.size(), false);
                } else {
                    if (!buf1.empty()) {
                        buf2.clear();
                        s2.process(buf1.data(), buf1.size(), buf2);
                        if (!buf2.empty()) pushSlot(buf2.data(), buf2.size(), false);
                    }
                    buf2.clear();
                    s2.flush(buf2);
                    if (!buf2.empty()) pushSlot(buf2.data(), buf2.size(), false);
                }
            }
            pushSlot(nullptr, 0, true);
        }
    }

    void run(const double* in, size_t n, bool flush) {
        bits.clear();
        const uint32_t totalL = twoStage ? (s1.L() * s2.L()) : s1.L();
        bits.reserve((n * totalL) / (8 * std::max(1u, s1.M())) + 256);

        // Kick off Stage 1 + Stage 2 + Dither/Limiter on the dedicated Prep thread.
        {
            std::lock_guard<std::mutex> lk(prepMu);
            prepIn = in;
            prepN = n;
            prepFlush = flush;
            prepHasWork = true;
        }
        prepCv.notify_one();

        // Consume prepared (x, d) tiles and run pure delta-sigma modulation on this thread.
        uint64_t r = readIdx.load(std::memory_order_relaxed);
        while (true) {
            uint32_t spins = 0;
            while (writeIdx.load(std::memory_order_acquire) == r) {
                spinPause(spins);
            }
            TileSlot& slot = ring[r & kRingMask];
            const size_t cnt = slot.count;
            const bool end = slot.isEnd;
            if (cnt > 0) {
                mod.modulatePrepared(slot.x.data(), slot.d.data(), cnt, bits);
            }
            readIdx.store(++r, std::memory_order_release);
            if (end) break;
        }
        if (flush) mod.finish(bits);
    }
};

// Persistent worker pool for channels 1 .. ch-1 (channel 0 runs on the caller thread).
class ChannelWorkerPool {
public:
    explicit ChannelWorkerPool(std::vector<std::unique_ptr<ChannelPipe>>& pipes) : pipes_(pipes) {
        const size_t workers = pipes_.size() > 1 ? pipes_.size() - 1 : 0;
        threads_.reserve(workers);
        for (size_t w = 0; w < workers; ++w) {
            const uint32_t chIdx = static_cast<uint32_t>(w + 1);
            threads_.emplace_back([this, chIdx] { workerLoop(chIdx); });
        }
    }

    ~ChannelWorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cvStart_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

    void runAll(const std::vector<std::vector<double>>& chan, size_t got, bool eof) {
        const uint32_t ch = static_cast<uint32_t>(pipes_.size());
        if (ch > 1) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                chanPtr_ = &chan;
                got_ = got;
                eof_ = eof;
                pending_ = ch - 1;
                ++gen_;
            }
            cvStart_.notify_all();
        }
        // Run channel 0 on the main thread concurrently.
        pipes_[0]->run(chan[0].data(), got, eof);

        if (ch > 1) {
            std::unique_lock<std::mutex> lk(mu_);
            cvDone_.wait(lk, [this] { return pending_ == 0; });
        }
    }

private:
    void workerLoop(uint32_t c) {
        uint64_t myGen = 0;
        while (true) {
            const std::vector<std::vector<double>>* chan = nullptr;
            size_t got = 0;
            bool eof = false;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cvStart_.wait(lk, [&] { return stop_ || gen_ != myGen; });
                if (stop_) return;
                myGen = gen_;
                chan = chanPtr_;
                got = got_;
                eof = eof_;
            }
            pipes_[c]->run((*chan)[c].data(), got, eof);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (--pending_ == 0) cvDone_.notify_one();
            }
        }
    }

    std::vector<std::unique_ptr<ChannelPipe>>& pipes_;
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cvStart_;
    std::condition_variable cvDone_;
    const std::vector<std::vector<double>>* chanPtr_ = nullptr;
    size_t got_ = 0;
    bool eof_ = false;
    uint32_t pending_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 0) {
        args.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
            if (len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
            }
            args.push_back(std::move(s));
        }
        LocalFree(wargv);
    } else
#endif
    {
        args.assign(argv, argv + argc);
    }

    Options o;
    if (!parseArgs(args, o)) { usage(); return 2; }

    struct WorkItem {
        std::string displayPath;
        std::string audioPath;
        std::string baseDir;
        std::string defaultStem;
        bool isCueTrack = false;
        uint64_t startSector75 = 0;
        bool hasEndSector75 = false;
        uint64_t endSector75 = 0;
        TrackMetadata cueMeta;
    };

    std::vector<WorkItem> workItems;
    int exitCode = 0;

    for (const std::string& inArg : o.inputs) {
        std::string ext = toLowerAscii(pathToUtf8(utf8ToPath(inArg).extension()));
        if (ext == ".cue") {
            try {
                std::vector<CueTrackItem> cueTracks = parseCueSheet(inArg);
                std::string cueFolder, cueStem;
                getSourceFolderAndStem(inArg, cueFolder, cueStem);
                for (auto& ct : cueTracks) {
                    WorkItem wi;
                    std::string title = ct.metadata.getField("TITLE");
                    char dispBuf[64];
                    std::snprintf(dispBuf, sizeof(dispBuf), " [Track %02d", ct.trackNumber);
                    wi.displayPath = inArg + dispBuf + (title.empty() ? "]" : (": " + title + "]"));
                    wi.audioPath = ct.audioPath;
                    wi.baseDir = cueFolder;
                    char stemBuf[32];
                    std::snprintf(stemBuf, sizeof(stemBuf), "%02d", ct.trackNumber);
                    if (!title.empty()) {
                        wi.defaultStem = std::string(stemBuf) + " - " + sanitizeFilename(title);
                    } else {
                        wi.defaultStem = std::string(stemBuf) + " - Track " + std::string(stemBuf);
                    }
                    wi.isCueTrack = true;
                    wi.startSector75 = ct.startFrame44k;
                    wi.hasEndSector75 = ct.hasEndSector;
                    wi.endSector75 = ct.endFrame44k;
                    wi.cueMeta = std::move(ct.metadata);
                    workItems.push_back(std::move(wi));
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "error (%s): %s\n", inArg.c_str(), e.what());
                exitCode = 1;
            }
        } else {
            WorkItem wi;
            wi.displayPath = inArg;
            wi.audioPath = inArg;
            getSourceFolderAndStem(inArg, wi.baseDir, wi.defaultStem);
            wi.isCueTrack = false;
            workItems.push_back(std::move(wi));
        }
    }

    if (workItems.empty()) {
        return exitCode != 0 ? exitCode : 1;
    }

    // If -o was given for a CUE file that expanded into multiple tracks, treat -o as outDir
    // unless it explicitly ends with .dsf or .dff
    if (!o.output.empty() && o.outDir.empty() && workItems.size() > 1) {
        o.outDir = o.output;
        o.output.clear();
    }

    const size_t trackCount = workItems.size();
    const bool hasOutDir = !o.outDir.empty() || !o.subfolderPattern.empty();

    // Pre-scan disc numbers across input items when an output folder is specified
    // so multi-disc albums automatically get disc1, disc2, ... subfolders.
    std::vector<int> trackDiscNums(trackCount, 0);
    std::vector<bool> trackIsMultiDisc(trackCount, false);
    if (hasOutDir && !o.noDiscSubfolder && !o.info) {
        struct AlbumDiscStats {
            std::string albumKey;
            int maxTotalDiscs = 0;
            int minDisc = 0;
            int maxDisc = 0;
        };
        std::vector<AlbumDiscStats> albumStats;
        std::vector<size_t> trackAlbumIdx(trackCount, 0);

        for (size_t i = 0; i < trackCount; ++i) {
            const WorkItem& wi = workItems[i];
            TrackMetadata meta;
            if (wi.isCueTrack) {
                meta = wi.cueMeta;
                if (meta.getField("DISCNUMBER").empty() && meta.getField("DISC").empty()) {
                    TrackMetadata audioMeta;
                    if (PcmDecoder::readMetadataOnly(wi.audioPath, audioMeta)) {
                        for (const auto& kv : audioMeta.fields) {
                            meta.setOrAddFieldIfEmpty(kv.first, kv.second);
                        }
                    }
                }
            } else {
                PcmDecoder::readMetadataOnly(wi.audioPath, meta);
            }
            int dNum = 0, tDiscs = 0;
            getTrackDiscInfo(meta, dNum, tDiscs);
            trackDiscNums[i] = dNum;

            std::string alb = meta.getField("ALBUM");
            std::string key = !alb.empty() ? toLowerAscii(alb) : std::string();
            if (key.empty()) {
                key = toLowerAscii(wi.baseDir);
            }

            size_t foundIdx = albumStats.size();
            for (size_t a = 0; a < albumStats.size(); ++a) {
                if (albumStats[a].albumKey == key) {
                    foundIdx = a;
                    break;
                }
            }
            if (foundIdx == albumStats.size()) {
                albumStats.push_back({key, 0, 0, 0});
            }
            trackAlbumIdx[i] = foundIdx;

            AlbumDiscStats& st = albumStats[foundIdx];
            if (tDiscs > st.maxTotalDiscs) st.maxTotalDiscs = tDiscs;
            if (dNum > 0) {
                if (st.minDisc == 0 || dNum < st.minDisc) st.minDisc = dNum;
                if (dNum > st.maxDisc) st.maxDisc = dNum;
            }
        }

        for (size_t i = 0; i < trackCount; ++i) {
            const AlbumDiscStats& st = albumStats[trackAlbumIdx[i]];
            bool multi = (st.maxTotalDiscs >= 2) ||
                         (st.minDisc > 0 && st.maxDisc > st.minDisc) ||
                         (st.maxDisc >= 2);
            trackIsMultiDisc[i] = multi;
        }
    }

    for (size_t idx = 0; idx < trackCount; ++idx) {
        const WorkItem& wi = workItems[idx];
        try {
            PcmDecoder dec(wi.audioPath, true);
            const uint32_t ch = dec.channels();
            const uint32_t fsIn = dec.sampleRate();
            if (ch == 0 || ch > 6) throw std::runtime_error("1..6 channels supported");
            Plan plan = makePlan(o, fsIn);

            uint64_t startFrame = 0;
            uint64_t endFrame = dec.totalFrames();
            if (wi.isCueTrack) {
                startFrame = CueTrackItem::sectorToSampleFrame(wi.startSector75, fsIn);
                if (startFrame > dec.totalFrames()) startFrame = dec.totalFrames();
                if (wi.hasEndSector75) {
                    uint64_t cueEnd = CueTrackItem::sectorToSampleFrame(wi.endSector75, fsIn);
                    endFrame = std::min(dec.totalFrames(), cueEnd);
                }
                if (endFrame < startFrame) endFrame = startFrame;
            }
            const uint64_t total = endFrame - startFrame;

            if (!o.quiet || o.info) {
                if (idx == 0) {
                    std::fprintf(stderr, "deltarec %s\n", kVersion);
                }
                if (trackCount > 1 || wi.isCueTrack) {
                    std::fprintf(stderr, "\n[%zu/%zu] %s\n", idx + 1, trackCount, wi.displayPath.c_str());
                }
                if (wi.isCueTrack) {
                    double startSec = double(startFrame) / double(fsIn);
                    double durSec = double(total) / double(fsIn);
                    std::fprintf(stderr, "  source     : %s, %u-bit, %s (offset %.2fs, duration %.2fs)\n",
                                 wi.audioPath.c_str(), dec.bitsPerSample(), dec.formatName(), startSec, durSec);
                } else {
                    std::fprintf(stderr, "  source     : %s, %u-bit, %s\n", wi.audioPath.c_str(), dec.bitsPerSample(), dec.formatName());
                }
                printPlan(plan, o, ch);
            }
            if (o.info) continue;

            // Merge metadata: for CUE tracks, CUE metadata takes priority, and backing file supplies coverArt / missing fields
            TrackMetadata effMeta;
            if (wi.isCueTrack) {
                effMeta = wi.cueMeta;
                const TrackMetadata& audioMeta = dec.metadata();
                for (const auto& kv : audioMeta.fields) {
                    effMeta.setOrAddFieldIfEmpty(kv.first, kv.second);
                }
                if (effMeta.coverArt.empty() && !audioMeta.coverArt.empty()) {
                    effMeta.coverArt = audioMeta.coverArt;
                }
            } else {
                effMeta = dec.metadata();
            }

            const std::string& srcFolder = wi.baseDir;
            const std::string& srcStem = wi.defaultStem;

            std::string outPath;
            if (!hasOutDir && !o.output.empty() && trackCount == 1) {
                outPath = (o.output.find('%') != std::string::npos)
                    ? expandPathPattern(o.output, effMeta, o, srcStem)
                    : o.output;
                fs::path parentDir = utf8ToPath(outPath).parent_path();
                if (!parentDir.empty()) {
                    std::error_code ec;
                    fs::create_directories(parentDir, ec);
                }
            } else {
                std::string targetDir = !o.outDir.empty()
                    ? expandPathPattern(o.outDir, effMeta, o, srcStem)
                    : srcFolder;
                if (!o.subfolderPattern.empty()) {
                    std::string sub = expandPathPattern(o.subfolderPattern, effMeta, o, srcStem);
                    if (!sub.empty()) {
                        targetDir = joinPath(targetDir, sub);
                    }
                }
                if (hasOutDir && !o.noDiscSubfolder && trackIsMultiDisc[idx]) {
                    int discNum = trackDiscNums[idx] > 0 ? trackDiscNums[idx] : 1;
                    std::string discFolder = "disc" + std::to_string(discNum);
                    targetDir = joinPath(targetDir, discFolder);
                }
                if (!targetDir.empty()) {
                    std::error_code ec;
                    fs::create_directories(utf8ToPath(targetDir), ec);
                    if (ec) {
                        throw std::runtime_error("cannot create output directory: " + targetDir + " (" + ec.message() + ")");
                    }
                }
                outPath = joinPath(targetDir, srcStem + "." + o.format);
            }

            if (wi.isCueTrack && startFrame > 0) {
                if (!dec.seek(startFrame)) {
                    throw std::runtime_error("failed to seek to track start frame in: " + wi.audioPath);
                }
            }

            const double gain = 0.5 * std::pow(10.0, o.gainDb / 20.0);

            std::vector<std::unique_ptr<ChannelPipe>> pipes(ch);
            for (uint32_t c = 0; c < ch; ++c) {
                pipes[c] = std::make_unique<ChannelPipe>();
                pipes[c]->s1.init(plan.s1);
                pipes[c]->twoStage = plan.L2 > 1;
                if (pipes[c]->twoStage) pipes[c]->s2.init(plan.s2);
                pipes[c]->mod = DeltaSigmaModulator(plan.ntf, o.limit, o.dither, 8.0, 0x5eed1234u + c * 7919u);
            }

            DsdWriter writer(outPath, o.format == "dff" ? DsdWriter::Format::DFF : DsdWriter::Format::DSF, ch, plan.fsDsd);
            ChannelWorkerPool pool(pipes);

            const size_t blockFrames = 16384;
            std::vector<double> inter;
            std::vector<std::vector<double>> chan(ch);
            std::vector<std::vector<uint8_t>> outBits(ch);
            uint64_t done = 0;
            auto t0 = std::chrono::steady_clock::now();
            int lastPct = -1;
            bool eof = false;

            while (!eof) {
                uint64_t rem = (total > done) ? (total - done) : 0;
                size_t toRead = static_cast<size_t>(std::min<uint64_t>(blockFrames, rem));
                size_t got = (toRead > 0) ? dec.read(toRead, inter) : 0;
                done += got;
                eof = (got < toRead) || (done >= total);
                for (uint32_t c = 0; c < ch; ++c) {
                    chan[c].resize(got);
                    double* __restrict dst = chan[c].data();
                    const double* __restrict src = inter.data() + c;
                    for (size_t i = 0; i < got; ++i) dst[i] = src[i * ch] * gain;
                }
                pool.runAll(chan, got, eof);
                for (uint32_t c = 0; c < ch; ++c) outBits[c].swap(pipes[c]->bits);
                writer.write(outBits);
                if (!o.quiet && total > 0) {
                    int pct = static_cast<int>(100.0 * done / total);
                    if (pct != lastPct) {
                        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        std::fprintf(stderr, "\r  converting : %3d%%  (%.1fx realtime)   ", pct, (done / double(fsIn)) / std::max(el, 1e-9));
                        std::fflush(stderr);
                        lastPct = pct;
                    }
                }
            }

            std::vector<uint8_t> id3Tag;
            if (!o.noTags) {
                id3Tag = buildId3v2Tag(effMeta);
            }
            writer.finish(pipes[0]->mod.stats().samples, id3Tag);
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            if (!o.quiet) {
                std::fprintf(stderr, "\r  converting : done in %.1f s (%.1fx realtime)      \n", el, (done / double(fsIn)) / std::max(el, 1e-9));
                double peak = 0; uint64_t clips = 0, resets = 0;
                for (auto& p : pipes) {
                    peak = std::max(peak, p->mod.stats().peakInput);
                    clips += p->mod.stats().inputClips;
                    resets += p->mod.stats().resets;
                }
                std::fprintf(stderr, "  peak level : %.1f%% modulation (%.2f dBFS PCM equivalent)\n", 100.0 * peak, 20.0 * std::log10(std::max(peak, 1e-12) / 0.5));
                if (!id3Tag.empty()) {
                    std::fprintf(stderr, "  tags       : embedded ID3v2.3 (%zu bytes)\n", id3Tag.size());
                }
                if (clips) std::fprintf(stderr, "  WARNING    : %llu samples hit the limiter - consider --gain -3\n", static_cast<unsigned long long>(clips));
                if (resets) std::fprintf(stderr, "  WARNING    : modulator reset %llu times (overload)\n", static_cast<unsigned long long>(resets));
                std::fprintf(stderr, "  wrote      : %s\n", outPath.c_str());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error (%s): %s\n", wi.displayPath.c_str(), e.what());
            exitCode = 1;
        }
    }
    return exitCode;
}
