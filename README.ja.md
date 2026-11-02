# deltarec — PCM (WAV/FLAC) → 1bit DSD (DSF/DFF) オフラインコンバータ

[English README](README.md)

deltarec は、WAV / FLAC を DSD64・DSD128・DSD256（・DSD512）の 1bit ストリームに変換し、
DSF または DFF ファイルとして書き出すコマンドラインツールです。
依存ライブラリなし（単一ヘッダの dr_wav / dr_flac を同梱）、C++17、CMake で
Windows (Visual Studio 2022/2026)・macOS (Xcode / Apple clang)・Linux (gcc / clang) でビルドできます。

## 考え方：「PCM を DSD レコーダーで録り直す」

DSD レコーダーの ADC の中身は ΔΣ（デルタシグマ）変調器です。アナログ波形を
2.8224 MHz（DSD64）で 1bit に量子化し、量子化ノイズをノイズシェーピングで
可聴帯域の外へ追いやります。deltarec はこれをまるごと数値計算で行います。

```
PCM ──▶ ポリフェーズ FIR（1段目：急峻）──▶ ポリフェーズ FIR（2段目：緩い）──▶ ΔΣ変調器（1bit）──▶ DSF/DFF
        fs_in → 8×fs_in 付近                 → DSD レート                      ノイズシェーピング
```

入力が「マイクからのアナログ電圧」か「PCM のサンプル列」かの違いだけで、
やっていることは本物の DSD 録音と同じです。D/A → A/D を実際に通す必要はありません。

AK4497 / AK4499 / AK4499EX のように DSD 入力時に内蔵デジタルフィルタと変調器を
バイパスする経路（DSD Direct）を持つ DAC では、PC 側で（チップより遥かに贅沢な）
変調器を走らせた結果だけを渡せるため、PCM 再生とは異なる結果が得られます。

## ビルド

### Windows — Visual Studio 2022 / 2026

いちばん簡単なのはソリューションを開く方法です：

```
vs\deltarec.sln
```

を Visual Studio で開き、`Release | x64` でビルドすると `vs\bin\x64\Release\deltarec.exe`
ができます（ARM64 構成もあります）。プロジェクトはプラットフォームツールセットに
`$(DefaultPlatformToolset)` を指定しているので、VS2026（v145）でも VS2022（v143）でも
再ターゲットなしでそのまま開けます。必要なのは「C++ によるデスクトップ開発」
ワークロードだけです。

CMake 派の人は **フォルダーを開く** でこのディレクトリを開けば `CMakePresets.json` が
認識されます。コマンドラインなら Developer PowerShell で：

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

Apple Silicon / Intel 両対応。追加のライブラリは不要です（pthread のみ）。

## 使い方

```
deltarec [options] input.(wav|flac) [-o output.dsf]

  -o, --output FILE     出力パス（省略時は入力名の拡張子を .dsf / .dff に）
  -r, --rate N          DSD 倍率: 64, 128, 256（既定）, 512
  -b, --base HZ         レートファミリ: 44100（既定）または 48000
  -f, --format FMT      dsf（既定）または dff
      --order N         変調器の次数 2..8（既定 7）
      --hinf X          NTF の帯域外ゲイン（Lee の基準、既定 1.4）
                        大きいほどノイズシェーピングが強く、入力ヘッドルームは減る
  -g, --gain DB         追加ゲイン dB（既定 0: PCM 0 dBFS → 変調度 50 %、SACD の慣例）
      --limit X         変調器入力のハードリミット 0..1（既定 0.6）
      --dither X        量子化器での TPDF ディザ量、0 で無効（既定 0.05）
      --passband HZ     アンチイメージフィルタの通過帯域端（既定 0.4535×fs_in ＝ 44.1 kHz 素材で 20 kHz）
      --attenuation DB  アンチイメージフィルタの阻止域減衰（既定 160）
      --info            設定に対する DSP 設計を表示して終了
  -q, --quiet           進捗表示なし
```

例：

```sh
deltarec album01.flac                       # DSD256 (11.2896 MHz) の .dsf を生成
deltarec album01.flac -r 128                # DSD128
deltarec album01.flac -r 64 -f dff          # DSD64 の .dff
deltarec track.wav -g -3                    # 3 dB 下げて変調器のヘッドルームを確保
deltarec hires96k.flac -b 48000             # 48 kHz 系 DSD（12.288 MHz）を使う
deltarec track.flac --info                  # フィルタ・変調器の設計値を確認
```

### レベルについて

SACD では「0 dB SACD ＝ 変調度 50 %」が慣例で、市販 DSD 音源もおおむねこれに
従っています。deltarec も既定で PCM 0 dBFS を変調度 50 % に対応させるので、
他の DSD 音源と同じ音量感で再生されます。1bit 変調器は変調度 65 % 程度で不安定に
なるため、変調器入力を 60 % でハードリミットし、万一発振状態に入った場合は状態を
リセットして続行します。リミッタに当たった回数とリセット回数は終了時に表示される
ので、警告が出たら `-g -3` などで下げてください（通常のマスターでは出ません）。

## DSP の中身

**アップサンプラ** — Kaiser 窓 sinc のポリフェーズ FIR を 2 段。1 段目は入力レートの
約 8 倍まで（有理比 L/M に対応、48 kHz 系 → 44.1 kHz 系の変換も可）、通過帯域
0.4535×fs_in、阻止域 0.5×fs_in、減衰 160 dB、位相あたり約 228 タップ。2 段目は
DSD レートまでの 2 のべき乗倍で、イメージが遠いので位相あたり 12 タップで済みます。
合計コストは DSD サンプルあたり 20 積和程度です。群遅延は整数サンプルに揃えて
除去しているので、出力は入力とサンプル単位で位相が一致します。

**ΔΣ変調器** — 7 次（既定）、誤差帰還形で NTF を厳密に実現（STF = 1）。NTF の零点は
Gauss–Legendre 節点（帯域内ノイズ電力を最小化する配置）、極は Butterworth 配置で
‖NTF‖∞ = H_inf となるよう二分探索。係数は実行時に `ntf.hpp` が設計するので、
任意の次数・OSR・H_inf に対応します。ハードウェア向けの CIFB/CRFB 積分器連鎖を
使わず倍精度の転置直接形 II で計算しているため係数感度の問題はありません。
量子化器の前に微小 TPDF ディザを入れてアイドルトーンを抑えています（ディザも NTF で
整形されるので帯域内には現れません）。

### 測定値（同梱の `tools/verify_dsf.py` で再現可）

24bit 1 kHz 正弦波 → DSD256（既定設定）を 20 kHz 帯域で評価：

| 入力レベル | 帯域内 SNR | 備考 |
|---|---|---|
| 0 dBFS | 148 dB | 24 bit ソースの量子化ノイズで律速 |
| −6 dBFS | 142 dB | 〃 |
| −60 dBFS | 88 dB | 〃（24 bit の理論値通り） |

変調器自体のノイズ密度は 10 kHz で −188 dBFS/bin（2.7 Hz 幅）、
float32 の複合信号を DSD256 にして 20 kHz でブリックウォールした残差は
元信号比 −170 dB、位相誤差 0.0000°。DSD64（7 次、H_inf 1.4）で帯域内 SNR 127 dB。

## 対応フォーマット

- 入力：WAV（8/16/24/32 bit 整数、32/64 bit float、WAVE_FORMAT_EXTENSIBLE）、FLAC（〜32 bit）。1〜6 ch。
- 出力：DSF（4096 byte ブロック・LSB ファースト）、DFF（DSDIFF 1.5、非圧縮）。
  どちらも 4 GB を超えるファイルに対応。
- メタデータ（タグ）は CLI 版ではコピーしません（ID3v2 タグおよびアルバムアートの引き継ぎに対応した foobar2000 用プラグイン版は [`foo_deltarec`](https://github.com/yasudaz/foo_deltarec) を参照してください）。

## 速度

1 ch あたり 1 スレッド（マルチコア CPU では 1 ch あたり前段・後段の 2 スレッドパイプライン）。本フォークの AVX2・キャッシュタイリング最適化により、DSD256 で実時間の **7〜15 倍程度**（オリジナル `0.1.0` 比で出力結果 100% ビット完全一致のまま約 3.2 倍高速化）で変換できます。

## オリジナル（[Imaha486/deltarec](https://github.com/Imaha486/deltarec)）からの変更点

本リポジトリは [Imaha486/deltarec](https://github.com/Imaha486/deltarec) をフォークし、以下の CPU パフォーマンス最適化を加えたものです（詳細は [CHANGELOG.md](CHANGELOG.md) を参照）：

1. **ポリフェーズ FIR の転置レイアウトと AVX2 ベクトル化 (`src/resampler.hpp`)**
   - 2 のべき乗補間パス（`M == 1`：44.1/48 kHz 系の Stage 1 およびすべての Stage 2）向けに転置係数テーブル `ht_`（`[tapsPerPhase][L]`）を追加し、AVX2 FMA（`_mm256_fmadd_pd`）により 1 入力サンプルのブロードキャストに対して 4 位相分の積和を並列計算。
2. **ΔΣ 変調器の状態更新 SIMD レジスタ常駐化と 2 パス分離 (`src/modulator.hpp`)**
   - 7 次誤差帰還ループ内の状態ベクトル `s_[0..7]` と NTF 係数 `k_[0..7]` を 128-bit XMM レジスタに常駐化（`_mm_add_pd` / `_mm_mul_pd` / `_mm_alignr_epi8`）。
   - 入力リミッタ（`[-limit, +limit]`）と xorshift64* による TPDF ディザ生成・加算を 1-bit 帰還ループの前段パスに分離し、逐次ループ内の分岐と乱数依存チェインを排除。
3. **L1/L2 キャッシュ・タイリング (`src/main.cpp`)**
   - 16,384 フレームのバッチを `ChannelPipe` 内で 512 フレーム単位のサブブロックに分割して処理することで、中間バッファ（Stage 1: `32 KB`、Stage 2: `1 MB`）が L1/L2 キャッシュに常駐するようにし、DRAM へのスピル（ch あたり約 32 MB）を解消。
4. **チャンネル内ロックフリー SPSC パイプラインと常駐スレッドプール (`src/main.cpp`)**
   - 論理コア数に余裕がある環境（`hc >= ch * 2`）では、2 スロットのロックフリーリングバッファによりブロック `N+1` の FIR アップサンプリング＋ディザ生成（`PrepWorker`）とブロック `N` の 1-bit ΔΣ 変調（`ModWorker`）を別コアでオーバーラップ実行。
   - バッチごとの `std::thread` 生成・破棄を廃止し、ファイル変換全体でスレッドプール（`ChannelWorkerPool`）を再利用。
5. **ビット完全一致（Bit-Exact）の保証**
   - `/fp:precise` と IEEE-754 の演算順序を厳密に維持しており、オリジナル `0.1.0` と 1 バイトの狂いもない完全一致の `.dsf` / `.dff` を出力します。

また、本エンジンをベースに foobar2000 上の任意のデコード可能フォーマットから右クリックで DSD 変換・ID3v2 タグ＆カバーアートのコピーを行うプラグイン版を **[yasudaz/foo_deltarec](https://github.com/yasudaz/foo_deltarec)** にて公開しています。

## ライセンスと特許について

本体は MIT License。同梱の `third_party/dr_wav.h`, `dr_flac.h`（David Reid 作）は
public domain / MIT-0 のデュアルライセンスです。

ΔΣ変調（猪瀬・安田、1962）と高次ノイズシェーピング（1980〜90 年代）の基本特許は
失効しています。DSF の仕様はソニーが公開しており、DSDIFF の仕様はフィリップスが
公開しています。本実装は教科書（Schreier & Temes, *Understanding Delta-Sigma Data
Converters*）に基づく一般的な手法のみで構成され、他社製品のコードや係数は
一切使っていません。なお "DSD" および "Direct Stream Digital" はソニーの商標です。
本プロジェクトは説明のためにのみ用いており、製品名には含めていません。
（以上は法的助言ではありません。）

## 権利関係

著作権・特許・商標の調査記録は [docs/IP-REVIEW.ja.md](docs/IP-REVIEW.ja.md) を参照。

## 公開・リリース手順

[docs/RELEASING.ja.md](docs/RELEASING.ja.md) を参照。

## 謝辞

- **[Imaha486](https://github.com/Imaha486)** — オリジナルの [deltarec](https://github.com/Imaha486/deltarec) の作者。明快で美しいアーキテクチャと高精度な NTF・ポリフェーズ FIR 設計を実装し、MIT ライセンスのオープンソースとして公開されたことに深く感謝いたします。
- David Reid — dr_libs
- R. Schreier, G. C. Temes — *Understanding Delta-Sigma Data Converters*
