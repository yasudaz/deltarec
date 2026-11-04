// decoder.hpp - WAV / FLAC input and metadata extraction via dr_wav and dr_flac.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "dr_flac.h"
#include "dr_wav.h"
#include "id3v2_builder.hpp"

namespace deltarec {

class PcmDecoder {
public:
    explicit PcmDecoder(const std::string& path, bool loadMetadata = true) {
        std::string lower = path;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool isFlac = lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".flac") == 0;
        if (!isFlac) {
            if (!openWav(path, loadMetadata)) {
                if (!openFlac(path, loadMetadata)) {
                    throw std::runtime_error("cannot open input (WAV/FLAC expected): " + path);
                }
            }
        } else {
            if (!openFlac(path, loadMetadata)) {
                throw std::runtime_error("cannot open FLAC: " + path);
            }
        }
        if (wav_) {
            channels_ = wav_->channels;
            rate_ = wav_->sampleRate;
            bits_ = wav_->bitsPerSample;
            frames_ = wav_->totalPCMFrameCount;
            isFloat_ = (wav_->translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT);
        } else {
            channels_ = flac_->channels;
            rate_ = flac_->sampleRate;
            bits_ = flac_->bitsPerSample;
            frames_ = flac_->totalPCMFrameCount;
        }
    }

    ~PcmDecoder() {
        if (wav_) drwav_uninit(wav_.get());
        if (flac_) drflac_close(flac_);
    }

    PcmDecoder(const PcmDecoder&) = delete;
    PcmDecoder& operator=(const PcmDecoder&) = delete;

    uint32_t channels() const { return channels_; }
    uint32_t sampleRate() const { return rate_; }
    uint32_t bitsPerSample() const { return bits_; }
    uint64_t totalFrames() const { return frames_; }  // may be 0 if unknown
    const char* formatName() const { return wav_ ? (isFloat_ ? "WAV (float)" : "WAV") : "FLAC"; }
    const TrackMetadata& metadata() const { return meta_; }

    // Read up to `frames` frames as interleaved doubles in [-1, 1).
    size_t read(size_t frames, std::vector<double>& out) {
        scratch_.resize(frames * channels_);
        uint64_t got = 0;
        if (wav_) {
            if (isFloat_) {
                fscratch_.resize(frames * channels_);
                got = drwav_read_pcm_frames_f32(wav_.get(), frames, fscratch_.data());
                out.resize(static_cast<size_t>(got) * channels_);
                for (size_t i = 0; i < out.size(); ++i) out[i] = fscratch_[i];
                return static_cast<size_t>(got);
            }
            got = drwav_read_pcm_frames_s32(wav_.get(), frames, scratch_.data());
        } else {
            got = drflac_read_pcm_frames_s32(flac_, frames, scratch_.data());
        }
        out.resize(static_cast<size_t>(got) * channels_);
        const double scale = 1.0 / 2147483648.0;
        for (size_t i = 0; i < out.size(); ++i) out[i] = scratch_[i] * scale;
        return static_cast<size_t>(got);
    }

    bool seek(uint64_t pcmFrame) {
        if (wav_) {
            return drwav_seek_to_pcm_frame(wav_.get(), pcmFrame) != 0;
        }
        if (flac_) {
            return drflac_seek_to_pcm_frame(flac_, pcmFrame) != 0;
        }
        return false;
    }

    static bool readMetadataOnly(const std::string& path, TrackMetadata& outMeta) {
        try {
            PcmDecoder dec(path, true);
            outMeta = dec.metadata();
            return true;
        } catch (...) {
            return false;
        }
    }

private:
#ifdef _WIN32
    static std::wstring toWide(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
        return w;
    }
#endif

    static void onFlacMeta(void* pUserData, drflac_metadata* pMetadata) {
        auto* self = static_cast<PcmDecoder*>(pUserData);
        if (!self || !pMetadata) return;
        if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            drflac_vorbis_comment_iterator it;
            drflac_init_vorbis_comment_iterator(
                &it,
                pMetadata->data.vorbis_comment.commentCount,
                pMetadata->data.vorbis_comment.pComments
            );
            drflac_uint32 commentLen = 0;
            const char* comment = nullptr;
            while ((comment = drflac_next_vorbis_comment(&it, &commentLen)) != nullptr) {
                if (commentLen == 0) continue;
                std::string s = id3detail::ensureUtf8String(
                    reinterpret_cast<const uint8_t*>(comment), commentLen);
                size_t eq = s.find('=');
                if (eq != std::string::npos && eq > 0 && eq + 1 < s.size()) {
                    std::string key = s.substr(0, eq);
                    std::string val = s.substr(eq + 1);
                    self->meta_.addField(key, val);
                }
            }
        } else if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
            const auto& pic = pMetadata->data.picture;
            if (pic.pPictureData && pic.pictureDataSize > 0) {
                // Prefer front cover (type 3), or take the first picture if none is stored yet.
                if (pic.type == DRFLAC_PICTURE_TYPE_COVER_FRONT || self->meta_.coverArt.empty()) {
                    self->meta_.coverArt.assign(pic.pPictureData, pic.pPictureData + pic.pictureDataSize);
                }
            }
        }
    }

    bool openFlac(const std::string& path, bool loadMetadata) {
#ifdef _WIN32
        std::wstring wpath = toWide(path);
        if (!wpath.empty()) {
            flac_ = loadMetadata
                ? drflac_open_file_with_metadata_w(wpath.c_str(), &PcmDecoder::onFlacMeta, this, nullptr)
                : drflac_open_file_w(wpath.c_str(), nullptr);
        }
#else
        flac_ = loadMetadata
            ? drflac_open_file_with_metadata(path.c_str(), &PcmDecoder::onFlacMeta, this, nullptr)
            : drflac_open_file(path.c_str(), nullptr);
#endif
        return flac_ != nullptr;
    }

    bool openWav(const std::string& path, bool loadMetadata) {
        wav_ = std::make_unique<drwav>();
        drwav_bool32 ok = DRWAV_FALSE;
#ifdef _WIN32
        std::wstring wpath = toWide(path);
        if (!wpath.empty()) {
            ok = loadMetadata
                ? drwav_init_file_with_metadata_w(wav_.get(), wpath.c_str(), 0, nullptr)
                : drwav_init_file_w(wav_.get(), wpath.c_str(), nullptr);
        }
#else
        ok = loadMetadata
            ? drwav_init_file_with_metadata(wav_.get(), path.c_str(), 0, nullptr)
            : drwav_init_file(wav_.get(), path.c_str(), nullptr);
#endif
        if (!ok) {
            wav_.reset();
            return false;
        }
        if (loadMetadata && wav_->pMetadata && wav_->metadataCount > 0) {
            extractWavMetadata();
        }
        return true;
    }

    void extractWavMetadata() {
        // 1. First inspect any embedded "id3 " or "ID3 " RIFF chunk (richer UTF-16/UTF-8 tags + cover art)
        for (drwav_uint32 i = 0; i < wav_->metadataCount; ++i) {
            const drwav_metadata& m = wav_->pMetadata[i];
            if (m.type == drwav_metadata_type_unknown &&
                m.data.unknown.pData && m.data.unknown.dataSizeInBytes >= 10) {
                const auto* id = m.data.unknown.id;
                if ((id[0] == 'i' || id[0] == 'I') &&
                    (id[1] == 'd' || id[1] == 'D') &&
                    id[2] == '3') {
                    meta_.rawId3v2.assign(
                        m.data.unknown.pData,
                        m.data.unknown.pData + m.data.unknown.dataSizeInBytes
                    );
                    parseId3v2IntoMetadata(m.data.unknown.pData, m.data.unknown.dataSizeInBytes, meta_);
                }
            }
        }

        // 2. Also read standard RIFF LIST INFO subchunks if not already populated by ID3v2
        for (drwav_uint32 i = 0; i < wav_->metadataCount; ++i) {
            const drwav_metadata& m = wav_->pMetadata[i];
            if ((m.type & drwav_metadata_type_list_all_info_strings) != 0 &&
                m.data.infoText.pString && m.data.infoText.stringLength > 0) {
                std::string raw(m.data.infoText.pString, m.data.infoText.stringLength);
                while (!raw.empty() && raw.back() == '\0') raw.pop_back();
                if (raw.empty()) continue;
                std::string val = id3detail::ensureUtf8String(
                    reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
                if (val.empty()) continue;

                switch (m.type) {
                case drwav_metadata_type_list_info_title:
                    meta_.setOrAddFieldIfEmpty("TITLE", val);
                    break;
                case drwav_metadata_type_list_info_artist:
                    meta_.setOrAddFieldIfEmpty("ARTIST", val);
                    break;
                case drwav_metadata_type_list_info_album:
                    meta_.setOrAddFieldIfEmpty("ALBUM", val);
                    break;
                case drwav_metadata_type_list_info_date:
                    meta_.setOrAddFieldIfEmpty("DATE", val);
                    break;
                case drwav_metadata_type_list_info_genre:
                    meta_.setOrAddFieldIfEmpty("GENRE", val);
                    break;
                case drwav_metadata_type_list_info_tracknumber:
                    meta_.setOrAddFieldIfEmpty("TRACKNUMBER", val);
                    break;
                case drwav_metadata_type_list_info_comment:
                    meta_.setOrAddFieldIfEmpty("COMMENT", val);
                    break;
                case drwav_metadata_type_list_info_copyright:
                    meta_.setOrAddFieldIfEmpty("COPYRIGHT", val);
                    break;
                case drwav_metadata_type_list_info_organization:
                    meta_.setOrAddFieldIfEmpty("PUBLISHER", val);
                    break;
                default:
                    break;
                }
            }
        }
    }

    std::unique_ptr<drwav> wav_;
    drflac* flac_ = nullptr;
    uint32_t channels_ = 0, rate_ = 0, bits_ = 0;
    uint64_t frames_ = 0;
    bool isFloat_ = false;
    TrackMetadata meta_;
    std::vector<int32_t> scratch_;
    std::vector<float> fscratch_;
};

}  // namespace deltarec
