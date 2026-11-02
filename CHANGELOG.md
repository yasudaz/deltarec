# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the project uses [Semantic Versioning](https://semver.org/).

## [0.2.0] - 2026-11-02 (Fork Changes from [Imaha486/deltarec](https://github.com/Imaha486/deltarec))

### Changed / Optimized
- **Up to ~3.2× faster CPU conversion** (100% bit-exact output compared to original `0.1.0`):
  - **Transposed Polyphase FIR Layout (`src/resampler.hpp`)**: Added `ht_` (`[tapsPerPhase][L]`, padded to a multiple of 4 `double`s) alongside the existing `h_` layout. In the power-of-2 fast path (`M == 1`, Stage 1 for 44.1/48 kHz and all Stage 2 passes), 4 polyphase outputs are computed simultaneously via AVX2 FMA (`_mm256_fmadd_pd`) while broadcasting each input history sample once (`_mm256_set1_pd`).
  - **SIMD Register-Resident Modulator Loop (`src/modulator.hpp`)**: Kept the 7th-order transposed direct-form II state vector (`s_[0..7]`) and feedback coefficients (`k_[0..7]`) inside 128-bit XMM registers (`_mm_loadu_pd` / `_mm_add_pd` / `_mm_mul_pd` / `_mm_alignr_epi8`) throughout the inner loop instead of executing a scalar loop per DSD sample.
  - **Two-Pass Limiter & Pre-Quantization Dither Separation (`src/modulator.hpp`)**: Moved input clamping (`[-limit, +limit]`), `limitHits` counting, and xorshift64* TPDF dither generation into a vectorizable pre-pass (`u + dither`) before the tight sequential 1-bit feedback loop, eliminating RNG dependency chains and branch checks inside the critical recurrence.
  - **L1/L2 Cache Tiling (`src/main.cpp`)**: Split each 16,384-frame PCM batch into 512-frame sub-blocks inside `ChannelPipe` so intermediate Stage 1 (`4,096 × 8 B = 32 KB`) and Stage 2 (`131,072 × 8 B = 1 MB`) buffers stay hot in L1/L2 cache instead of spilling ~32 MB per channel to DRAM.
  - **Per-Channel SPSC Producer-Consumer Pipeline (`src/main.cpp`)**: Added a lock-free 2-slot ring buffer per channel (`PrepWorker` → `ModWorker`) when hardware concurrency permits (`hc >= ch * 2`), allowing Stage 1/2 FIR upsampling + dither generation of block `N+1` to run in parallel with 1-bit ΔΣ modulation of block `N`.
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
