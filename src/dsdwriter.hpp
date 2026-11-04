// dsdwriter.hpp - DSF (Sony) and DFF (Philips DSDIFF) container writers.
//
// Both formats are publicly documented:
//   DSF : "DSF File Format Specification v1.01", Sony Corporation.
//   DFF : "Direct Stream Digital Interchange File Format (DSDIFF) v1.5", Philips.
// Bit order differs: DSF stores the oldest sample in the LSB of each byte and
// interleaves channels in 4096-byte blocks; DFF stores the oldest sample in
// the MSB and interleaves channels byte by byte.
//
// Input to write(): per-channel vectors of bytes packed LSB-first (DSF order).
// Supports optional embedded ID3v2 metadata chunk at the end of the file.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deltarec {

class DsdWriter {
public:
    enum class Format { DSF, DFF };

    DsdWriter(const std::string& path, Format fmt, uint32_t channels, uint32_t sampleRate)
        : fmt_(fmt), channels_(channels), rate_(sampleRate) {
        if (channels_ < 1 || channels_ > 6) throw std::runtime_error("1..6 channels supported");
#ifdef _WIN32
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wpath(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
            f_ = _wfopen(wpath.c_str(), L"wb");
        }
#else
        f_ = std::fopen(path.c_str(), "wb");
#endif
        if (!f_) throw std::runtime_error("cannot open output file: " + path);
        // Use a 256 KB stdio buffer to reduce syscall overhead.
        std::setvbuf(f_, nullptr, _IOFBF, 256 * 1024);
        pending_.assign(channels_, {});
        writeHeader();
    }

    ~DsdWriter() {
        if (f_) {
            try { finish(0); } catch (...) {}
        }
    }

    DsdWriter(const DsdWriter&) = delete;
    DsdWriter& operator=(const DsdWriter&) = delete;

    // packed[ch]: bytes, 8 samples each, LSB = oldest.  Equal length per channel.
    void write(const std::vector<std::vector<uint8_t>>& packed) {
        if (packed.size() != channels_) throw std::runtime_error("channel count mismatch");
        size_t n = packed[0].size();
        for (const auto& b : packed) if (b.size() != n) throw std::runtime_error("channel length mismatch");
        for (uint32_t ch = 0; ch < channels_; ++ch)
            pending_[ch].insert(pending_[ch].end(), packed[ch].begin(), packed[ch].end());
        drain(false);
    }

    // totalSamplesPerChannel is stored in the DSF header (DFF derives it from size).
    // id3v2Tag (optional): raw ID3v2 tag bytes to embed in the metadata chunk.
    void finish(uint64_t totalSamplesPerChannel, const std::vector<uint8_t>& id3v2Tag = {}) {
        if (!f_) return;
        drain(true);
        if (fmt_ == Format::DSF) {
            uint64_t dataChunkSize = 12 + bytesWrittenPerCh_ * channels_;
            uint64_t metaOffset = 0;
            uint64_t fileSize = 28 + 52 + dataChunkSize;
            if (!id3v2Tag.empty()) {
                metaOffset = fileSize;
                std::fwrite(id3v2Tag.data(), 1, id3v2Tag.size(), f_);
                fileSize += id3v2Tag.size();
            }
            seek64(12);
            putU64(fileSize);
            putU64(metaOffset);
            seek64(28 + 36);  // 'fmt ' chunk: sample-count field
            putU64(totalSamplesPerChannel);
            seek64(28 + 52 + 4);
            putU64(dataChunkSize);
        } else {
            uint64_t dsdChunkSize = bytesWrittenPerCh_ * channels_;
            uint64_t extraId3ChunkBytes = 0;
            if (!id3v2Tag.empty()) {
                putTag("ID3 ");
                putU64BE(id3v2Tag.size());
                std::fwrite(id3v2Tag.data(), 1, id3v2Tag.size(), f_);
                extraId3ChunkBytes = 12 + id3v2Tag.size();
                if (id3v2Tag.size() & 1) {
                    uint8_t pad = 0;
                    std::fwrite(&pad, 1, 1, f_);
                    ++extraId3ChunkBytes;
                }
            }
            uint64_t frm8Size = frm8BodyBeforeData_ + 12 + dsdChunkSize + extraId3ChunkBytes;
            seek64(4);
            putU64BE(frm8Size);
            seek64(static_cast<int64_t>(dsdChunkOffset_ + 4));
            putU64BE(dsdChunkSize);
        }
        std::fclose(f_);
        f_ = nullptr;
    }

    uint64_t bytesPerChannelWritten() const { return bytesWrittenPerCh_; }

private:
    void seek64(int64_t pos) {
#ifdef _WIN32
        _fseeki64(f_, pos, SEEK_SET);
#else
        fseeko(f_, pos, SEEK_SET);
#endif
    }

    int64_t tell64() {
#ifdef _WIN32
        return _ftelli64(f_);
#else
        return ftello(f_);
#endif
    }

    void putU32(uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f_); }
    void putU64(uint64_t v) { for (int i = 0; i < 8; ++i) { uint8_t b = uint8_t(v >> (8 * i)); std::fwrite(&b, 1, 1, f_); } }
    void putU32BE(uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 4, f_); }
    void putU64BE(uint64_t v) { for (int i = 7; i >= 0; --i) { uint8_t b = uint8_t(v >> (8 * i)); std::fwrite(&b, 1, 1, f_); } }
    void putU16BE(uint16_t v) { uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)}; std::fwrite(b, 1, 2, f_); }
    void putTag(const char* t) { std::fwrite(t, 1, 4, f_); }

    void writeHeader() {
        if (fmt_ == Format::DSF) {
            putTag("DSD "); putU64(28); putU64(0 /*file size, patched*/); putU64(0 /*metadata offset, patched*/);
            putTag("fmt "); putU64(52);
            putU32(1);          // format version
            putU32(0);          // format id: DSD raw
            // channel type: 1 mono, 2 stereo, 3 3ch, 4 quad, 5 4ch, 6 5ch, 7 5.1ch
            static const uint32_t chType[7] = {0, 1, 2, 3, 5, 6, 7};
            putU32(chType[channels_]);
            putU32(channels_);
            putU32(rate_);
            putU32(1);          // bits per sample
            putU64(0);          // sample count per channel (patched)
            putU32(4096);       // block size per channel
            putU32(0);          // reserved
            putTag("data"); putU64(0 /*patched*/);
        } else {
            putTag("FRM8"); putU64BE(0 /*patched*/); putTag("DSD ");
            int64_t start = tell64();
            putTag("FVER"); putU64BE(4); putU32BE(0x01050000);
            putTag("PROP"); putU64BE(0);  // patched below
            int64_t propStart = tell64();
            putTag("SND ");
            putTag("FS  "); putU64BE(4); putU32BE(rate_);
            putTag("CHNL"); putU64BE(2 + 4ull * channels_); putU16BE(uint16_t(channels_));
            static const char* stereo[] = {"SLFT", "SRGT"};
            static const char* five[] = {"MLFT", "MRGT", "C   ", "LS  ", "RS  ", "LFE "};
            for (uint32_t c = 0; c < channels_; ++c) {
                if (channels_ == 1) putTag("C   ");
                else if (channels_ == 2) putTag(stereo[c]);
                else putTag(five[c]);
            }
            putTag("CMPR"); putU64BE(4 + 1 + 14 + 1); putTag("DSD "); uint8_t len = 14; std::fwrite(&len, 1, 1, f_);
            std::fwrite("not compressed", 1, 14, f_); uint8_t pad = 0; std::fwrite(&pad, 1, 1, f_);
            int64_t propEnd = tell64();
            seek64(propStart - 8); putU64BE(uint64_t(propEnd - propStart));
            seek64(propEnd);
            dsdChunkOffset_ = static_cast<uint64_t>(propEnd);
            frm8BodyBeforeData_ = static_cast<uint64_t>(propEnd - start) + 4;  // + 'DSD ' form type
            putTag("DSD "); putU64BE(0 /*patched*/);
        }
    }

    static uint8_t reverseBits(uint8_t b) {
        b = uint8_t(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
        b = uint8_t(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
        b = uint8_t(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
        return b;
    }

    void drain(bool final) {
        size_t avail = pending_[0].size();
        if (fmt_ == Format::DSF) {
            const size_t blk = 4096;
            size_t consumed = 0;
            while (true) {
                size_t left = avail - consumed;
                if (left < blk && !(final && left > 0)) break;
                size_t n = std::min(left, blk);
                for (uint32_t ch = 0; ch < channels_; ++ch) {
                    std::fwrite(pending_[ch].data() + consumed, 1, n, f_);
                    if (n < blk) {  // zero-pad the final block
                        static const uint8_t zeros[4096] = {0};
                        std::fwrite(zeros, 1, blk - n, f_);
                    }
                }
                consumed += n;
                bytesWrittenPerCh_ += blk;
            }
            if (consumed) for (auto& p : pending_) p.erase(p.begin(), p.begin() + consumed);
        } else {
            if (avail == 0) return;
            std::vector<uint8_t> out(avail * channels_);
            for (size_t i = 0; i < avail; ++i)
                for (uint32_t ch = 0; ch < channels_; ++ch)
                    out[i * channels_ + ch] = reverseBits(pending_[ch][i]);
            std::fwrite(out.data(), 1, out.size(), f_);
            for (auto& p : pending_) p.clear();
            bytesWrittenPerCh_ += avail;
        }
    }

    Format fmt_;
    uint32_t channels_;
    uint32_t rate_;
    std::FILE* f_ = nullptr;
    std::vector<std::vector<uint8_t>> pending_;
    uint64_t bytesWrittenPerCh_ = 0;
    uint64_t dsdChunkOffset_ = 0;
    uint64_t frm8BodyBeforeData_ = 0;
};

}  // namespace deltarec
