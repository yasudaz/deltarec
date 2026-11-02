# deltarec — offline PCM (WAV/FLAC) → 1-bit DSD (DSF/DFF) converter

[日本語の README はこちら](README.ja.md)

deltarec converts WAV / FLAC into a DSD64, DSD128, DSD256 (or DSD512) 1-bit stream and
writes it as a DSF or DFF file. No external dependencies (single-header dr_wav / dr_flac
are bundled), C++17, CMake. Builds on Windows (Visual Studio 2022/2026), macOS
(Apple clang) and Linux (gcc / clang).

## The idea: "re-record the PCM with a DSD recorder"

The front end of a DSD recorder is a delta-sigma modulator: it quantises the analog
waveform to 1 bit at 2.8224 MHz (DSD64) and noise-shapes the quantisation error out of
the audio band. deltarec does exactly that, numerically:

```
PCM ──▶ polyphase FIR (stage 1, sharp) ──▶ polyphase FIR (stage 2, relaxed) ──▶ ΔΣ modulator (1 bit) ──▶ DSF/DFF
        fs_in → ~8×fs_in                    → DSD rate                           noise shaping
```

Whether the modulator's input is an analog voltage from a microphone or a stream of PCM
samples makes no difference to the maths; no real D/A → A/D round trip is involved.

DACs such as the AK4497 / AK4499 / AK4499EX have a "DSD Direct" path that bypasses the
chip's own digital filter and modulator. With such a DAC you can run a far more generous
modulator on the PC and hand the chip only the result.

## Building

### Windows — Visual Studio 2022 / 2026

The easiest route is the solution file:

```
vs\deltarec.sln
```

Open it in Visual Studio and build `Release | x64`; the binary lands in
`vs\bin\x64\Release\deltarec.exe` (an ARM64 configuration is included too). The project
uses `$(DefaultPlatformToolset)`, so it opens without retargeting in both VS 2026 (v145)
and VS 2022 (v143). Only the "Desktop development with C++" workload is needed.

If you prefer CMake, **Open Folder** on this directory picks up `CMakePresets.json`.
From a Developer PowerShell:

```powershell
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
# => build\msvc-x64-release\Release\deltarec.exe
```

### macOS / Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# => build/deltarec
```

Works on Apple Silicon and Intel. Nothing beyond pthreads is required.

## Usage

```
deltarec [options] input.(wav|flac) [-o output.dsf]

  -o, --output FILE     output path (default: input name with .dsf/.dff)
  -r, --rate N          DSD rate multiple: 64, 128, 256 (default), 512
  -b, --base HZ         rate family: 44100 (default) or 48000
  -f, --format FMT      dsf (default) or dff
      --order N         modulator order 2..8 (default 7)
      --hinf X          NTF out-of-band gain, Lee's rule (default 1.4;
                        higher = more noise shaping, less input headroom)
  -g, --gain DB         extra gain in dB (default 0: PCM 0 dBFS -> 50% modulation,
                        the SACD convention; use -3 to leave more headroom)
      --limit X         hard limit on modulator input, 0..1 (default 0.6)
      --dither X        TPDF dither at quantiser, 0 disables (default 0.05)
      --passband HZ     anti-image filter passband edge (default 0.4535*fs_in,
                        i.e. 20 kHz for 44.1 kHz material)
      --attenuation DB  anti-image filter stopband attenuation (default 160)
      --info            print the DSP design for these settings and exit
  -q, --quiet           no progress output
```

Examples:

```sh
deltarec album01.flac                       # DSD256 (11.2896 MHz) .dsf
deltarec album01.flac -r 128                # DSD128
deltarec album01.flac -r 64 -f dff          # DSD64 .dff
deltarec track.wav -g -3                    # 3 dB more modulator headroom
deltarec hires96k.flac -b 48000             # 48 kHz-family DSD (12.288 MHz)
deltarec track.flac --info                  # show filter / modulator design
```

### Levels

SACD defines "0 dB SACD = 50 % modulation", and commercial DSD releases follow it.
deltarec maps PCM 0 dBFS to 50 % modulation by default, so converted files play at the
same loudness as other DSD material. A 1-bit modulator becomes unstable around 65 %
modulation, so the modulator input is hard-limited at 60 % and the state is reset if
the loop ever runs away. Limiter hits and resets are reported at the end; if you see a
warning, lower the level with e.g. `-g -3` (ordinary masters never trigger it).

## DSP details

**Upsampler** — two polyphase Kaiser-windowed-sinc FIR stages. Stage 1 goes to roughly
8 × fs_in (rational L/M, so 48 kHz-family sources convert to the 44.1 kHz DSD family too):
passband 0.4535 × fs_in, stopband 0.5 × fs_in, 160 dB attenuation, ~228 taps per phase.
Stage 2 goes to the DSD rate by a power of two; its images are far away, so 12 taps per
phase suffice. Total cost is about 20 multiply-adds per DSD sample. Group delay is forced
to an integer number of samples and removed, so the output is sample-aligned with the input.

**Modulator** — 7th order by default, error-feedback topology realising the NTF exactly
(STF = 1). NTF zeros sit at the Gauss–Legendre nodes (the in-band-noise-minimising
placement); poles are Butterworth, with the radius bisected until ‖NTF‖∞ = H_inf.
Coefficients are designed at run time by `ntf.hpp`, so any order / OSR / H_inf works.
Double-precision transposed direct form II means the coefficient-sensitivity issues that
force CIFB/CRFB integrator chains in hardware do not apply. A small TPDF dither at the
quantiser suppresses idle tones; it is shaped by the NTF like the quantisation error, so
it does not appear in band.

### Measurements (reproducible with `tools/verify_dsf.py`)

24-bit 1 kHz sine → DSD256, default settings, evaluated over 0–20 kHz:

| input level | in-band SNR | note |
|---|---|---|
| 0 dBFS | 148 dB | limited by the 24-bit source's own quantisation noise |
| −6 dBFS | 142 dB | ″ |
| −60 dBFS | 88 dB | ″ (exactly what 24 bits allows) |

The modulator's own noise density is −188 dBFS per 2.7 Hz bin at 10 kHz. A float32
multi-tone programme converted to DSD256 and brick-walled at 20 kHz differs from the
source by −170 dB with 0.0000° phase error. DSD64 (7th order, H_inf 1.4) gives 127 dB
in-band SNR.

## Formats

- Input: WAV (8/16/24/32-bit integer, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE), FLAC (up to 32-bit). 1–6 channels.
- Output: DSF (4096-byte blocks, LSB first), DFF (DSDIFF 1.5, uncompressed). Both handle files > 4 GB.
- Tags / metadata are not copied in the CLI version (see [`foo_deltarec`](https://github.com/yasudaz/foo_deltarec) for foobar2000 integration with full ID3v2 tag & cover art copying).

## Speed

One thread per channel (or two pipelined threads per channel on multi-core CPUs). With the AVX2 and cache-tiling optimizations in this fork, conversion runs at roughly **7–15 × real time** at DSD256 depending on the CPU (~3.2× faster than original `0.1.0` while producing 100% bit-identical output).

## Changes from Upstream ([Imaha486/deltarec](https://github.com/Imaha486/deltarec))

This repository is a fork of [Imaha486/deltarec](https://github.com/Imaha486/deltarec) with CPU performance optimizations (documented in [CHANGELOG.md](CHANGELOG.md)):

1. **AVX2 Transposed Polyphase FIR (`src/resampler.hpp`)**: Added a transposed coefficient layout (`ht_`) that computes 4 polyphase branches simultaneously using AVX2 FMA (`_mm256_fmadd_pd`) when `M == 1` (power-of-2 interpolation in Stage 1 and Stage 2).
2. **SIMD Register-Resident ΔΣ Modulator (`src/modulator.hpp`)**: Kept the 7th-order error-feedback state vector (`s_[0..7]`) and NTF coefficients (`k_[0..7]`) in XMM registers across iterations (`_mm_add_pd` / `_mm_mul_pd` / `_mm_alignr_epi8`), and separated input limiting (`[-limit, +limit]`) and xorshift64* TPDF dither generation into a vectorizable pre-pass prior to the sequential 1-bit feedback loop.
3. **L1/L2 Cache Tiling (`src/main.cpp`)**: Divided 16,384-frame PCM batches into 512-frame sub-blocks inside `ChannelPipe` so intermediate Stage 1 (`32 KB`) and Stage 2 (`1 MB`) buffers stay hot in L1/L2 cache instead of spilling ~32 MB per channel to main memory.
4. **Lock-Free Per-Channel SPSC Pipeline & Persistent Worker Pool (`src/main.cpp`)**: Added a 2-slot lock-free ring buffer (`PrepWorker` → `ModWorker`) per channel to overlap Stage 1/2 FIR upsampling + dither generation of block `N+1` with 1-bit ΔΣ modulation of block `N`, and kept worker threads alive across the entire file.
5. **Bit-Exact Output**: Maintained `/fp:precise` and exact IEEE-754 evaluation order in the recurrence, producing byte-for-byte identical `.dsf` / `.dff` output to upstream `0.1.0`.

A companion foobar2000 x64 component built on this engine is available at **[yasudaz/foo_deltarec](https://github.com/yasudaz/foo_deltarec)**.

## License and patents

The code is MIT licensed. The bundled `third_party/dr_wav.h` and `dr_flac.h` (David Reid)
are dual-licensed public domain / MIT-0.

The fundamental patents on delta-sigma modulation (Inose & Yasuda, 1962) and high-order
noise shaping (1980s–90s) have expired. Sony publishes the DSF specification and Philips
the DSDIFF specification. This implementation uses only textbook techniques (Schreier &
Temes, *Understanding Delta-Sigma Data Converters*) and contains no code or coefficients
from any commercial product. "DSD" and "Direct Stream Digital" are trademarks of Sony,
used here descriptively only and not as part of the project name. (None of this is legal
advice.)

## IP review

See [docs/IP-REVIEW.ja.md](docs/IP-REVIEW.ja.md) (Japanese) and THIRD_PARTY_NOTICES.md.

## Releasing

See [docs/RELEASING.ja.md](docs/RELEASING.ja.md) (Japanese). Tag `vX.Y.Z` and the release workflow builds Windows / macOS / Linux binaries and publishes them.

## Acknowledgements

- **[Imaha486](https://github.com/Imaha486)** — Original author and creator of [deltarec](https://github.com/Imaha486/deltarec). Special thanks for designing and releasing this clean, high-precision ΔΣ modulator and polyphase FIR upsampler under the MIT License.
- David Reid — dr_libs
- R. Schreier, G. C. Temes — *Understanding Delta-Sigma Data Converters*
