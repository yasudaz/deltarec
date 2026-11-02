// modulator.hpp - 1-bit delta-sigma modulator (software "DSD recorder").
//
// Topology: error-feedback form with an exact rational NTF.
//   y[n] = u[n] + ((N/D) - 1) * e[n]        (strictly causal part)
//   v[n] = sign(y[n] + dither)              (1-bit quantiser)
//   e[n] = v[n] - y[n]                      (quantiser error)
// which gives V = U + NTF * E, i.e. STF = 1 exactly.
// The filter is implemented in transposed direct form II in double
// precision, so coefficient sensitivity (the usual reason hardware uses
// CIFB/CRFB integrator chains) is not a concern here.
//
// Input scale: +-1.0 corresponds to 100% modulation depth, which a 1-bit
// modulator cannot reach.  The converter maps PCM full scale to 0.5
// (the SACD "0 dB SACD = 50% modulation" convention).  A hard limiter on
// the modulator input plus a state reset on overload keep it from locking up.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#define DELTAREC_HAS_AVX2 1
#endif

#include "ntf.hpp"

namespace deltarec {

class DeltaSigmaModulator {
public:
    struct Stats {
        uint64_t samples = 0;
        uint64_t inputClips = 0;   // input hit the hard limiter
        uint64_t resets = 0;       // modulator was reset after overload
        double peakInput = 0.0;    // |u| before limiting, in modulation units
        double peakY = 0.0;        // |y| at the quantiser
    };

    DeltaSigmaModulator() = default;

    // ntf: designed NTF; inputLimit: hard clip of |u|; dither: TPDF amplitude
    // at the quantiser input (0 disables); resetThreshold: |y| above which the
    // modulator is declared unstable and its state zeroed.
    DeltaSigmaModulator(const NtfDesign& ntf, double inputLimit, double dither,
                        double resetThreshold = 8.0, uint32_t seed = 0x5eed1234u)
        : b_(ntf.b), a_(ntf.a), n_(ntf.order), w_(ntf.order + 1, 0.0),
          inputLimit_(inputLimit), dither_(dither), resetThreshold_(resetThreshold),
          rng_(0x9E3779B97F4A7C15ull ^ (uint64_t(seed) * 0xD1342543DE82EF95ull)) {
        if (rng_ == 0) rng_ = 0x9E3779B97F4A7C15ull;
    }

    // Process `count` input samples; appends the 1-bit output packed 8 samples
    // per byte, oldest sample in the LSB (DSF bit order), to `out`.  A partial
    // byte is carried over to the next call; call finish() at the end.
    void process(const double* u, size_t count, std::vector<uint8_t>& out) {
        constexpr size_t kChunk = 512;
        alignas(32) double xBuf[kChunk];
        alignas(32) double dBuf[kChunk];
        size_t pos = 0;
        while (pos < count) {
            const size_t n = std::min(kChunk, count - pos);
            prepare(u + pos, xBuf, dBuf, n);
            modulatePrepared(xBuf, dBuf, n, out);
            pos += n;
        }
    }

    // Stage 3a (data-parallel / feedback-free): apply input limiter, track peak
    // input, and generate TPDF dither for `count` samples.
    void prepare(const double* __restrict u, double* __restrict xOut, double* __restrict dOut, size_t count) {
        const double inputLimit = inputLimit_;
        const double dither = dither_;
        const bool hasDither = dither > 0.0;
        uint64_t rng = rng_;
        double peakInput = stats_.peakInput;
        uint64_t inputClips = stats_.inputClips;

        if (hasDither) {
            for (size_t j = 0; j < count; ++j) {
                double x = u[j];
                double ax = x < 0 ? -x : x;
                if (ax > peakInput) peakInput = ax;
                if (ax > inputLimit) {
                    x = x < 0 ? -inputLimit : inputLimit;
                    ++inputClips;
                }
                xOut[j] = x;
                double u1 = nextUniform(rng);
                double u2 = nextUniform(rng);
                dOut[j] = dither * (u1 - u2);
            }
        } else {
            for (size_t j = 0; j < count; ++j) {
                double x = u[j];
                double ax = x < 0 ? -x : x;
                if (ax > peakInput) peakInput = ax;
                if (ax > inputLimit) {
                    x = x < 0 ? -inputLimit : inputLimit;
                    ++inputClips;
                }
                xOut[j] = x;
                dOut[j] = 0.0;
            }
        }

        rng_ = rng;
        stats_.peakInput = peakInput;
        stats_.inputClips = inputClips;
    }

    // Stage 3b (feedback loop only): modulate pre-limited `xBuf` and pre-generated
    // `dBuf` into 1-bit packed bytes appended to `out`.
    void modulatePrepared(const double* __restrict xBuf, const double* __restrict dBuf, size_t count, std::vector<uint8_t>& out) {
        switch (n_) {
        case 2: modulateOrder<2>(xBuf, dBuf, count, out); break;
        case 3: modulateOrder<3>(xBuf, dBuf, count, out); break;
        case 4: modulateOrder<4>(xBuf, dBuf, count, out); break;
        case 5: modulateOrder<5>(xBuf, dBuf, count, out); break;
        case 6: modulateOrder<6>(xBuf, dBuf, count, out); break;
        case 7: modulateOrder<7>(xBuf, dBuf, count, out); break;
        case 8: modulateOrder<8>(xBuf, dBuf, count, out); break;
        default: modulateGeneric(xBuf, dBuf, count, out); break;
        }
    }

    // Flush a trailing partial byte (zero padded).  Returns true if one was written.
    bool finish(std::vector<uint8_t>& out) {
        if (accBits_ == 0) return false;
        out.push_back(acc_);
        acc_ = 0; accBits_ = 0;
        return true;
    }

    const Stats& stats() const { return stats_; }

private:
    uint8_t acc_ = 0;
    int accBits_ = 0;
    std::vector<double> b_, a_;
    int n_ = 0;
    std::vector<double> w_;
    double inputLimit_ = 0.6;
    double dither_ = 0.0;
    double resetThreshold_ = 8.0;
    uint64_t rng_ = 1;
    Stats stats_;

    // xorshift64* : fast, more than adequate for dither.  Returns [0, 1).
    static inline double nextUniform(uint64_t& rng) {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        uint64_t r = rng * 0x2545F4914F6CDD1Dull;
        return double(r >> 11) * (1.0 / 9007199254740992.0);
    }

    inline double uniform() { return nextUniform(rng_); }

    template <int Order>
    void modulateOrder(const double* __restrict xBuf, const double* __restrict dBuf, size_t count, std::vector<uint8_t>& out) {
        const size_t oldBytes = out.size();
        const size_t newBytes = (static_cast<size_t>(accBits_) + count) >> 3;
        out.resize(oldBytes + newBytes);
        uint8_t* __restrict outPtr = out.data() + oldBytes;

        double wArr[10] = {0.0};
        double bArr[10] = {0.0};
        double aArr[10] = {0.0};
        for (int k = 1; k <= Order; ++k) {
            wArr[k] = w_[k];
            bArr[k] = b_[k];
            aArr[k] = a_[k];
        }

#if defined(DELTAREC_HAS_AVX2)
        __m128d w12 = _mm_loadu_pd(&wArr[1]);
        __m128d w34 = _mm_loadu_pd(&wArr[3]);
        __m128d w56 = _mm_loadu_pd(&wArr[5]);
        __m128d w78 = _mm_loadu_pd(&wArr[7]);

        const __m128d b12 = _mm_loadu_pd(&bArr[1]);
        const __m128d b34 = _mm_loadu_pd(&bArr[3]);
        const __m128d b56 = _mm_loadu_pd(&bArr[5]);
        const __m128d b78 = _mm_loadu_pd(&bArr[7]);

        const __m128d a12 = _mm_loadu_pd(&aArr[1]);
        const __m128d a34 = _mm_loadu_pd(&aArr[3]);
        const __m128d a56 = _mm_loadu_pd(&aArr[5]);
        const __m128d a78 = _mm_loadu_pd(&aArr[7]);
        const __m128d zero = _mm_setzero_pd();
#else
        double w1 = wArr[1], w2 = wArr[2], w3 = wArr[3], w4 = wArr[4];
        double w5 = wArr[5], w6 = wArr[6], w7 = wArr[7], w8 = wArr[8];
        const double b1 = bArr[1], b2 = bArr[2], b3 = bArr[3], b4 = bArr[4];
        const double b5 = bArr[5], b6 = bArr[6], b7 = bArr[7], b8 = bArr[8];
        const double a1 = aArr[1], a2 = aArr[2], a3 = aArr[3], a4 = aArr[4];
        const double a5 = aArr[5], a6 = aArr[6], a7 = aArr[7], a8 = aArr[8];
#endif

        const double resetThreshold = resetThreshold_;
        double peakY = stats_.peakY;
        uint64_t resets = stats_.resets;
        uint8_t acc = acc_;
        int accBits = accBits_;

        auto stepMod = [&](double x, double d) -> uint8_t {
#if defined(DELTAREC_HAS_AVX2)
            double w1 = _mm_cvtsd_f64(w12);
            __m128d w23 = _mm_shuffle_pd(w12, w34, 1);
            __m128d w45 = _mm_shuffle_pd(w34, w56, 1);
            __m128d w67 = _mm_shuffle_pd(w56, w78, 1);
            __m128d w80 = _mm_unpackhi_pd(w78, zero);

            double y = x + w1;
            double ay = y < 0 ? -y : y;
            if (ay > peakY) peakY = ay;
            if (ay > resetThreshold) {
                w12 = w34 = w56 = w78 = zero;
                w23 = w45 = w67 = w80 = zero;
                w1 = 0.0;
                y = x;
                ++resets;
            }

            double yq = y + d;
            uint8_t bit = (yq >= 0.0) ? 1u : 0u;
            double v = (yq >= 0.0) ? 1.0 : -1.0;
            double e = v - y;
            double outv = e + w1;

            __m128d ve = _mm_set1_pd(e);
            __m128d voutv = _mm_set1_pd(outv);
            w12 = _mm_sub_pd(_mm_add_pd(_mm_mul_pd(b12, ve), w23), _mm_mul_pd(a12, voutv));
            if constexpr (Order >= 3) {
                w34 = _mm_sub_pd(_mm_add_pd(_mm_mul_pd(b34, ve), w45), _mm_mul_pd(a34, voutv));
            }
            if constexpr (Order >= 5) {
                w56 = _mm_sub_pd(_mm_add_pd(_mm_mul_pd(b56, ve), w67), _mm_mul_pd(a56, voutv));
            }
            if constexpr (Order >= 7) {
                w78 = _mm_sub_pd(_mm_add_pd(_mm_mul_pd(b78, ve), w80), _mm_mul_pd(a78, voutv));
            }
            return bit;
#else
            double y = x + w1;
            double ay = y < 0 ? -y : y;
            if (ay > peakY) peakY = ay;
            if (ay > resetThreshold) {
                w1 = w2 = w3 = w4 = w5 = w6 = w7 = w8 = 0.0;
                y = x;
                ++resets;
            }
            double yq = y + d;
            uint8_t bit = (yq >= 0.0) ? 1u : 0u;
            double v = (yq >= 0.0) ? 1.0 : -1.0;
            double e = v - y;
            double outv = e + w1;
            if constexpr (Order == 2) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e - a2 * outv;
            } else if constexpr (Order == 3) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e - a3 * outv;
            } else if constexpr (Order == 4) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e + w4 - a3 * outv;
                w4 = b4 * e - a4 * outv;
            } else if constexpr (Order == 5) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e + w4 - a3 * outv;
                w4 = b4 * e + w5 - a4 * outv;
                w5 = b5 * e - a5 * outv;
            } else if constexpr (Order == 6) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e + w4 - a3 * outv;
                w4 = b4 * e + w5 - a4 * outv;
                w5 = b5 * e + w6 - a5 * outv;
                w6 = b6 * e - a6 * outv;
            } else if constexpr (Order == 7) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e + w4 - a3 * outv;
                w4 = b4 * e + w5 - a4 * outv;
                w5 = b5 * e + w6 - a5 * outv;
                w6 = b6 * e + w7 - a6 * outv;
                w7 = b7 * e - a7 * outv;
            } else if constexpr (Order == 8) {
                w1 = b1 * e + w2 - a1 * outv;
                w2 = b2 * e + w3 - a2 * outv;
                w3 = b3 * e + w4 - a3 * outv;
                w4 = b4 * e + w5 - a4 * outv;
                w5 = b5 * e + w6 - a5 * outv;
                w6 = b6 * e + w7 - a6 * outv;
                w7 = b7 * e + w8 - a7 * outv;
                w8 = b8 * e - a8 * outv;
            }
            return bit;
#endif
        };

        size_t j = 0;
        while (accBits != 0 && j < count) {
            uint8_t bit = stepMod(xBuf[j], dBuf[j]);
            ++j;
            acc |= uint8_t(bit << accBits);
            if (++accBits == 8) {
                *outPtr++ = acc;
                acc = 0;
                accBits = 0;
            }
        }
        const size_t n8 = j + ((count - j) & ~size_t(7));
        for (; j < n8; j += 8) {
            uint8_t b0 = stepMod(xBuf[j + 0], dBuf[j + 0]);
            uint8_t b1_bit = stepMod(xBuf[j + 1], dBuf[j + 1]);
            uint8_t b2_bit = stepMod(xBuf[j + 2], dBuf[j + 2]);
            uint8_t b3_bit = stepMod(xBuf[j + 3], dBuf[j + 3]);
            uint8_t b4_bit = stepMod(xBuf[j + 4], dBuf[j + 4]);
            uint8_t b5_bit = stepMod(xBuf[j + 5], dBuf[j + 5]);
            uint8_t b6_bit = stepMod(xBuf[j + 6], dBuf[j + 6]);
            uint8_t b7_bit = stepMod(xBuf[j + 7], dBuf[j + 7]);
            *outPtr++ = uint8_t(b0 | (b1_bit << 1) | (b2_bit << 2) | (b3_bit << 3) |
                                (b4_bit << 4) | (b5_bit << 5) | (b6_bit << 6) | (b7_bit << 7));
        }
        for (; j < count; ++j) {
            uint8_t bit = stepMod(xBuf[j], dBuf[j]);
            acc |= uint8_t(bit << accBits);
            ++accBits;
        }

#if defined(DELTAREC_HAS_AVX2)
        _mm_storeu_pd(&wArr[1], w12);
        _mm_storeu_pd(&wArr[3], w34);
        _mm_storeu_pd(&wArr[5], w56);
        _mm_storeu_pd(&wArr[7], w78);
        for (int k = 1; k <= Order; ++k) w_[k] = wArr[k];
#else
        w_[1] = w1;
        if constexpr (Order >= 2) w_[2] = w2;
        if constexpr (Order >= 3) w_[3] = w3;
        if constexpr (Order >= 4) w_[4] = w4;
        if constexpr (Order >= 5) w_[5] = w5;
        if constexpr (Order >= 6) w_[6] = w6;
        if constexpr (Order >= 7) w_[7] = w7;
        if constexpr (Order >= 8) w_[8] = w8;
#endif

        stats_.peakY = peakY;
        stats_.resets = resets;
        stats_.samples += count;
        acc_ = acc;
        accBits_ = accBits;
    }

    void modulateGeneric(const double* __restrict xBuf, const double* __restrict dBuf, size_t count, std::vector<uint8_t>& out) {
        out.reserve(out.size() + count / 8 + 1);
        const int n = n_;
        for (size_t i = 0; i < count; ++i) {
            double x = xBuf[i];
            double y = x + w_[1];
            double ay = y < 0 ? -y : y;
            if (ay > stats_.peakY) stats_.peakY = ay;
            if (ay > resetThreshold_) {
                for (double& s : w_) s = 0.0;
                y = x;
                ++stats_.resets;
            }
            double yq = y + dBuf[i];
            double v = yq >= 0.0 ? 1.0 : -1.0;
            double e = v - y;
            double outv = e + w_[1];
            for (int k = 1; k < n; ++k) w_[k] = b_[k] * e + w_[k + 1] - a_[k] * outv;
            w_[n] = b_[n] * e - a_[n] * outv;

            if (v > 0) acc_ |= uint8_t(1u << accBits_);
            if (++accBits_ == 8) { out.push_back(acc_); acc_ = 0; accBits_ = 0; }
        }
        stats_.samples += count;
    }
};

}  // namespace deltarec
