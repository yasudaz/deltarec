# deltarec — offline PCM (WAV/FLAC/CUE) → 1-bit DSD (DSF/DFF) converter

[日本語の README はこちら](README.ja.md)

deltarec converts WAV / FLAC (and `.cue` album images) into a DSD64, DSD128, DSD256 (or DSD512)
1-bit stream and writes it as a DSF or DFF file with embedded ID3v2.3 metadata and cover art.
No external dependencies (single-header dr_wav / dr_flac are bundled), C++17, CMake. Builds on
Windows (Visual Studio 2022/2026), macOS (Apple clang) and Linux (gcc / clang).

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
deltarec [options] input1.(wav|flac|cue) [input2.(wav|flac|cue) ...] [-d outdir] [-o output.dsf]

  -d, --outdir DIR      output directory; supports %tag% placeholders such as
                        "D:\DSD\[%album artist%] %album%-(%encoder%)"
                        (creates disc1, disc2... subfolders automatically for multi-disc albums)
  -S, --subfolder PAT   optional subfolder pattern under --outdir (e.g. "[%artist%] %album%")
  -o, --output FILE     output file path (or output directory when converting multiple files)
  -r, --rate N          DSD rate multiple: 64, 128, 256 (default), 512
  -b, --base HZ         rate family: 44100 (default) or 48000
  -f, --format FMT      dsf (default) or dff
      --order N         modulator order 2..8 (default by rate: DSD64=7, DSD128=5, DSD256/512=4)
      --hinf X          NTF out-of-band gain, Lee's rule (default 1.4, or preset:
                        standard=1.45, gentle=1.35, aggressive=1.55)
  -g, --gain DB         extra gain in dB (default 0: PCM 0 dBFS -> 50% modulation,
                        the SACD convention; use -3 to leave more headroom)
      --limit X         hard limit on modulator input, 0..1 (default 0.6)
      --dither X        TPDF dither at quantiser, 0 disables (default 0.05)
      --passband HZ     anti-image filter passband edge (default 0.4535*fs_in,
                        i.e. 20 kHz for 44.1 kHz material)
      --attenuation DB  anti-image filter stopband attenuation (default 160)
      --no-tags         do not copy source metadata tags / cover art into output
      --no-disc-folder  do not create discN subfolders for multi-disc albums
      --info            print the DSP design for these settings and exit
  -q, --quiet           no progress output
```

Examples:

```sh
deltarec album01.flac                       # DSD256 (11.2896 MHz, 4th-order default) .dsf with tags
deltarec album.cue -d "D:\DSD\[%album artist%] %album%-(%encoder%)" # split CUE into tagged tracks
deltarec *.flac -d "D:\DSD\[%album artist%] %album%-(%encoder%)"    # batch convert with tag folder & discN
deltarec disc1/*.flac disc2/*.flac -d "D:\DSD\%artist%\%album%"     # creates disc1/ and disc2/ automatically
deltarec album01.flac -r 128                # DSD128 (5th-order default)
deltarec album01.flac -r 64 -f dff          # DSD64 (7th-order default) .dff
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

**Modulator** — rate-dependent default order (`7th` for DSD64, `5th` for DSD128, `4th` for
DSD256/DSD512, or manually selectable `2..8` via `--order`), error-feedback topology
realising the NTF exactly (STF = 1). NTF zeros sit at the Gauss–Legendre nodes (the
in-band-noise-minimising placement); poles are Butterworth, with the radius bisected until
‖NTF‖∞ = H_inf. Coefficients are designed at run time by `ntf.hpp`, so any order / OSR /
H_inf works. Double-precision transposed direct form II means the coefficient-sensitivity
issues that force CIFB/CRFB integrator chains in hardware do not apply. A small TPDF dither
at the quantiser suppresses idle tones; it is shaped by the NTF like the quantisation error,
so it does not appear in band.

### Measurements (reproducible with `tools/verify_dsf.py`)

24-bit 1 kHz sine → DSD256 (`--order 7`), evaluated over 0–20 kHz:

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

- Input: WAV (8/16/24/32-bit integer, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE), FLAC (up to 32-bit), and CUE sheets (`.cue`, automatically splits tracks and populates tags). 1–6 channels. Multiple files, wildcards, and directory inputs are supported.
- Output: DSF (4096-byte blocks, LSB first), DFF (DSDIFF 1.5, uncompressed). Both handle files > 4 GB.
- Metadata / Tags: Copies Vorbis comments and front cover pictures from FLAC, RIFF LIST INFO / embedded ID3v2 tags from WAV, and album/track metadata from CUE sheets, embedding them as an ID3v2.3 (UTF-16) chunk in both DSF and DFF.

## Speed

One thread per channel (plus a dedicated pipelined prep thread per channel). With the AVX2 and cache-tiling optimizations in this fork, conversion runs at roughly **7–15 × real time** at DSD256 depending on the CPU (~3.2× faster than original `0.1.0` while producing 100% bit-identical DSP output).

## Changes from Upstream ([Imaha486/deltarec](https://github.com/Imaha486/deltarec))

This repository is a fork of [Imaha486/deltarec](https://github.com/Imaha486/deltarec) with feature enhancements backported from [foo_deltarec](https://github.com/yasudaz/foo_deltarec), CUE sheet splitting, and CPU performance optimizations (documented in [CHANGELOG.md](CHANGELOG.md)):

1. **Rate-Dependent Default Modulator Order**: Defaults to `7th` order for DSD64, `5th` order for DSD128, and `4th` order for DSD256 / DSD512 (reduces ultrasonic noise peaking at high OSR while maintaining >140–155 dB in-band SNR).
2. **Source Metadata & Cover Art Embedding (`src/id3v2_builder.hpp`, `src/decoder.hpp`, `src/dsdwriter.hpp`)**: Reads FLAC Vorbis comments / `PICTURE` blocks and WAV `LIST INFO` / `ID3 ` chunks and embeds a compliant ID3v2.3 (UTF-16LE) metadata chunk into output `.dsf` and `.dff` files.
3. **CUE Sheet Automatic Track Splitting & Tagging (`src/cuesheet.hpp`, `src/main.cpp`)**: Passing a `.cue` file automatically converts non-UTF-8 CUE text (such as Japanese Shift-JIS / CP932 or UTF-16) to UTF-8, splits the referenced `.flac` / `.wav` image into individual track files (`01 - Title.dsf`, etc.), and embeds per-track metadata (`TITLE`, `ARTIST`, `ALBUM ARTIST`, `ALBUM`, `TRACKNUMBER`, `TOTALTRACKS`, `COMPOSER`, `ISRC`, `REM GENRE/DATE/DISCNUMBER`) as well as `cover.jpg` in the same folder (or embedded cover art from the audio file).
4. **Multi-File Batch Processing, Output Folder (`-d, --outdir`), and Tag-Based Naming**: Accepts multiple files/wildcards/directories, supports `%tag%` placeholders in the output directory path (e.g. `-d "D:\DSD\[%album artist%] %album%-(%encoder%)"`), and automatically creates `disc1`, `disc2`... subfolders when converting multi-disc albums (2 or more discs).
5. **AVX2 Transposed Polyphase FIR (`src/resampler.hpp`)**: Computes 4 polyphase branches simultaneously using AVX2 FMA (`_mm256_fmadd_pd`) when `M == 1`.
6. **SIMD Register-Resident ΔΣ Modulator (`src/modulator.hpp`)**: Keeps the error-feedback state vector and NTF coefficients in XMM registers across iterations and separates input limiting and xorshift64* TPDF dither generation into a vectorizable pre-pass.
7. **L1/L2 Cache Tiling & Lock-Free Per-Channel SPSC Pipeline (`src/main.cpp`)**: Processes 16,384-frame batches in 512-frame cache-resident sub-blocks and overlaps Stage 1/2 FIR upsampling + dither generation with 1-bit ΔΣ modulation via a lock-free ring buffer and persistent worker pool.

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
