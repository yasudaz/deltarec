# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the project uses [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-11-04 (Fork Changes from [Imaha486/deltarec](https://github.com/Imaha486/deltarec))

### Added
- **CUE Sheet Input & Automatic Track Splitting (`src/cuesheet.hpp`, `src/decoder.hpp`, `src/main.cpp`)**:
  - When a `.cue` file is passed as an input argument, automatically parses the CUE sheet (supporting UTF-8 with/without BOM, UTF-16LE/BE, and Windows Shift-JIS CP932 / ANSI encodings, automatically converting non-UTF-8 CUE text to UTF-8), resolves the backing `.flac` / `.wav` audio file, and splits the audio stream into individual tracks with sample-accurate seeking (`INDEX 01` / `INDEX 00` 1/75s CD sectors).
  - Automatically populates each split track's ID3v2.3 metadata (`TITLE`, `ARTIST`, `ALBUM ARTIST`, `ALBUM`, `TRACKNUMBER`, `TOTALTRACKS`, `COMPOSER`, `ISRC`, `CATALOG`, and `REM` fields such as `GENRE`, `DATE`, `DISCNUMBER`, `TOTALDISCS`, `COMMENT`) from the CUE sheet.
  - Automatically embeds `cover.jpg` in the same folder as the `.cue` file (or embedded cover art from the backing audio file) as the `APIC` front cover art frame.
- **Rate-Dependent Default Modulator Order (`src/main.cpp`)** (Backported from [`foo_deltarec`](https://github.com/yasudaz/foo_deltarec)):
  - Automatically sets the default ΔΣ Modulator Order based on the target DSD rate when `--order` is not explicitly specified: `7th order` for **DSD64**, `5th order` for **DSD128**, and `4th order` for **DSD256 / DSD512** (manual selection `2..8` via `--order N` remains available).
  - Added `--hinf` preset names (`standard` = `1.45`, `gentle` = `1.35`, `aggressive` = `1.55`) in addition to numeric values.
- **Source Metadata Tag & Cover Art Embedding (`src/id3v2_builder.hpp`, `src/decoder.hpp`, `src/dsdwriter.hpp`)**:
  - Reads Vorbis comments and embedded front cover pictures (`METADATA_BLOCK_PICTURE`) from FLAC files, as well as RIFF `LIST INFO` subchunks and embedded `id3 ` / `ID3 ` chunks from WAV files.
  - Builds a compliant **ID3v2.3 (UTF-16LE with BOM)** metadata block (`TIT2`, `TPE1`, `TALB`, `TPE2`, `TYER`, `TRCK`, `TPOS`, `TCON`, `TCOM`, `TPE3`, `TPE4`, `TCOP`, `TPUB`, `TSRC`, `TBPM`, `COMM`, `USLT`, custom `TXXX`, and `APIC` front cover art) and embeds it into both `.dsf` and `.dff` output files by default (can be disabled with `--no-tags`).
- **Multi-File Batch Processing (`src/main.cpp`)**:
  - Supports passing multiple input files (`.wav`, `.flac`, `.cue`), wildcard patterns (`*.flac`, `*.wav`, `*.cue`), or directory paths on a single command line.
- **Output Directory & Tag-Based Folder Templates (`-d, --outdir DIR`, `-S, --subfolder PAT`)**:
  - Added `-d, --outdir DIR` (and `-S, --subfolder PAT`) to specify the target output folder.
  - Supports `%tag%` placeholders in the output directory path (e.g., `-d "D:\DSD\[%album artist%] %album%-(%encoder%)"` or `-d "D:\DSD\%artist%\%album%"`), expanding `%artist%` (and `%arthist%`), `%album artist%`, `%album%`, `%title%`, `%date%`, `%genre%`, `%tracknumber%`, `%discnumber%`, `%encoder%` (`DSD64`..`DSD512`), `%format%` (`DSF`/`DFF`), `%filename%`, and any custom tag from the source file or CUE sheet.
- **Automatic Multi-Disc Subfolder Creation (`disc1`, `disc2`, ...)**:
  - When an output folder is specified (`-d` / `--outdir`), automatically detects multi-disc albums (2 or more discs via `TOTALDISCS` / `DISCTOTAL`, `DISCNUMBER` `N/M`, or batch track pre-scanning) and outputs tracks into `disc1`, `disc2`, ... subfolders inside the target directory (can be disabled with `--no-disc-folder`).

## [0.2.0] - 2026-11-04

### Changed / Optimized
- **Up to ~3.2× faster CPU conversion** (100% bit-exact output for identical DSP parameters):
  - **Transposed Polyphase FIR Layout (`src/resampler.hpp`)**: Added `ht_` (`[tapsPerPhase][L]`, padded to a multiple of 4 `double`s) alongside the existing `h_` layout. In the power-of-2 fast path (`M == 1`, Stage 1 for 44.1/48 kHz and all Stage 2 passes), 4 polyphase outputs are computed simultaneously via AVX2 FMA (`_mm256_fmadd_pd`) while broadcasting each input history sample once (`_mm256_set1_pd`).
  - **SIMD Register-Resident Modulator Loop (`src/modulator.hpp`)**: Kept the 7th-order transposed direct-form II state vector (`s_[0..7]`) and feedback coefficients (`k_[0..7]`) inside 128-bit XMM registers (`_mm_loadu_pd` / `_mm_add_pd` / `_mm_mul_pd` / `_mm_alignr_epi8`) throughout the inner loop instead of executing a scalar loop per DSD sample.
  - **Two-Pass Limiter & Pre-Quantization Dither Separation (`src/modulator.hpp`)**: Moved input clamping (`[-limit, +limit]`), `limitHits` counting, and xorshift64* TPDF dither generation into a vectorizable pre-pass (`u + dither`) before the tight sequential 1-bit feedback loop, eliminating RNG dependency chains and branch checks inside the critical recurrence.
  - **L1/L2 Cache Tiling (`src/main.cpp`)**: Split each 16,384-frame PCM batch into 512-frame sub-blocks inside `ChannelPipe` so intermediate Stage 1 (`4,096 × 8 B = 32 KB`) and Stage 2 (`131,072 × 8 B = 1 MB`) buffers stay hot in L1/L2 cache instead of spilling ~32 MB per channel to DRAM.
  - **Per-Channel SPSC Producer-Consumer Pipeline (`src/main.cpp`)**: Added a lock-free 8-slot ring buffer per channel (`PrepWorker` → `ModWorker`), allowing Stage 1/2 FIR upsampling + dither generation of block `N+1` to run in parallel with 1-bit ΔΣ modulation of block `N`.
  - **Persistent Worker Pool (`src/main.cpp`)**: Replaced per-batch `std::thread` creation/destruction with a persistent `ChannelWorkerPool` alive for the entire conversion.
  - **256 KB File Write Buffering (`src/dsdwriter.hpp`)**: Added explicit 256 KB `setvbuf` stdio buffering for `.dsf` / `.dff` output streams.
  - **AVX2 Build Flags (`CMakeLists.txt`, `vs/deltarec.vcxproj`)**: Enabled `/arch:AVX2` on MSVC x64 (`-mavx2 -mfma` on x86_64 GCC/Clang) while preserving `/fp:precise` to guarantee identical IEEE-754 rounding and NTF design results.

## [0.1.0] - 2026-11-01

### Added
- WAV / FLAC (dr_wav / dr_flac) input, 1–6 channels, integer and float PCM.
- DSD64 / DSD128 / DSD256 / DSD512 output in the 44.1 kHz or 48 kHz family.
- DSF and DFF (DSDIFF 1.5) writers with 64-bit sizes (files > 4 GB).
- Two-stage polyphase Kaiser-windowed-sinc upsampler (rational L/M in stage 1, 160 dB stopband, integer group delay removed).
- Run-time designed delta-sigma NTF (orders 2–8, Gauss–Legendre zeros, Butterworth poles bisected to H_inf), error-feedback 1-bit modulator with TPDF dither, input limiter and overload reset.
- SACD level convention (PCM 0 dBFS → 50 % modulation) with `--gain`.
- `--info` design dump, progress / statistics output, per-channel threading.
- CMake (presets for VS "Open Folder", macOS, Linux), Visual Studio 2022/2026 solution in `vs/`, GitHub Actions build and release workflows.
- `tools/verify_dsf.py` measurement script.
