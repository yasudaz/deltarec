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
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "decoder.hpp"
#include "dsdwriter.hpp"
#include "modulator.hpp"
#include "ntf.hpp"
#include "resampler.hpp"

using namespace deltarec;

namespace {

const char* kVersion = "0.1.0";

struct Options {
    std::string input;
    std::string output;
    int rate = 256;             // DSD64 / 128 / 256 / 512
    uint32_t base = 44100;      // 44100 or 48000 family
    std::string format;         // dsf | dff (default from extension)
    int order = 7;
    double hinf = 1.4;
    double gainDb = 0.0;        // extra gain; 0 dB => PCM FS -> 50% modulation
    double limit = 0.6;         // modulator input hard limit (modulation units)
    double dither = 0.05;       // TPDF dither amplitude at quantiser
    double passband = 0.0;      // Hz, 0 = auto (0.4535 * fs_in)
    double attenuation = 160.0; // dB, anti-image filter stopband
    bool info = false;
    bool quiet = false;
};

void usage() {
    std::fprintf(stderr,
        "deltarec %s - PCM (WAV/FLAC) -> DSD (DSF/DFF) converter\n"
        "\n"
        "usage: deltarec [options] input.(wav|flac) [-o output.dsf]\n"
        "\n"
        "  -o, --output FILE     output path (default: input name with .dsf/.dff)\n"
        "  -r, --rate N          DSD rate multiple: 64, 128, 256 (default), 512\n"
        "  -b, --base HZ         rate family: 44100 (default) or 48000\n"
        "  -f, --format FMT      dsf (default) or dff\n"
        "      --order N         modulator order 2..8 (default 7)\n"
        "      --hinf X          NTF out-of-band gain, Lee's rule (default 1.4;\n"
        "                        higher = more noise shaping, less input headroom)\n"
        "  -g, --gain DB         extra gain in dB (default 0: PCM 0 dBFS -> 50%% modulation,\n"
        "                        the SACD convention; use -3 to leave more headroom)\n"
        "      --limit X         hard limit on modulator input, 0..1 (default 0.6)\n"
        "      --dither X        TPDF dither at quantiser, 0 disables (default 0.05)\n"
        "      --passband HZ     anti-image filter passband edge (default 0.4535*fs_in,\n"
        "                        i.e. 20 kHz for 44.1 kHz material)\n"
        "      --attenuation DB  anti-image filter stopband attenuation (default 160)\n"
        "      --info            print the DSP design for these settings and exit\n"
        "  -q, --quiet           no progress output\n"
        "  -h, --help            this text\n"
        "  -V, --version         print version\n",
        kVersion);
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else if (a == "-V" || a == "--version") { std::printf("deltarec %s\n", kVersion); std::exit(0); }
        else if (a == "-o" || a == "--output") o.output = need("-o");
        else if (a == "-r" || a == "--rate") o.rate = std::atoi(need("-r"));
        else if (a == "-b" || a == "--base") o.base = static_cast<uint32_t>(std::atoi(need("-b")));
        else if (a == "-f" || a == "--format") o.format = need("-f");
        else if (a == "--order") o.order = std::atoi(need("--order"));
        else if (a == "--hinf") o.hinf = std::atof(need("--hinf"));
        else if (a == "-g" || a == "--gain") o.gainDb = std::atof(need("-g"));
        else if (a == "--limit") o.limit = std::atof(need("--limit"));
        else if (a == "--dither") o.dither = std::atof(need("--dither"));
        else if (a == "--passband") o.passband = std::atof(need("--passband"));
        else if (a == "--attenuation") o.attenuation = std::atof(need("--attenuation"));
        else if (a == "--info") o.info = true;
        else if (a == "-q" || a == "--quiet") o.quiet = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
        else if (o.input.empty()) o.input = a;
        else { std::fprintf(stderr, "unexpected argument %s\n", a.c_str()); return false; }
    }
    if (o.input.empty()) return false;
    if (!(o.rate == 64 || o.rate == 128 || o.rate == 256 || o.rate == 512)) { std::fprintf(stderr, "--rate must be 64, 128, 256 or 512\n"); return false; }
    if (!(o.base == 44100 || o.base == 48000)) { std::fprintf(stderr, "--base must be 44100 or 48000\n"); return false; }
    if (o.order < 2 || o.order > 8) { std::fprintf(stderr, "--order must be 2..8\n"); return false; }
    if (o.limit <= 0.0 || o.limit >= 1.0) { std::fprintf(stderr, "--limit must be in (0,1)\n"); return false; }
    if (o.format.empty()) {
        std::string ext = o.output.size() >= 4 ? o.output.substr(o.output.size() - 4) : "";
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        o.format = (ext == ".dff") ? "dff" : "dsf";
    }
    if (o.format != "dsf" && o.format != "dff") { std::fprintf(stderr, "--format must be dsf or dff\n"); return false; }
    if (o.output.empty()) {
        size_t dot = o.input.find_last_of('.');
        size_t slash = o.input.find_last_of("/\\");
        std::string stem = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? o.input.substr(0, dot) : o.input;
        o.output = stem + "." + o.format;
    }
    return true;
}

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
    Options o;
    if (!parseArgs(argc, argv, o)) { usage(); return 2; }

    try {
        PcmDecoder dec(o.input);
        const uint32_t ch = dec.channels();
        const uint32_t fsIn = dec.sampleRate();
        if (ch == 0 || ch > 6) throw std::runtime_error("1..6 channels supported");
        Plan plan = makePlan(o, fsIn);

        if (!o.quiet || o.info) {
            std::fprintf(stderr, "deltarec %s\n", kVersion);
            std::fprintf(stderr, "  source     : %s, %u-bit, %s\n", o.input.c_str(), dec.bitsPerSample(), dec.formatName());
            printPlan(plan, o, ch);
        }
        if (o.info) return 0;

        const double gain = 0.5 * std::pow(10.0, o.gainDb / 20.0);

        std::vector<std::unique_ptr<ChannelPipe>> pipes(ch);
        for (uint32_t c = 0; c < ch; ++c) {
            pipes[c] = std::make_unique<ChannelPipe>();
            pipes[c]->s1.init(plan.s1);
            pipes[c]->twoStage = plan.L2 > 1;
            if (pipes[c]->twoStage) pipes[c]->s2.init(plan.s2);
            pipes[c]->mod = DeltaSigmaModulator(plan.ntf, o.limit, o.dither, 8.0, 0x5eed1234u + c * 7919u);
        }

        DsdWriter writer(o.output, o.format == "dff" ? DsdWriter::Format::DFF : DsdWriter::Format::DSF, ch, plan.fsDsd);
        ChannelWorkerPool pool(pipes);

        const size_t blockFrames = 16384;
        std::vector<double> inter;
        std::vector<std::vector<double>> chan(ch);
        std::vector<std::vector<uint8_t>> outBits(ch);
        uint64_t done = 0;
        const uint64_t total = dec.totalFrames();
        auto t0 = std::chrono::steady_clock::now();
        int lastPct = -1;
        bool eof = false;

        while (!eof) {
            size_t got = dec.read(blockFrames, inter);
            eof = got < blockFrames;
            for (uint32_t c = 0; c < ch; ++c) {
                chan[c].resize(got);
                double* __restrict dst = chan[c].data();
                const double* __restrict src = inter.data() + c;
                for (size_t i = 0; i < got; ++i) dst[i] = src[i * ch] * gain;
            }
            pool.runAll(chan, got, eof);
            for (uint32_t c = 0; c < ch; ++c) outBits[c].swap(pipes[c]->bits);
            writer.write(outBits);
            done += got;
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
        writer.finish(pipes[0]->mod.stats().samples);
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
            if (clips) std::fprintf(stderr, "  WARNING    : %llu samples hit the limiter - consider --gain -3\n", static_cast<unsigned long long>(clips));
            if (resets) std::fprintf(stderr, "  WARNING    : modulator reset %llu times (overload)\n", static_cast<unsigned long long>(resets));
            std::fprintf(stderr, "  wrote      : %s\n", o.output.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
