# 音MAD用サンプラー 設計書 v5 (仮称: `Sampler81800`)

VST3 / CLAP 対応のワンショット・サンプラープラグイン。
D&Dでサンプルを読み込み、ピッチオフセット・ポルタメント（カーブ調整可）・
**ピッチシフトアルゴリズムの選択**を主軸にする。

> **v5 での変更点** — 設計レビュー指摘の反映（実装前の穴・矛盾の解消）
> - **【Critical】エンジンの状態をボイスごとに持つ設計へ確定** (§2.1, §3.5, §3.6, §4.1, §5.3)
>   位相ボコーダの位相配列・WSOLAテンプレート・OLA/FIFO はボイス固有の持続状態であり、
>   単一エンジンインスタンスを全ボイスで共有すると壊れる。**エンジンはボイスごとに実体化**し、
>   読み取り専用の大きなLUT（sinc表・窓・FFTプラン）だけを `EngineResources` として共有する。
>   `IPitchEngine::process()` の署名は変えない（`srcPos` 外部保持のまま）。
> - **【Critical】テールのドレイン量に固定レイテンシ整列バッファを算入** (§3.6, §4.1, §5.5)
>   `getTailSamples()` だけでは末尾 `(FIXED_LATENCY − intrinsicLatency)` が欠ける。
> - **【Critical】`pitchRatio[i]` のサンプル精度契約を明文化** (§4.1, §4.3, §4.4, §5.3)
>   サンプル精度を守れるのは Varispeed のみ。フレーム系はhop粒度に丸める、と契約に明記。
> - **ポリグライドを「即時発音＋グループ内オリジン再マッチング」へ** (§3.4, §8.1)
>   貪欲・逐次は交差し得る（証明した最適性を継承しない）。発音遅延ゼロを保ったまま最適化する。
> - **長さテストの基準を統一** (§7 Phase3, §8.1) — 原音長ではなく `原音長 + getTailSamples()`。
> - **サンプルは原音SRで保持・埋め込み、SR変更時は一度だけ変換** (§3.1, §3.9) — 二重リサンプル回避。
> - **固定レイテンシはREAPER全サブモードを覆う定数** (§5.5) — 実行時のレイテンシ変更を禁止。
> - **Analogポルタメントの `portaTime` 意味を到達時間へ正規化** (§3.3)。
> - **VST2 連携経路を削除** (§5.2) — VST2 SDK 入手不可、かつビルド対象はVST3/CLAPのみ。
> - **FLAC埋め込みのfloatクリップ対策** (§3.9)、**`n==0` スライスの明示** (§2.2)。
> - §9 の不変条件・§10 のリスク表を上記に合わせて更新。
>
> **v4 での変更点** — 実装開始可能な状態まで詰めた版
> - MIDIサンプル精度とサブブロック分割を明記 (§2.3)
> - ボイススチール時のフェード、ボイス終了判定（テール欠け対策）を追加 (§3.5, §3.6)
> - パラメータのスムージング方針を一覧化 (§3.9)
> - フェーズを再編。トリムとポリグライドを適切な位置へ (§7)
> - 不変条件を CLAUDE.md 雛形に集約 (§9)
>
> **v3 での変更点**
> - **ピッチと時間を分離** — `IPitchEngine::process()` に `timeRatio` を追加 (§4.1)
> - 「音程を変えても長さを変えない/任意の長さにする」を `durationMode` として
>   ユーザーに見える一級のパラメータに引き上げ (§3.7, §4.7)
> - **【修正】** WSOLA / 位相ボコーダの解析hop計算式を訂正 (§4.3, §4.4)
>   誤 `hopSynth * timeRatio * pitchRatio` → 正 `hopSynth * timeRatio / pitchRatio`
> - Phase -1 (JSFX試作) を本書から分離。`otomad_proto.jsfx` として独立管理
>
> **v2 での変更点**
> - REAPER のピッチシフタ（élastique 含む）をエンジンの1つとして利用する設計を追加 (§5)
> - ホスト非依存性とフォールバック規約を明文化 (§5.3) ← **最重要**
> - JSFX によるアルゴリズム試作フェーズ (Phase -1) を追加
> - ReaScript コンパニオンツールを付録として追加 (付録A)

---

## 0. 前提と技術選定

### 0.1 決定事項

| 項目 | 選択 | 理由 |
|---|---|---|
| 言語 | C++20 | DSP資産・ホスト側の情報量 |
| フレームワーク | **JUCE 8** | D&D・波形描画・パラメータ管理・ビルドが一式そろう |
| CLAP対応 | **clap-juce-extensions** (free-audio) | JUCE本体はCLAPを内蔵しないが、これで同一コードから両フォーマットを吐ける |
| ビルド | CMake + FetchContent | Projucer非依存。CIに載せやすい |
| テスト | Catch2 (DSPユニット) + pluginval (フォーマット適合) | |

### 0.2 却下した選択肢（記録）

- **nih-plug (Rust)**: CLAPネイティブだが、VST3エクスポートが GPLv3 系の `vst3-sys` 依存になる。C++資産も使えない。
- **iPlug2**: ライセンスは緩くREAPER連携のサンプルも充実しているが、D&D・波形UI周りの整備コストがJUCEより高い。
- **JSFX単体で完結**: `gfx_getdropfile` が JSFX の @gfx に存在せず（ReaScript専用）、D&D 要件を満たせない。
  ファイル指定がファイルスライダー経由（REAPERリソースパス配下のみ）に限定されるため最終形にはできない。
  → アルゴリズムの試聴・比較用のプロトタイプは別途 JSFX で用意し、本体とは切り離して管理する。

### 0.3 ライセンス確認事項（実装前に必ず確認）

- **JUCE 8**: AGPLv3 または商用ライセンス。無償枠の売上上限・スプラッシュ要否は公式で最新を確認。
  AGPLv3 と VST3 SDK の GPLv3 は相互に互換（両者の §13）。オーディオプラグインはネットワーク越しの
  相互作用がないため、AGPLの追加義務（ネットワーク条項）は実質的に発火しない。
- **VST3 SDK**: GPLv3 または Steinberg商用ライセンス。
- **CLAP**: MIT。制約なし。
- **VST2**: Steinberg が新規ライセンス配布を終了しており、SDK を合法に入手できない。
  **本プロジェクトは VST2 をビルドせず、REAPER連携もVST2経路を持たない** (§5.2)。
- **zplane élastique**: SDKライセンスは販売数ベースのロイヤリティで個別交渉。
  **本プロジェクトでは SDK を一切同梱しない。** REAPER がホストとして提供するものを
  API経由で呼ぶだけなので、élastique のコードを配布することにはならない (§5参照)。
- 外部ストレッチライブラリ:
  - `signalsmith-stretch`: MIT → **推奨**
  - `SoundTouch`: LGPL-2.1 → 動的リンクなら可
  - `Rubber Band`: GPL / 商用 → GPL汚染を許容する場合のみ

---

## 1. 機能要件

### 1.1 必須 (MVP)

1. **D&D読み込み**: wav / aiff / flac / mp3 / ogg。プラグインウィンドウにドロップ。
2. **ピッチオフセット**: -48 〜 +48 半音 + セント微調整 (-100 〜 +100)。
3. **ポルタメント**: 時間 (0〜5000ms)、カーブ形状の調整、モード (Off / Legato / Always)。
4. **ピッチシフトアルゴリズム選択**: §4・§5 の複数方式をランタイム切替。
5. **長さ制御 (`durationMode`)**: 音程を変えても長さを変えないモードを用意する。
   Natural / Sync（テンポ同期）/ Manual（倍率直指定）の3種 (§4.7)。
6. MIDIノートで再生。ルートキー設定。ADSRエンベロープ。
7. Mono / Poly 切替（ポルタメントはMonoで真価を発揮）。

### 1.2 あると強い (音MAD特化)

- **フォルマントシフト**: ピッチと独立して ±12半音。
- サンプル開始/終了位置トリム、ループ (Off / Forward / PingPong)、リバース再生。
- **サンプルのプロジェクト埋め込み**: FLAC圧縮してステートに埋める。
- ベロシティ→ゲイン感度、ピッチベンドレンジ。
- 波形表示 + 再生位置カーソル。

### 1.3 非対応 (スコープ外)

マルチサンプル/キーマップ、内蔵エフェクト、MPE、モジュレーションマトリクス、外部サンプルブラウザ。

---

## 2. アーキテクチャ

```
┌─────────────────────────────────────────────────────┐
│ PluginEditor (Message Thread)                        │
│  DropZone / WaveformView / CurveEditor / Knobs       │
└────────────┬────────────────────────────────────────┘
             │ APVTS (atomic) + SampleSlot (lock-free swap)
┌────────────▼────────────────────────────────────────┐
│ PluginProcessor (Audio Thread)                       │
│  ┌──────────────┐                                    │
│  │ VoiceManager │─ Voice[N]                          │
│  └──────────────┘     ├─ PortamentoGenerator         │
│                       ├─ SourceReader (loop/rev/trim)│
│                       ├─ IPitchEngine[6] (ボイス所有) │
│                       └─ AdsrEnvelope                │
│  PitchEngineHost ── EngineResources (読み取り専用共有)│
└─────────────────────────────────────────────────────┘
             ▲                          ▲
   SampleLoader (BG Thread)      host::ReaperApi
                                 (init時に一度だけ取得)
```

### 2.1 スレッド境界のルール

- **オーディオスレッドで禁止**: `new` / `delete` / `malloc` / ロック / ファイルI/O / ログ出力 / `std::string`操作。
- サンプル差し替えは `std::shared_ptr<const SampleBuffer>` を
  **バックグラウンドで構築 → アトミック交換 → 旧データはGCスレッドで解放**。
  （**Phase 1 実装は簡素化**: 旧バッファはメッセージスレッド側の graveyard に積んで
  プラグイン生存中は保持する。RT安全は満たすが差し替えを繰り返すとメモリが積むため、
  **Phase 5 で GCスレッド化**して古いものから解放する。）
- **エンジンの実体はボイスごとに持つ。** 位相ボコーダの位相アキュムレータ、WSOLAの合成テンプレート、
  OLAオーバーラップバッファ、出力FIFO は**ボイス固有の持続状態**であり、共有できない。
  各 `Voice` が全エンジン型のインスタンスを1つずつ保持し、`prepareToPlay` で最大サイズ分を
  確保しきる。アルゴリズム切替は**ボイス内のアクティブポインタ差し替えのみ**にする。
- **大きな読み取り専用リソースだけを共有する。** sinc LUT・窓関数テーブル・FFTプラン(twiddle)・
  FFT作業用スクラッチは `EngineResources`（`PitchEngineHost` が prepare 時に一度だけ構築）に置き、
  各ボイスのエンジンは参照だけ持つ。これで 16ボイス×6エンジン のLUT重複を避けつつ、
  可変状態はボイス固有に保てる。
  （FFTスクラッチはボイスが区間ごとに逐次レンダリングされる＝同時実行されないため共有で安全。
  ただし1回の `process()` 内で完結する用途に限る。）
- **REAPER API 関数のうち、プロジェクト状態を変更するものはオーディオスレッドから呼ばない。**
  `IReaperPitchShift` オブジェクトの取得はメッセージスレッド、
  DSPメソッド (`GetBuffer`/`BufferDone`/`GetSamples`) の呼び出しのみオーディオスレッドで行う。

> **なぜ (A) ボイス所有 を選んだか（レビューでの判断変更を記録）**
> 当初は「エンジンをステートレスにして `process()` に状態オブジェクトを渡す (B)」も検討したが、
> (B) は `process()` のホットパス署名を全エンジンで変えることになり、§10 の
> 「署名を後から変える＝全エンジン改修」リスクに真っ向から反する。(A) は `process()` の署名を
> 一切変えず、メモリ量も（RTでの再確保を禁じる以上どちらも maxVoices 分を先取りするので）実質同等。
> よって (A) を採用する。

### 2.2 MIDIタイミングとサブブロック分割

JUCE の `processBlock` は MIDI イベントを**サンプルオフセット付き**で受け取る。
ノートオンをブロック先頭にまとめてしまうと最大で1ブロック（数ms）の揺れが出て、
速いフレーズや連打で破綻する。音MADは16分以下の刻みが日常なのでここは妥協しない。

**方針: MIDIイベント位置でブロックを分割し、区間ごとに全ボイスをレンダリングする。**

```cpp
void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midi)
{
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, buffer.getNumSamples(), meta.samplePosition);
        if (t > pos) { renderSlice (buffer, pos, t - pos); pos = t; }
        handleMidiMessage (meta.getMessage());
    }
    const int tail = buffer.getNumSamples() - pos;
    if (tail > 0) renderSlice (buffer, pos, tail);   // 末尾区間。n==0 になりうるのでガード
}
```

**帰結として、エンジンは `n == 1` でも正しく動く必要がある。** また `renderSlice` は `n == 0` で
呼ばれないようにガードする（最終イベントがブロック末尾にあると差分が 0 になる）。
分割区間は1サンプルになりうる。フレーム単位で処理する WSOLA / 位相ボコーダは
「中間ストリームを必要なだけプルする」構造 (§4.1) なので、この要件は自然に満たされる。
逆に「ブロック先頭でフレームを1つ処理する」ような書き方をすると即破綻する。

`prepare()` で確保するバッファは `maxBlockSize` 基準のままでよい（分割は小さくなる方向）。
ポリグライドのグループ判定 (§3.4) はサンプル単位の時刻で行うので、この分割と整合する。

### 2.3 ディレクトリ構成

```
otomad-sampler/
├── CMakeLists.txt
├── CLAUDE.md                  # Claude Code向け作業規約 (§9)
├── docs/DESIGN.md             # 本書
├── scripts/
│   └── otomad_item_generator.lua   # 付録A: ReaScriptコンパニオン
├── src/
│   ├── PluginProcessor.{h,cpp}
│   ├── PluginEditor.{h,cpp}
│   ├── host/
│   │   ├── ReaperApi.{h,cpp}          # REAPER API 取得の唯一の窓口
│   │   └── reaper_sdk/                # reaper_plugin.h 等 (SDKヘッダのみ)
│   ├── core/
│   │   ├── Params.h                   # ParameterID定数 + createLayout()
│   │   ├── SampleBuffer.h
│   │   ├── SampleLoader.{h,cpp}
│   │   ├── SourceReader.{h,cpp}
│   │   ├── PortamentoGenerator.{h,cpp}
│   │   ├── Voice.{h,cpp}
│   │   └── VoiceManager.{h,cpp}
│   ├── pitch/
│   │   ├── IPitchEngine.h
│   │   ├── EngineResources.{h,cpp}    # sinc/窓/FFTプラン等の読み取り専用共有リソース
│   │   ├── PitchEngineHost.{h,cpp}    # 型解決・フォールバック・レイテンシ統一・Resources所有
│   │   ├── VarispeedEngine.{h,cpp}
│   │   ├── WsolaEngine.{h,cpp}
│   │   ├── PhaseVocoderEngine.{h,cpp}
│   │   ├── GranularEngine.{h,cpp}
│   │   ├── ReaperPitchShiftEngine.{h,cpp}
│   │   └── dsp/
│   │       ├── Interpolators.h
│   │       ├── Windows.h
│   │       ├── FftWrapper.h
│   │       └── SpectralEnvelope.h
│   └── gui/
│       ├── DropZone.{h,cpp}
│       ├── WaveformView.{h,cpp}
│       ├── CurveEditor.{h,cpp}
│       └── Theme.{h,cpp}
└── tests/
```

---

## 3. コアコンポーネント設計

### 3.1 `SampleBuffer`

```cpp
struct SampleBuffer {
    std::vector<std::vector<float>> data;      // 再生用。常にホストSRへ変換済み（planar [ch][sample]）
    int          numChannels = 0;
    std::int64_t numSamples  = 0;
    double       sampleRate  = 0.0;            // = ホストSR (dataのSR)
    std::vector<std::vector<float>> original;  // 原音SRのままの生データ（保存・再変換の元）
    double       originalSampleRate = 0.0;
    std::string  name;
    std::vector<std::pair<float,float>> peaks; // GUI表示用ピーク列（data基準, モノミックス）
};
```

> **【v5→実装】格納は `std::vector`（planar）で確定。** 設計初版は `juce::AudioBuffer<float>` を
> 挙げていたが、DSPコア（`SourceReader` / `VarispeedEngine` / `Interpolators`）を **JUCE非依存**にして
> 単体テスト可能にするため planar な `std::vector` に変更した（連続領域なので RT読み出しは安全）。
> ファイル読み込み（`SampleLoader`）だけが JUCE に依存し、デコード結果をここへコピーする。

再生用 `data` はホストSRへリサンプル済みにしておくと、後段のratio計算が `2^(semitone/12)`
だけで済む。原音SRのまま扱うと `ratio *= originalSR / hostSR` が全エンジンに漏れ出すので避ける。

**ただし原音 `original` を必ず保持する。** ホストSRが変わったとき（デバイス切替・別SRでの
オフラインバウンス・別環境でのプロジェクト再読込）、`prepareToPlay` の新SRへは**必ず `original`
から一度だけ変換する**。`data` を再リサンプルすると二重変換になり音質が累積劣化する。
埋め込み保存 (§3.9) も `original` を対象にする。

> **【実装メモ / 過去のバグ】** v0.4.0 まで `prepareToPlay` は新SRを保存するだけで
> **`data` を作り直していなかった**。再生側は data がホストSRである前提で読む（SR補正なし）ため、
> 食い違うとその比率がそのまま音程のズレになる（44.1k の data を 48k で読むと **+147 cent**）。
> 実際に踏んだ経路は2つ:
>
> - `setStateInformation` は `prepareToPlay` より**前**に走る。よってプロジェクトを開き直すと
>   `hostSampleRate` が**既定値 44100 のまま** data が作られ、その後 48k を伝えられても直らない。
>   「44100Hz でないとピッチがズレる」という報告はこれ。
> - REAPER のレンダリング(エンコード)SR がプレビューSRと違う場合も同じ。
>
> 対策は `prepareToPlay` から `rebuildSamplesForSampleRate()` を呼び、
> **SRが食い違うスロットだけ** original から作り直すこと。加えて `replaceSlotBuffer()` でも
> 同じ検査をする（平坦化 → SR変更 → UNDO で、SR変更前のバッファが戻ってくるため）。
> 回帰テストは `tests/test_samplerate.cpp`。

**外部 ffmpeg によるデコード（同梱しない / ユーザー指定）**

JUCE の `AudioFormatManager` が読めるのは wav/aiff/flac/mp3/ogg まで。ユーザーが
設定画面で ffmpeg の実行ファイルを指定すると、mp4 / m4a / webm / mkv / mov / opus なども
読めるようになる（`src/core/FfmpegDecoder.*`）。

- **拡張子で振り分けない。** まず `loadFromMemory` を試し、**失敗したときだけ** ffmpeg へ回す。
  こうすれば対応拡張子の一覧を持たずに済み（増えるたびに更新漏れが起きる）、
  ffmpeg 未設定なら従来と完全に同じ挙動のままになる。
- 中間形式は `pcm_f32le` の wav。劣化とクリップを避けるため float のまま出す。
  **サンプルレートとチャンネル数は ffmpeg 側で変換しない** — ホストSRへの変換は
  `SampleLoader` の仕事で、ここで変えると二重変換になる（規約16）。
- `-nostdin` は必須。端末の無いプラグインプロセスから起動すると stdin 待ちで固まることがある。
- ffmpeg 本体はライセンス構成が配布ビルドによって変わる（GPL/LGPL、非フリーのコーデックを
  有効にしたビルドもある）ため、**バイナリはリポジトリに含めず同梱もしない**。
  パスは `<userAppData>/OtoMadSampler/ffmpeg.txt` に保存する（マシン固有の設定）。
  保存時に `-version` で検証済みなので、起動時の復元では再検証しない（子プロセス起動は遅い）。

### 3.2 `SourceReader`

trim / loop / reverse を吸収し、エンジン側は無限ストリームとして扱えるようにする。

**トリムをエンジンに見せないことが重要。** `SourceReader` が論理位置0をトリム開始点に
マップし、トリム長を超えたら0を返す。こうすれば `sampleStart`/`sampleEnd` の実装は
読み出し口の1箇所で済み、Varispeed から位相ボコーダまで全エンジンに自動的に効く。
逆にエンジンがトリムを知る設計にすると、エンジンを追加するたびに実装漏れが起きる。

各エンジンが保持する再生位置 (`srcPos`, WSOLA の解析位置など) は
**すべてトリム開始点からの相対値**とし、終端判定も `slen` ではなくトリム長と比較する。

**ゼロクロス吸着**: 波形の途中でトリムするとエッジでクリックが出る。
`snapZeroCross` 有効時は、トリム端を ±2ms 以内の立ち上がりゼロクロスへ吸着させる。
これは読み込み済みバッファに対する探索なので、トリム値の変更時（非RT）に一度計算して
サンプル数として保持し、オーディオスレッドでは探索しない。

```cpp
class SourceReader {
public:
    void configure (const SampleBuffer*, int startSample, int endSample,
                    LoopMode, bool reverse);
    float read (int ch, double pos) const noexcept;
    void  readBlock (int ch, double startPos, float* dst, int n) const noexcept;
    bool  isFinished (double pos) const noexcept;
};
```

### 3.3 `PortamentoGenerator`

**ピッチは常に半音（対数）ドメインで補間する。** Hz直線補間は音楽的に破綻する。

```cpp
class PortamentoGenerator {
public:
    enum class Mode  { Off, Legato, Always };
    enum class Shape { Time, Analog };
    void  setSampleRate (double);
    void  noteOn (float targetSemitone, bool legato);
    void  setTarget (float targetSemitone);
    void  setOrigin (float originSemitone);   // グループ内オリジン再割当用 (§3.4)
    void  setTime (float ms);
    void  setCurve (float curve);      // -1(緩→急) .. 0(直線) .. +1(急→緩)
    void  setShape (Shape);
    void  snap();
    float progress() const noexcept;   // 0..1。オリジン再割当を安全に行える閾値判定に使う
    float nextSemitone() noexcept;
    void  process (float* dst, int n) noexcept;
    bool  isGliding() const noexcept;
};
```

**カーブの数式（Timeモード）**

```
p     = clamp(elapsed / totalTime, 0, 1)
k     = pow(4.0, -curve)              // curve∈[-1,1] → k∈[0.25, 4]
s(p)  = pow(p, k)
out   = start + (target - start) * s(p)
```

- `curve = 0` → 直線
- `curve > 0` → 立ち上がりが速く終端が緩い（シンセの標準的なグライド感）
- `curve < 0` → じわっと出て最後に一気（音MAD的なタメ）

カーブエディタから3次ベジェを受け取る場合は、`s(p)` を256点LUT化して線形補間で引く（RTセーフ）。

**Analogモード（指数ラグ）**

`portaTime` は Time モードでは「到達までの総時間」を意味する。Analog でも意味を揃えるため、
`portaTime` を**ほぼ到達（99%）までの時間**として解釈し、時定数 τ を逆算する。

```
tau   = max(portaTimeSec, epsilon) / 6.9   // 6.9 ≈ ln(1000)。5τ〜7τで実質到達
coeff = exp(-1 / (tau * sampleRate))        // prepare時に計算
cur  += (target - cur) * (1 - coeff)
```

厳密には到達しないので `|target - cur| < 0.001` で吸着。カーブノブはこのモードでは無効化。
これにより「同じ `portaTime` 値なら Time/Analog で体感グライド長がほぼ揃う」。

**ノート優先度**: Monoは last-note priority。`legato` は「発音中のボイスがある状態でのnoteOn」。
`Mode::Legato` で非レガートなnoteOnなら `snap()`。

**Poly でもポルタメントを効かせる（ポリグライド）。**
Serum をはじめ主要なシンセが実装している。詳細は §3.4。

### 3.4 ポリグライド — 和音→和音のボイスマッチング

「どの声部がどの声部へ滑るか」を決める問題。

**マッチング規則: 昇順ソートして同順位どうしを対応させる。**
C(E,G,C) → F(F,A,C) なら最低音は最低音へ、中音は中音へ、最高音は最高音へ。
声部が交差しないので和声的に自然に聞こえる。

これは同時に**最適**でもある。1次元上の点集合の最小コストマッチングは、
ソート順のペアリングが最小値を与える（交差するペアを入れ替えると必ずコストが減る）。
新旧で個数が違う場合は、スキップを許した DP（編集距離と同じ形）で最適解が出る。
`O(n*m)`、声部数はたかだか16なので実行コストは無視できる。ハンガリアン法は不要。

**問題は「和音は同時に届かない」こと。** MIDIノートオンは1個ずつ順に来る。

> **【v5 修正】貪欲・逐次の最近傍法は、上の最適性・非交差性を継承しない。**
> 反例（タイなし）: 旧プール `{0, 3}`、新ノートが到着順 `2 → 4`。
> `2` は最近傍 `3`（距離1）を消費し、残った `4` は `0` を取る → `3→2, 0→4`（**交差**, コスト5）。
> ソート最適は `0→2, 3→4`（コスト3）。MIDIが昇順で届く保証はないため、これは実害になる。

**採用方式: 即時発音 ＋ グループ内オリジン再マッチング（発音遅延ゼロ ＋ 最適・非交差）**

1. 各 noteOn は**即座に発音**する（暫定オリジンは未使用プールの最近傍で仮割当）。
   → 発音は1サンプルも遅れない。ここは死守する。
2. 同一グループ（直前 noteOn から `glideGroupMs` 以内）に後続の noteOn が届くたびに、
   **これまでに届いたグループ全体でソート＋DP再マッチングし、各ボイスのグライドオリジンを更新**する
   (`PortamentoGenerator::setOrigin`)。
3. オリジンの再割当は **glide が視覚上ほとんど進んでいない間だけ許可**する
   （`progress() < ε`、通常は和音が1ms以内に届くので実質常に満たす）。
   グライド開始直後の origin 変更は不可聴。

これで「到着順に依存しない最適マッチング」と「発音遅延ゼロ」を両立する。

```cpp
void VoiceManager::noteOn (int note, float vel)
{
    const auto now = sampleCounter;
    if (now - lastNoteOnTime > groupWindowSamples) {
        snapshotGlidePool();     // 現在鳴っているボイスのピッチを控える
        currentGroup.clear();    // 新しい和音グループの開始
    }
    lastNoteOnTime = now;

    const float target = (float) (note - rootKey);
    Voice* v = allocateVoice (note, vel);
    v->startAt (target);                 // ← まず即時発音（オリジン未確定でも音は出す）
    currentGroup.push_back ({ v, target });

    rematchGroupOrigins();               // グループ全体でDP再マッチ → 各vのoriginを更新
}
```

`snapshotGlidePool()` はリリース中でないボイスの**現在の**ピッチを集める
（グライド途中なら途中の値。連続した和音進行でも滑らかにつながる）。
`rematchGroupOrigins()` はプール（旧ノート）と `currentGroup`（新ノート）を昇順ソートし、
スキップ許容DPで最適対応を求め、各ボイスに `setOrigin()` を適用する。
プールの各要素は一度対応させたら消費済みにし、重複割り当てを防ぐ。
対応相手がいない声部はオリジン＝ターゲット（グライドなし）。
`portaMode = Off` のときはプール処理・再マッチをまるごとスキップする。

### 3.5 `VoiceManager` — ボイス割り当て

- **Mono**: 常にボイス0を使い、last-note priority + レガート判定。
- **Poly**: 同一ノートの再発音を最優先で拾い（連打でボイスが積み上がるのを防ぐ）、
  次に空きボイス、どちらも無ければ**最も古いボイスを奪う**。
- ボイス数上限はエンジンごとに変える。位相ボコーダはボイスあたりホップごとに
  fftSize 点の FFT を回すため、他より厳しく制限する (§4.4)。
  `maxVoices` の値はそのままに、内部で `min()` を取る。
- **和音時の自動ゲイン補正はしない。** 和音は実際に大きいのが正しく、
  自動で下げると単音との音量差が不自然になる。ヘッドルームは `gain` でユーザーが取る。
- **各ボイスがエンジンの実体を所有する** (§2.1)。ボイスごとに中間ストリーム用バッファ・
  WSOLA合成テンプレート・位相配列・出力FIFO を持つ。読み取り専用の LUT / 窓 / FFTプランと
  FFTスクラッチだけは `EngineResources` から共有参照する。

**スチール時のクリック対策（忘れがち）**

発音中のボイスを奪うと波形の途中で切ることになり、確実にクリックが出る。

- 空きボイスがある間は当然そちらを使う。`maxVoices` に余裕を持たせるのが第一の対策。
- 奪うしかない場合、**5ms のフェードアウトを掛けてから**新しいノートを開始する。
  `Voice::steal()` でフェードを開始し、新ノートは同じボイスの「予約」として保持、
  フェード完了時に切り替える。発音が5ms遅れる。
- 遅れを避けたいなら、奪う側を別ボイスで立ち上げつつ古い方を短くフェードさせる
  （実質クロスフェード）。ボイス数に余裕があるならこちらが望ましい。

MVP では前者（単純フェード）で十分。後者は Phase 5 の任意項目とする。

### 3.6 `Voice`

```cpp
class Voice {
public:
    void prepare (double sr, int maxBlock, int numCh, EngineResources&);
    void noteOn (int midiNote, float velocity, bool legato);
    void noteOff();
    void setAlgorithm (AlgorithmId);   // ボイス内の自前インスタンスへポインタを差し替え
    void renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int n);
    bool isActive() const noexcept;
};
```

`renderNextBlock` の流れ:

1. `portamento.process(pitchBuf, n)` で半音列を生成
2. `pitchOffsetSemi + cents/100 + pitchBend + (midiNote - rootKey)` を加算
3. `ratioBuf[i] = exp2(pitchBuf[i] / 12.0f)`
4. `timeRatio = resolveTimeRatio()` — §4.7 に従いブロック単位で算出
5. `engine->process(reader, srcPos, out, numCh, n, ratioBuf, timeRatio)`
6. ADSR・ゲイン・パン適用

**ボイスの終了判定 — 素材を読み切った瞬間に止めてはいけない**

長さ保持系エンジンは中間ストリームにテールを抱えている。さらに §5.5 の固定レイテンシ方式では
各エンジンが `(FIXED_LATENCY − 自身のレイテンシ)` の整列遅延バッファを通す。
`srcPos >= trimmedLength` になった時点でボイスを落とすと、**エンジン内部テールと整列バッファの
両方が欠ける**。ワンショットの語尾が切れるので音MADでは即バレる。

> **【v5 修正】ドレイン量に整列バッファ分を必ず足す。**
> `getTailSamples()` だけでは末尾 `(FIXED_LATENCY − intrinsicLatency)` が失われる。

```cpp
int Voice::totalTailSamples() const noexcept
{
    // エンジン内部の残響 + 固定レイテンシ整列バッファ (§5.5) の両方を流し切る
    return engine->getTailSamples()
         + (FIXED_LATENCY - engine->getIntrinsicLatency());
}

bool Voice::isFinished() const noexcept
{
    const bool srcDone = (srcPos >= reader.getTrimmedLength());
    return srcDone
        && drainCounter >= totalTailSamples()
        && envelope.isIdle();
}
```

`srcDone` になったら `drainCounter` を進め始める。
`getTailSamples()` は Varispeed なら 0、WSOLA なら frameSize、位相ボコーダなら fftSize を返す。
素材の範囲外は `SourceReader` が 0 を返すので、ドレイン中は無音が流し込まれ、
テールだけが正しく出力される。Varispeed（intrinsic 0）でも整列バッファ分
（最大 `FIXED_LATENCY`）を流し切るまで落とさない点に注意。

### 3.7 パラメータ一覧 (APVTS)

> **重要**: パラメータの個数・レンジ・選択肢は**ホストによらず常に同一**にすること。
> 理由は §5.3。

| ID | 型 | 範囲 | 既定 | 備考 |
|---|---|---|---|---|
| `pitchSemi` | int | -48..48 | 0 | オフセット |
| `pitchCents` | float | -100..100 | 0 | |
| `rootKey` | int | 0..127 | 60 | 原音のキー |
| `algorithm` | choice | 下記6種で固定 | Varispeed | |
| `interpQuality` | choice | Linear/Hermite/Sinc | Hermite | Varispeed時のみ有効 |
| `reaperSubMode` | int | 0..31 | 0 | REAPERシフタのサブモード。他ホストでは無視 |
| `durationMode` | choice | Natural/Sync/Manual | Natural | §4.7。Varispeed時はNatural固定 |
| `syncLength` | choice | 1/4,1/2,1,2,4拍 | 1拍 | Sync時のみ有効 |
| `stretchAmount` | float | 0.25..4.0 x (skew) | 1.0 | Manual時のみ有効 |
| `cacheFallback` | choice | Varispeed/WSOLA/Phase Vocoder | Varispeed | REAPER Shifter のキャッシュを外した音を鳴らすエンジン。申告レイテンシはこのエンジンの固有遅延になり、キャッシュ再生側は先頭無音で揃える（`Voice::NoteOptions::alignLatency`） |
| `formant` | float | -12..12 st | 0 | 対応エンジンのみ |
| `portaMode` | choice | Off/Legato/Always | Legato | Polyでも有効 (§3.4) |
| `glideGroupMs` | float | 0..100 ms | 30 | 和音グループ判定の間隔 |
| `portaShape` | choice | Time/Analog | Time | |
| `portaTime` | float | 0..5000 ms (skew) | 80 | Time=総時間 / Analog=99%到達時間 |
| `portaCurve` | float | -1..1 | 0 | |
| `attack/decay/sustain/release` | float | | 1/100/1.0/50 | |
| `polyMode` | choice | Mono/Poly | Mono | |
| `maxVoices` | int | 1..16 | 8 | 重いエンジンでは内部でさらに制限 |
| `sampleStart` `sampleEnd` | float | 0..1 | 0 / 1 | 正規化位置 |
| `snapZeroCross` | bool | | true | トリム端を±2ms内のゼロクロスへ吸着 |
| `loopMode` | choice | Off/Fwd/PingPong | Off | |
| `reverse` | bool | | false | |
| `gain` `pan` | float | | 0dB / center | |
| `velSens` | float | 0..1 | 0.5 | |
| `bendRange` | int | 0..48 | 2 | |
| `octave` | int | -4..4 | 0 | ピッチに 12×n 半音を加算 |
| `vibDepth` | float | 0..200 cent | 0 | 0 で無効 |
| `vibRate` | float | 0.1..20 Hz | 5 | |
| `vibDelay` | float | 0..2000 ms | 0 | 発音から効き始めるまで |
| `vibFade` | float | 0..2000 ms | 200 | 最大振幅に達するまで（ADSR の A 相当） |

`algorithm` の選択肢（**この6個から増減させない**）:

```
0: Varispeed        (長さ変化あり)
1: WSOLA            (長さ保持)
2: Phase Vocoder    (長さ保持)
3: Granular         (長さ保持)
4: Stretch Library  (長さ保持、Phase 4で追加、それまでは2にフォールバック)
5: REAPER Shifter   (長さ保持、REAPER上でのみ実動作)
```

### 3.8 パラメータのスムージング

「全部スムージングすればいい」は誤り。かえって不自然になるものがある。

| パラメータ | 扱い | 理由 |
|---|---|---|
| `pitchSemi` / `pitchCents` / ベンド | スムージングしない | `PortamentoGenerator` が半音ドメインで面倒を見る。二重に掛けると効きが鈍る |
| `timeRatio` | 20ms | 急変で WSOLA / 位相ボコーダにクリック |
| `gain` / `pan` | 10ms | |
| `formant` | 20ms | スペクトル包絡の移動が飛ぶとザラつく |
| `sampleStart` / `sampleEnd` | **ノートオン時に確定**。発音中は変えない | 発音中に動かすと再生位置が飛ぶ |
| `algorithm` / `durationMode` / `loopMode` / `interpQuality` | 離散。スムージングしない | 切替はエンジン差し替えと固定レイテンシで吸収 |
| ADSR 時定数 | 次回発音から反映 | 発音中に変えると包絡が破綻する |
| `vibDepth` | 20ms | 自動化で段差が出るとピッチのジッパーノイズになる |
| `vibRate` | スムージングしない | 位相の増分にしか効かず、段差が可聴でない |
| `vibDelay` / `vibFade` | **ノートオン時に確定** | 発音からの経過時間が基準なので、発音中に変えても意味を持たない |

離散パラメータを発音中に変えてもクリックが出ないことは Phase 3 の受け入れ条件で検証する。

#### ビブラート

`VibratoLfo`（`src/core/VibratoLfo.h`, JUCE非依存）が担当する。
発音から `vibDelay` 経過後、`vibFade` をかけて `vibDepth` へ線形に立ち上げ、正弦波でピッチを揺らす。

- 変調量は**半音**で返し、ピッチの半音値に加算してから比へ変換する（規約4）。
- 位相と経過サンプルは**ボイス固有**（規約9）。発音ごとに `reset()` する。
- `reset()` にはエンジンの固定レイテンシを**負値**で渡す。こうすると Delay/Fade の起点が
  ADSR と揃い、高レイテンシのエンジンで「音より先に揺れ始める」現象を防げる。
- フレーム系エンジン（WSOLA / PV / REAPER Shifter）は規約10によりフレーム先頭で
  pitchRatio を固定するため、揺れの解像度はフレームレートに丸められる。滑らかにしたい場合は
  Varispeed か REAPER Shifter の Natural/Manual（キャッシュ再生＝内部Varispeed）を使う。

### 3.9 ステート保存

```xml
<OtoMadState stateVersion="2" uiScalePct="100" editorW="890" editorH="755">
  <APVTS .../>                        <!-- 選択中スロットの設定 -->

  <samples active="0">                <!-- stateVersion 2 以降。複数サンプルの切り替え用 -->
    <sample name="voice.wav" path="C:/..." normGain="1.0" embedded="1"
            format="flac" srcSampleRate="44100" normScale="1.0">
      <APVTS .../>                    <!-- スロットごとの設定 -->
      BASE64...
    </sample>
    ...
  </samples>

  <!-- 旧ビルド互換: 選択中スロットを従来の場所にも書く（新形式を読める版はこちらを無視する） -->
  <sample name="voice.wav" ... >BASE64...</sample>

  <appearance mainColour="ff4bb4f5" bgOpacity="0.25" panelOpacity="1.0">BASE64(PNG)</appearance>
</OtoMadState>
```

- `stateVersion`: 1 = 単一 `<sample>` のみ / 2 = `<samples>` リスト。
  古いビルドで開かれたとき「サンプル無し」と「壊れている」を区別できるようにする。
- **複数サンプル**: 差し替えて聴き比べるための機能。スロットごとに素材・`normGain`・
  APVTS を保持し、切り替えると設定が丸ごと入れ替わる。キャッシュは切替時に作り直す。
  読み込み時の新スロットは「その時点の設定」を引き継ぐ（同条件で比較を始められる）。
- 復元は APVTS に触れない専用経路で積む。読み込みと同じ経路を通すと
  「直前スロットを現在の APVTS で上書き」が走り、復元済みの設定が壊れる。
- スロット一覧はメッセージスレッド専用だが `getStateInformation` は任意スレッドから
  呼ばれうるため、走査は `slotLock` 下のスナップショットで行い、重い FLAC エンコードは
  ロックを離してから行う。FLAC は `SampleBuffer` ポインタをキーにキャッシュする
  （ホストの autosave で毎回エンコードし直さないため）。

- `embedSample` オプション（既定ON）。**原音SR** の PCM を FLACエンコード後にBase64。
  再生用の `data`（ホストSR変換済み）ではなく `original` を埋め込む。ロード時に
  現在のホストSRへ一度だけ変換すれば、SRの異なる環境で開いても二重リサンプルにならない
  （`srcSampleRate` を属性に持たせて変換元SRを明示）。
- **FLACは整数PCM。** float素材で `|x| > 1.0` の場合、そのままでは書き込み時にクリップする。
  エンコード前にピーク正規化し、正規化係数を `normScale` として保存、ロード時に復元する
  （またはピークが1を超える素材は 32bit WAV にフォールバックする）。
- ロード順: 埋め込み → パス → 両方失敗ならGUIに警告表示して無音（**クラッシュさせない**）。

### 3.10 エディタ（ネイティブ / Web の2実装）

エディタは2つあり、CMake の `OTOMAD_WEB_UI` で切り替える（既定 1 = Web）。
**どちらを使ってもパラメータ定義は同一**（規約12）。UI は表示と無効化を変えるだけ。

| | 実装 | 位置づけ |
|---|---|---|
| ネイティブ | `src/PluginEditor.*` | 従来の JUCE Component。WebView2 が無い環境のフォールバックも兼ねる |
| Web | `src/WebEditor.*` + `resources/webui/*` | 既定。HTML/CSS/JS |

- **パラメータ**は `WebSliderRelay` / `WebComboBoxRelay` / `WebToggleButtonRelay` と
  対応する `…ParameterAttachment` で APVTS と双方向バインドする。
- **それ以外**（波形ピーク・D&D のバイト列・鍵盤・REAPER モード一覧・状態表示・外観設定）は
  `withNativeFunction` と `emitEventIfBrowserIsVisible` で受け渡す。
- **アセットは BinaryData に埋め込み**、`ResourceProvider` でローカル配信する。
  外部ネットワークへは一切アクセスしない。配信は許可リスト方式にして、
  将来 `juce_add_binary_data` に足したものが自動的に Web から到達可能にならないようにする。
- JUCE のフロントエンド JS（`juce-index.js` / `check_native_interop.js`）は
  **configure 時に使用中の JUCE からコピー**する。手でベンダリングすると、JUCE のタグを
  上げたとき C++ 側のプロトコルだけが変わって JS が取り残される。
- WebView2 は**プラグインでは `withUserDataFolder` の指定が必須**。既定はホスト実行ファイル
  基準（Program Files 等）で書き込めず生成に失敗し、JUCE は黙って IE バックエンドへ
  フォールバックして真っ白な窓になる。
- WebView2 ランタイムが無い環境では `areOptionsSupported` で判定してネイティブ版へ落とす
  （規約15の精神: 黙って壊れた表示を返さない）。
- **D&D は WebView2 から実パスを取得できない**ため、JS 側でバイト列を読み標準 base64 で
  バックエンドへ渡す（`SampleLoader::loadFromMemory`）。転送中はファイルサイズの5〜6倍の
  メモリを使うので上限（64MB）を設ける。`MemoryBlock::toBase64Encoding` は JUCE 独自形式で
  JS の `btoa()` と互換が無いため、**`juce::Base64` を使うこと**。
- デコードは背景スレッド（`loadPool`）で走るので、`loadSample` の戻り値では成否を返せない。
  失敗は `loadError` イベントで通知する。**`status` イベントに相乗りさせない** —
  status は 10Hz で回るぶんスカラー比較で早期棄却しており、文字列だけ変わった場合を
  取りこぼす。
- **WebView2 はネイティブ子ウィンドウなのでキー入力を全部食う。** Space が DAW まで届かず
  再生/停止できない上、直前に押したボタンにフォーカスが残っていると Space で再クリックされる。
  JS 側の capture フェーズで keydown/keyup 両方を `preventDefault`（button の click は
  **keyup** で合成されるので keydown だけでは足りない）し、ネイティブ側で
  `PostMessage(GetAncestor(hwnd, GA_ROOT), WM_KEYDOWN/UP, VK_SPACE)` としてホスト窓へ投げ直す。
  - **REAPER でもアクションIDを決め打ちしない**（`Main_OnCommand` を使わない）。Space の既定は
    Play/stop (40044) であって Play/pause (40073) ではなく、ユーザーが Space を別アクションへ
    割り当てている場合もある。キーを渡せばホスト側の割り当てがそのまま効く。
  - VST3 には「ホストのトランスポートを操作する」標準手段が無い
    （`AudioPlayHead::canControlTransport()` は JUCE の VST3 ラッパでは常に false。
    実装があるのは iOS Inter-App Audio のみ）。どのみち非REAPERホストではこれしかないので、
    経路は一本にしておく。

---

## 4. 自前ピッチシフトアルゴリズム

### 4.1 共通インターフェース

```cpp
struct PitchEngineContext {
    double sampleRate;
    int    maxBlockSize;
    int    numChannels;
};

class IPitchEngine {
public:
    virtual ~IPitchEngine() = default;
    virtual bool isAvailable() const { return true; }   // ★ 使えない環境では false
    // prepare は非RT。読み取り専用の共有リソース（sinc表・窓・FFTプラン）を受け取り、
    // 可変状態（位相配列・FIFO・テンプレート）はこの実体（ボイス所有）が確保する。
    virtual void prepare (const PitchEngineContext&, EngineResources&) = 0;
    virtual void reset() = 0;                               // RTセーフ
    virtual int  getIntrinsicLatency() const = 0;
    virtual int  getTailSamples()      const = 0;   // 素材が尽きた後のエンジン内部残響長 (§3.6)
    virtual bool preservesDuration()   const = 0;
    virtual bool supportsFormant()     const = 0;
    virtual void setFormantShift (float semitones) {}

    // pitchRatio[i] : 出力サンプルiにおけるピッチ比 (2^(st/12))。
    //   ★ サンプル精度で反映できるのは Varispeed のみ。
    //     フレーム系（WSOLA / Phase Vocoder / Granular）は各解析フレームの
    //     先頭値でサンプリングし、フレーム内は一定として扱う（hop粒度に丸める）。
    //     滑らかなポルタメントを最優先するなら Varispeed を使うよう UI で案内する (§5.3)。
    // timeRatio     : 出力1サンプルあたりソース位置が進む量。ブロック単位で十分
    // srcPos        : 呼び出し側(Voice)が保持し、エンジンが更新する
    //                 → 再生中にアルゴリズムを切り替えても再生位置が保たれる
    virtual void process (SourceReader& src, double& srcPos,
                          float* const* out, int numChannels, int n,
                          const float* pitchRatio, double timeRatio) = 0;
};
```

**★ ピッチと時間の分離**

`pitchRatio` と `timeRatio` を独立に受けるのがこのインターフェースの要。
両者の関係はエンジン種別で異なる:

```
Varispeed:   srcPos += pitchRatio[i]     // 時間はピッチに従属。timeRatio は無視される
長さ保持系:   srcPos += timeRatio         // pitchRatio は内部の周波数倍率にのみ使う
```

`timeRatio = 1.0` で原音と同じ長さ、`0.5` で2倍に伸びる。
長さ保持系のエンジン（WSOLA / Phase Vocoder / Granular）は元々
「時間軸の進み方」と「周波数の倍率」が別パラメータの構造をしている。
従来はこれを `timeRatio = 1/pitchRatio` に固定していたが、
その拘束を外すだけなので実装コストはほぼゼロで、むしろ構造が素直になる。

**★ フレーム内では pitchRatio を一定に保つ（長さドリフト防止）**
WSOLA は解析hop `hopSynth*timeRatio/pitchRatio` にも stage2 リサンプルにも pitchRatio を使う。
フレーム内で pitchRatio が動くと両者が食い違い、長さがドリフトする。
**1解析フレーム内は「フレーム先頭の pitchRatio」で hop もリサンプルも統一する。**

### 4.2 Varispeed（リサンプリング） — 既定

「テープを速く回す」方式。**長さが変わる**。音MADの原音イメージはこれ。

```
srcPos += pitchRatio[i]        // timeRatio は構造上使えない
out[i]  = interpolate(src, srcPos)
```

`preservesDuration() == false`。`durationMode` が Sync / Manual でも Natural として扱い、
UI に注意表示する（§4.7）。
**pitchRatio[i] をサンプル単位でそのまま反映できる唯一のエンジン**なので、
滑らかなポルタメントが要るケースはこれを使う。

| 品質 | 手法 | 特徴 |
|---|---|---|
| Linear | 2点線形 | 軽い。高域が鈍る。荒さが味になる場合も |
| Hermite | 4点3次エルミート | **既定**。コスト対品質が最良 |
| Sinc | 窓付きsinc 16〜32タップ + LUT | 最高品質。アップシフト時のエイリアシング対策込み |

**エイリアシング対策**: `ratio > 1` のとき原音のナイキストを超える成分が折り返す。
Sincモードでは**カットオフを `1/ratio` に縮めた**窓付きsinc（分数遅延フィルタ兼ローパス）を使う。
Linear/Hermiteでは対策しない（それも味として残す）。
LUT: `sincTable[phaseSteps=512][taps=32]`、位相は線形補間。`EngineResources` が prepare時に生成し共有。

### 4.3 WSOLA（時間領域、長さ保持）

「タイムストレッチ → リサンプル」の2段構成。

```
frameSize   = 40ms 相当 (2の冪に丸める)
hopSynth    = frameSize / 4
hopAnalysis = hopSynth * timeRatio / pitchRatio    // ★ pitchRatio で「割る」
searchRange = ±10ms
```

**符号を間違えやすいので導出を残す。**
WSOLA は「タイムストレッチ（中間ストリーム生成）→ リサンプル」の2段。

```
stage1 の時間伸縮率 = hopSynth / hopAnalysis
stage2 で中間ストリームを pitchRatio 倍速で読む → 長さは 1/pitchRatio 倍
総合の長さ倍率 = (hopSynth / hopAnalysis) / pitchRatio
これを 1/timeRatio に等しくしたい → hopAnalysis = hopSynth * timeRatio / pitchRatio
```

検算: `timeRatio = 1.0`（長さ保持）, `pitchRatio = 2.0`（1オクターブ上）
→ `hopAnalysis = hopSynth / 2` → stage1が2倍に伸ばし、stage2の2倍速読みで元の長さに戻る。
ピッチだけ2倍、長さは不変。✓

各フレームで前回の合成末尾と最も相関の高い位置を探索窓内から選び、Hann窓でクロスフェード加算。
**pitchRatio はフレーム先頭値を採り、そのフレームの hop 計算と stage2 リサンプルの両方に使う** (§4.1)。

- 声ネタなら十分実用的。CPU軽い。トランジェントでダブリングが出る。
- 相関計算は `O(searchRange × frameSize)`。SIMD化 or FFTベース相互相関で最適化余地あり。

### 4.4 Phase Vocoder（周波数領域、長さ保持）

```
fftSize  = 2048 or 4096 (パラメータ化)
hopSynth = fftSize / 4  (Hann窓、COLA条件を満たすのは合成側)
hopAna   = hopSynth * timeRatio / pitchRatio   // ★ §4.3 と同じ導出

各ビンkについて:
  Δφ_expected = 2π * hopAna * k / fftSize
  Δφ_actual   = principalArg(φ_cur[k] - φ_prev[k] - Δφ_expected)
  ω_true[k]   = (Δφ_expected + Δφ_actual) / hopAna
  φ_out[k]   += ω_true[k] * hopSynth
```

COLA条件は**合成側の hop で満たす**必要があるので、`hopSynth` を固定して
`hopAna` を可変にする。逆にすると `timeRatio` を変えた瞬間に振幅変調が乗る。

**pitchRatio はフレーム粒度**。周波数倍率はフレーム単位でしか変えられないため、
`pitchRatio[]` はフレーム先頭値でサンプリングする。速いポルタメントは階段状になるので、
滑らかさが要る場合は Varispeed へ誘導する (§4.1, §5.3)。

**位相コヒーレンス対策**: 素直に実装するとフェイジーなにじみが出る。
Laroche & Dolson の identity phase locking を実装し、
**`phaseLock` トグルで無効化もできるようにする**（にじみを味として使いたい需要がある）。

**フォルマント保存**:

```
1. log|X[k]| を実ケプストラムへ (FFT)
2. 低次 (0..~40) のみ残すリフタリング → 包絡 E[k]
3. 励振 R[k] = |X[k]| / E[k]
4. シフト後: |Y[k]| = R_shifted[k] * E_shifted_by_formantParam[k]
```

CPU最重。Polyでは最大4ボイスに制限する。位相配列・OLAバッファ・FIFOは**ボイス固有** (§2.1)。

### 4.5 Granular

```
grainSize   = 20..200ms (パラメータ化)
overlap     = 2..4
window      = Hann or Tukey
grainStart += hopSynth * timeRatio       // ★ グレイン開始位置の進み方 = 長さを決める
grain内の読み出し速度 = pitchRatio        // 音程を決める（グレイン開始時の値で固定）
```

グレイン開始位置を `timeRatio` で進め、各グレインを `pitchRatio` 倍速で再生。
この2つが完全に独立しているのがグラニュラーの構造上の利点で、
**`timeRatio` 導入で一番きれいに恩恵を受けるエンジン**。
`pitchRatio` はグレイン生成時の値でそのグレイン全体を通す（フレーム粒度, §4.1）。
独特のザラつき・揺らぎが音MADの表現として使える。

**【実装メモ】** `src/pitch/GranularEngine.*`（v0.3.0 で実装）。

- **開始位置ジッタは必須**であって「あれば嬉しい」ではない。ジッタ 0 だと粒どうしの位相差が
  `Δ = ω * hop * (1 - pitchRatio)` と規則的に並び、重なり数ぶんの位相が円周上で均等になる比率で
  基音が打ち消される。実測で最大 218〜494 cent 音程が狂った。
- 粒長・重なり・ジッタは実測で決めた（440Hz 正弦を -12..+12 半音、最悪ピッチ誤差 / 振幅のばらつき）:

  | 構成 | 最悪誤差 | 振幅ばらつき |
  |---|---|---|
  | **1024 x2 jit128** | **15 cent** | **0.073** ← 採用 |
  | 2048 x2 jit 64 | 7 cent | 0.107 |
  | 1024 x4 jit128 | 45 cent | 0.275 |
  | 1024 x2 jit  0 | 218 cent | 0.003 |

  重なりを 4 にするとジッタと相まって振幅が荒れる。2 のほうが精度・滑らかさとも良い。
- ジッタは**粒ごとに進める決定的な擬似乱数**（固定シード）。ブロックごとに進めると
  ブロック分割不変性（規約7）が壊れる。
- 初版を「WSOLA から相関探索を抜いた2段構成」で書いたが失敗した。位相を揃えずに OLA で
  時間伸縮すると粒の境界で位相が飛び、基音そのものがズレる（timeRatio=0.5 で 440→407.5Hz）。
  本節の「粒の中でリサンプル」が正しく、WSOLA が相関探索を持つ理由もここにある。

> **設計と実装の乖離（未解消）**: 本節は `grainSize` / `overlap` / `randomize` を
> パラメータとして露出させると書いているが、実装は `EngineResources` の固定値
> （1024 / x2 / 128）。APVTS パラメータは追加していない。
> 露出させるか、設計を「固定値」に改めるかは未判断。

### 4.6 外部ライブラリ（Phase 4、任意）

`signalsmith-stretch`（MIT、ヘッダオンリー）を `StretchLibEngine` として統合。
自作エンジンとの音質比較の基準にもなる。
このライブラリは元々ピッチと時間が独立したAPIなので `timeRatio` をそのまま渡せる。

**【実装メモ】** `src/pitch/StretchLibEngine.*`（v0.3.0 で実装）。FetchContent で取得。

- API は `process(inputs, inputSamples, outputs, outputSamples)`。時間比は N:M で直接渡せるので、
  `timeRatio` をそのまま入力サンプル数に反映すればよい（規約5 を自然に満たす）。
  端数は小数の読み出し位置に持ち越して、平均で出力1サンプルあたり入力が `timeRatio` 進むようにする。
- ピッチは `setTransposeFactor()`。ブロック先頭値で固定する（規約10）。
- **レイテンシが桁違いに大きい。** `presetDefault` は 48kHz で STFT ブロック 5760サンプル(120ms)、
  インターバル 1440。`inputLatency()+outputLatency()` で 5000サンプル超になる。
  `Voice::getReportedLatency` に case 4 を足し忘れると PV の 2048 を報告して発音がズレる。
- **RT安全性**: `presetDefault()` はもちろん、`process()` 内の一時バッファも初回だけ確保する。
  prepare で無音を通してウォームアップし、音声スレッドで確保させない（規約1）。
- **乱数シードを固定する。** 既定コンストラクタは `std::random_device` を引くので、
  同じ入力でも実行ごとに出力が変わり、ブロック分割不変性を検証できない。
- 未使用チャンネルの出力先に入力バッファを使い回さないこと（入出力エイリアシング）。専用の捨て先を持つ。
- テストで長さを測るときは `nonZeroExtent` のしきい値を上げる。ブロックが大きいぶん
  素材が尽きた後の減衰テールが長く、既定の 1e-4 だとテールまで長さに数えてしまう。

### 4.7 長さ制御モード (`durationMode`)

ユーザーは「WSOLAを選びたい」ではなく「長さを固定したい」と考える。
アルゴリズム選択とは別の、独立したパラメータとして露出させる。

| モード | 挙動 | `timeRatio` |
|---|---|---|
| **Natural** | 長さ保持系は原音長のまま、Varispeed は音程で伸縮 | `1.0` (保持系) |
| **Sync** | ホストテンポに同期し、指定拍数にフィットさせる | `原音長 / 目標長` |
| **Manual** | 倍率を直接指定 | `1 / stretchAmount` |

```cpp
double Voice::resolveTimeRatio() const noexcept
{
    if (! engine->preservesDuration())      // Varispeed
        return 1.0;                          // 無視される
    switch (durationMode) {
        case Natural: return 1.0;
        case Manual:  return 1.0 / stretchAmount;
        case Sync: {
            if (! hostBpmValid) return 1.0;              // テンポ不明ならNatural相当
            const double targetSec = syncBeats * 60.0 / hostBpm;
            const double srcSec    = reader.getTrimmedLengthSeconds();
            if (srcSec <= 0.0 || targetSec <= 0.0) return 1.0;
            return juce::jlimit (0.25, 4.0, srcSec / targetSec);
        }
    }
    return 1.0;
}
```

**制約: 「押した鍵盤の長さにフィット」は実装できない**
一番欲しくなるのはこれだが、リアルタイムでは原理的に不可能。
noteOn の時点でノートがいつ離されるか分からず、プラグインにルックアヘッドがないため。
`Sync`（テンポ同期の拍フィット）で代替する。
グリッドに合っている方が音MADでは扱いやすいので、実用上はこちらの方が使い勝手が良い。

**Varispeed との組み合わせ**
`Varispeed + Sync/Manual` は成立しない。ただし §5.3 の規約1により
**パラメータからは消さない**。UI でグレーアウトし、選択されていたら Natural として扱い、
規約3に従って注意表示する。

**`timeRatio` の更新粒度**
ブロック単位で十分（テンポは急変しない）。ただしブロック間で急に変わると
WSOLA/位相ボコーダでクリックが出るため、`juce::SmoothedValue<double>` で
20ms 程度のスムージングをかけてからエンジンへ渡す。

---

## 5. REAPER 連携（élastique を使う正攻法）

### 5.1 方針

zplane の élastique SDK はライセンス費用が個別交渉で、コードの同梱は現実的でない。
一方 **REAPER はホストとして自前のピッチシフタ群（élastique 各モードを含む）を
プラグインに公開している**。これを呼ぶだけなら:

- élastique のコードを自分で配布しない → ライセンス上クリーン
- ユーザーが持っている REAPER の機能を使うだけ
- REAPER のバージョンが上がればモードも自動で増える

### 5.2 `host::ReaperApi` — 取得の唯一の窓口

**REAPER API へのアクセスは必ずこのクラスに閉じ込める。**
他の場所で直接 API ポインタを触ることを禁止する（null 参照でホストごと落ちるため）。
ビルド対象は VST3 と CLAP のみなので、取得経路もこの2つに絞る（VST2 は §0.3 の通り非対応）。

```cpp
// src/host/ReaperApi.h
namespace host {
class ReaperApi {
public:
    // --- attach: 各フォーマットのラッパから一度だけ呼ぶ。失敗しても例外を投げない ---
    void attachFromVST3 (Steinberg::FUnknown* hostContext);
    void attachFromCLAP (const void* clapHost);

    bool isAvailable() const noexcept { return getPitchShiftApi != nullptr; }

    // 失敗時 nullptr。呼び出しはメッセージスレッドから
    reaper::IReaperPitchShift* createPitchShift() const;

    // UI構築用。モード一覧を動的に取得（番号のハードコードは禁止）
    bool enumModes    (int idx, juce::String& nameOut) const;
    bool enumSubModes (int mode, int idx, juce::String& nameOut) const;

private:
    void* (*getFunc)(const char*) = nullptr;
    reaper::IReaperPitchShift* (*getPitchShiftApi)(int) = nullptr;
};
} // namespace host
```

**取得タイミング（JUCE 8）**

| フォーマット | 経路 |
|---|---|
| VST3 | `juce::VST3ClientExtensions::setIHostApplication()` を override → `queryInterface(reaper::IReaperHostApplication::iid, ...)` → `getReaperApi("ReaperGetPitchShiftAPI")` |
| CLAP | `clap_host` から `clap_get_reaper_context()` 相当を取得 |

- JUCE 公式の `ReaperEmbeddedViewPluginDemo` が VST3 の書き方の実例になる（VST2部分は無視）。
- **CLAP経路は clap-juce-extensions が `clap_host*` を露出しているか要調査。**
  露出していなければ CLAP ビルドでは `isAvailable() == false` のままにする
  （機能が1つ減るだけで、破綻はしない）。

**null 安全の絶対規則**

```cpp
// ✗ 絶対にやらない
auto* ps = reaperHost->getReaperApi("ReaperGetPitchShiftAPI");
// ○ 常にこう
if (! reaperApi.isAvailable()) return nullptr;
```

API は動的で、REAPER 上でもバージョンによって関数が存在しないことがある。
つまり null チェックは「他ホスト対応のための特別扱い」ではなく**常に必須**。
プラグインスキャン時にクラッシュするとホストによってはブラックリスト入りして
二度と読み込まれなくなるため、ここは最優先で堅くする。

### 5.3 ★ ホスト非依存性とフォールバック規約

**この3つは違反するとプロジェクト互換が壊れる。CLAUDE.md にも転記すること。**

**規約1: パラメータ定義はホストによって変えない**
`algorithm` の選択肢は全ホストで常に6個。REAPER で `algorithm=5` として保存した
プロジェクトを Live や Cubase で開いたとき、選択肢が5個しかなければ範囲外の値になり、
オートメーションごと壊れる。VST3ホストはパラメータ数やレンジの動的変更を想定していない。

**規約2: 使えないエンジンは代替に落とす。無音にしない**

```cpp
IPitchEngine* Voice::resolveEngine (AlgorithmId id) noexcept
{
    auto* e = engines[(int) id].get();       // ★ ボイス自身が所有する実体 (§2.1)
    if (e != nullptr && e->isAvailable()) {
        host.setFallbackActive (false);
        return e;
    }
    host.setFallbackActive (true);           // UI通知は PitchEngineHost 経由（プラグイン単位）
    return engines[(int) AlgorithmId::PhaseVocoder].get();   // 一番近い自前エンジン
}
```

音は変わるが曲の構造は再生されるので、ユーザーは状況を理解できる。
無音を返すと「壊れている」としか思われない。
`fallbackActive` の集約と UI 表示は `PitchEngineHost`（プラグイン単位）が担うが、
**エンジンの実体はボイスごと**である点に注意（§2.1）。

**規約3: フォールバック中／サンプル精度が出ない場合は UI に明示する**

```
⚠ REAPER Shifter はREAPER上でのみ利用できます
   現在 Phase Vocoder で代替再生中
```

UI 上、利用不可のエンジンは選択肢から消さず**グレーアウトして残す**（規約1のため）。
また、フレーム系エンジン（WSOLA/PV/Granular/REAPER）選択時に速いポルタメントを使うと
ピッチがhop粒度に丸められる旨も、必要に応じて注記する（§4.1）。

### 5.4 `ReaperPitchShiftEngine`

`IReaperPitchShift` はストリーミング型のインターフェース:

```cpp
class IReaperPitchShift {
    virtual void set_srate (double srate) = 0;
    virtual void set_nch (int nch) = 0;
    virtual void set_shift (double shift) = 0;
    virtual void set_formant_shift (double shift) = 0;
    virtual void set_tempo (double tempo) = 0;
    virtual void Reset() = 0;
    virtual ReaSample* GetBuffer (int size) = 0;   // 入力を書き込むバッファを得る
    virtual void BufferDone (int input_filled) = 0;
    virtual void FlushSamples() = 0;
    virtual int  GetSamples (int requested_output, ReaSample* buffer) = 0;
    // ...
};
```

push/pull 型なので `IPitchEngine::process()` に噛ませるには変換層が要る:

```
1. set_shift(ratio) を ブロック先頭で1回だけ設定
2. 出力が n サンプル溜まるまで:
     buf = GetBuffer(chunk); SourceReader から chunk 分書き込む; BufferDone(chunk);
     GetSamples(n - produced, out + produced);
3. 出力が足りなければ 2 を繰り返す（入力枯渇時は FlushSamples）
```

**制約と対処**

| 制約 | 対処 |
|---|---|
| `set_shift` はブロック単位 → ポルタメントがサンプル単位で滑らかにならない | `pitchRatio[]` の中央値を使う。滑らかさ最優先なら Varispeed を使うよう UI で案内 (§5.3) |
| `timeRatio` の指定方法が独特 | このシフタは「入力を食った量 vs 出力した量」の比でストレッチが決まる。`set_tempo(timeRatio)` で指定し、入力供給量を `timeRatio` に応じて調整する。`set_shift` はピッチのみを担当。**要実機検証** |
| レイテンシが可変かつ非公開 | prepare 時にモードごとにプロービング（無音を流して最初の出力が出るまでのサンプル数を実測）してキャッシュ |
| `ReaSample` は double の場合がある | `REAPER_SAMPLE_TYPE` を見て変換層を用意 |
| モード番号がバージョン依存 | `enumModes` / `enumSubModes` で動的にコンボボックス構築。番号を直書きしない |

### 5.5 レイテンシ報告の統一（全エンジン共通）

アルゴリズムによってレイテンシが違う。再生中に `setLatencySamples()` を変えると
ホストによってはグリッチ・再生停止が起きる。**固定レイテンシ方式を採用する。**

```
FIXED_LATENCY = 自前エンジンの最大レイテンシ と REAPER全サブモードの実測レイテンシ を
                すべて覆う、十分大きな定数（例: 8192）。prepareToPlay で一度だけ報告する。
各エンジンは (FIXED_LATENCY - 自身のレイテンシ) 分の遅延バッファを通す
```

- **実行時にレポート値を変えない。** REAPERエンジンはサブモード依存でレイテンシが変わるが、
  それでも `setLatencySamples()` を鳴動中に変えてはいけない（固定レイテンシ方式の目的が消える）。
- prepare 時にプロービングした REAPER 各サブモードのレイテンシが `FIXED_LATENCY` を**超える場合、
  そのサブモードは選択不可（グレーアウト）にする**。実行時にレイテンシを変える方が害が大きい。
  そのため `FIXED_LATENCY` は最初から余裕を持たせる（8192 目安、64bit/96kHz環境も考慮）。
- エンジン切替でもフォールバック発動でもタイミングがズレない。
- テールのドレインはこの整列バッファ分も流し切る必要がある (§3.6)。

### 5.x REAPER 外で élastique を使う（実験機能・既定オフ）

REAPER Shifter は本来 REAPER ホスト上でしか動かない（§5 冒頭）。しかし REAPER 同梱の
`elastique3.dll` を**実行時ロード**すれば、他ホストでもピッチキャッシュのオフライン
レンダにだけ élastique を使える。`src/pitch/ElastiqueDirect.{h,cpp}` がこれを担う。

公式 SDK が無いため、C export と vtable スロットを直接叩く（RE + probe で確定）:

```
CreateInstance_E3(blockSize, channels, sampleRate, mode) -> void*   // mode 3 = Soloist
DestroyInstance_E3(void*)
vtable[1] ProcessData(inst, const float* const* in, int n, const float* const* out) -> int
vtable[5] SetStretchQFactor(inst, float* pfStretch /*in-out*/, float pitch) -> int
vtable[9] Reset(inst)
```

**確定した挙動（probe 実測）**

| 事実 | 影響 |
|---|---|
| `Reset()` は pitch factor を 1.0 に戻す | **必ず `Reset()` → `SetStretchQFactor()` の順**。逆順だと素通し音がキャッシュに焼き付く |
| `ProcessData` は起動プライミング中 `rc=-1`、有効出力で `rc=0` | 5 ブロック（約 5120 サンプル）は捨てる。頭出しは既存のオンセット検出でそのまま吸収 |
| 入力 n サンプル → 出力 n サンプルの 1:1 | **タイムストレッチを表現できない**。`timeRatio != 1.0` は空を返す |
| スレッド安全性は不明（保証なし） | `renderOffline` を `renderLock` で直列化。`load`/`unload` も同じロックで排他 |

**モードの実測結果**（220Hz 正弦・和音を通し、自己相関と Goertzel で測定）

`CreateInstance_E3` の第4引数。DLL の RTTI には Pro / Eff / Eff-mobile / SOLO の4系統が
あるが、SDK が無いのでどの番号がどれかは断定できない。**挙動で判定して命名した。**

| mode | 生成 | 挙動 | 採否 |
|---|---|---|---|
| 0, 1 | 成功 | 3 オクターブ下がる（＝この 1:1 契約では扱えない。`GetFramesNeeded` 系の可変入力が要ると思われる） | 使わない |
| **2** | 成功 | **多声OK**。-39〜+48 半音で音程・レベルとも正確（レベル低下なし） | `Pro` |
| **3** | 成功 | **単声専用**（SOLOIST）。和音だと片方の声部が消える。下方向でレベルが pitch² 程度に落ちる | `Soloist` |
| 4 | 成功 | 音程が壊れる | 使わない |
| 5, 6 | 成功 | mode 2 と同一出力（範囲外はクランプされている模様） | 使わない |

和音 220+330Hz を +7 半音した実測（入力各成分 0.350）:
mode 3 は上声部 494.4Hz が **0.005**（消滅）・330Hz が素通しで残る。mode 2 は 494.4Hz が **0.351**。

**使用可能な半音範囲**（`ElastiqueDirect::usableSemitoneRange`）

- `Pro`（UI表記 "Elastique Pro"）: **-39 〜 +48**。-40 以下は無音になる。
  なお **mode 2 が本当に CElastiqueProV3 かは未確認**。多声で正しく動くことを実測した上での
  表記であって、内部クラスの同定はできていない（SDK が無いため）。
- `Soloist`: **-17 〜 +41**。上は +42 から音程が 25 セント以上ずれ始める。
  下は音程自体は正しいがレベルが落ちるため、**makeup gain の上限 8 倍で救える -17 が実用限界**
  （-24 半音で入力の約 4%、-30 以下でほぼ無音）。
- 範囲外は空を返し、キャッシュを作らず Varispeed 再生になる（規約15）。

> 初版は参考 Rust 実装の `pitch.clamp(0.25, 4.0)` をそのまま持ってきて ±24 に固定し、
> モードも Soloist 決め打ちだった。どちらも**実測に基づかない値**だったので上記に置き換えた。
> 特に Soloist を既定にしていたのは誤り — 単声専用なので、音楽や複数話者の素材では
> 声部が消えていた。既定は `Pro`。

**設計上の位置づけ**

- **既定オフ。** ユーザーが設定画面で DLL パスを指定したときだけ有効。DLL は zplane の
  ライセンス品なので**リポジトリに含めず再配布もしない**。UI に実験的・再配布不可と明記する。
- **オフライン（キャッシュ）経路専用**（Duration = Natural / Manual）。リアルタイム経路では
  使わない。プライミング・FIFO・レイテンシ補償が必要になり別物の複雑さになる。
- モードは Soloist 固定、フォルマントは非対応。REAPER のモード/サブモード番号とは体系が違う。
- 使えない条件（DLL 無し・ストレッチ・±2oct 超）では `renderShift` が `nullptr` を返し、
  キャッシュが埋まらないまま Varispeed 再生になる。**規約15どおり無音にはならない。**
- DLL パスは「このPCに REAPER がどこに入っているか」というマシン固有の事実なので、
  プロジェクト state (`elastiqueDll` 属性) に加え
  `<userAppData>/OtoMadSampler/elastique.txt` にも保存し、新規インスタンスで復元する。

---

## 6. GUI設計

```
┌──────────────────────────────────────────────────────┐
│  OtoMadSampler                              [埋込 ☑] │
├──────────────────────────────────────────────────────┤
│ ┌──────────────────────────────────────────────────┐ │
│ │   ここに音声ファイルをドロップ                     │ │
│ │   ▁▂▃▅▇▅▃▂▁▂▄▆█▆▄▂▁  voice_neta.wav  (2.34s)     │ │
│ │        ├─start────────────end─┤                   │ │
│ └──────────────────────────────────────────────────┘ │
├──────────────┬───────────────────┬───────────────────┤
│ PITCH        │ PORTAMENTO        │ ALGORITHM         │
│  ◯ Semi  +0  │  Mode  [Legato ▾] │  [Varispeed    ▾] │
│  ◯ Cent   0  │  ◯ Time   80ms    │  Interp [Hermite▾]│
│  Root [C3]   │  ┌─────────────┐  │  ◯ Formant  0.0st │
│              │  │  カーブ描画  │  │                   │
│  ADSR ◯◯◯◯  │  └─────────────┘  │  Poly [Mono ▾]    │
│              │  ◯ Curve  0.00    │                   │
├──────────────┴───────────────────┴───────────────────┤
│ DURATION   [Natural ▾] [Sync ▾] [Manual ▾]           │
│   Natural : 音程を変えても長さは変わりません           │
│   Sync    : [1拍 ▾]     Manual : ◯ 1.00x             │
├──────────────────────────────────────────────────────┤
│ ⚠ REAPER Shifter はREAPER上でのみ利用できます          │
│    現在 Phase Vocoder で代替再生中                     │
└──────────────────────────────────────────────────────┘
```

### 6.1 D&D

```cpp
class DropZone : public juce::Component,
                 public juce::FileDragAndDropTarget {
    bool isInterestedInFileDragAndDrop (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter/Exit (...) override;   // ハイライト
};
```

- 複数ドロップ時は先頭のみ採用。対応拡張子は `AudioFormatManager::getWildcardForAllFormats()` から判定。
- `filesDropped` 内で同期読み込みしない。`ThreadPool` にjobを投げ `AsyncUpdater` でGUI通知。
- ホストによっては OS の D&D がプラグインウィンドウに届かない
  → **ドロップ領域のクリックでファイル選択ダイアログを開くフォールバックを必ず実装**。

### 6.2 アルゴリズムのコンボボックス

- 6項目を常に表示。`isAvailable() == false` のものは `setItemEnabled(id, false)` でグレーアウト。
- REAPER Shifter 選択時のみ、サブモードのコンボボックスを `enumSubModes` の結果で動的構築。
  他ホストではサブモードは「(利用不可)」1項目のみ表示し、値は保持だけする。
- `FIXED_LATENCY` を超えるサブモードもグレーアウトする（§5.5）。

### 6.3 Duration セクション

- `durationMode` の3項目は常に表示。**Varispeed 選択時は Sync / Manual をグレーアウト**し、
  「Varispeed では長さが音程に従属します」と注記を出す（規約1・3に従う）。
- Natural 選択時に、そのエンジンが `preservesDuration()` なら
  「音程を変えても長さは変わりません」と明示する。これが本機能の主目的なので
  ユーザーに黙って効かせず、状態を文言で見せる。
- Sync 選択時にホストからテンポが取れない場合は「テンポ不明 — Natural で動作中」と表示。

### 6.4 カーブエディタ / 波形表示

- カーブは MVP では `portaCurve` ノブに連動した描画のみ（読み取り専用）。Phase 4 でベジェ編集。
- 波形は `SampleBuffer::peaks` のみ参照。オーディオデータに直接触らない。
- 再生位置カーソルは `std::atomic<float>` を30fpsタイマーで読む。

---

## 7. 実装フェーズと受け入れ条件

Claude Code にはこの単位でタスクを渡す。**各フェーズの受け入れ条件を満たしてから次へ進む。**
各フェーズ末尾で必ず `ctest` と `pluginval` を通すこと。

### Phase 0: 土台

- [ ] CMake で JUCE 8 を FetchContent 取得、VST3 ビルドが通る
- [ ] REAPER に読み込め、MIDIノートでサイン波が鳴る
- [ ] **MIDIサブブロック分割 (§2.2) をこの時点で入れる**（後から入れると全体に波及する）
- [ ] `pluginval --strictness-level 5` 通過
- [ ] CI (GitHub Actions) で Win/macOS ビルド

### Phase 1: 最小の音MADサンプラー

- [ ] `DropZone` で wav/mp3 を読み込み、原音を保持しつつホストSRへ変換 (§3.1)
- [ ] `SourceReader`（トリム対応。ループ/リバースは後回しでよい）
- [ ] `sampleStart` / `sampleEnd` / `snapZeroCross`
- [ ] `VarispeedEngine`（Linear + Hermite）
- [ ] `pitchSemi` / `pitchCents` / `rootKey` / ADSR / モノフォニック
- [ ] 波形表示（ピーク列 + トリム範囲）
- [ ] **受け入れ条件**:
      (a) 440Hz正弦波を +12半音で再生 → 出力F0が 880Hz ±1cent（自動テスト）
      (b) トリム位置を変えても再生位置が破綻しない
      (c) `snapZeroCross` 有効時、トリム端でクリックが出ない
- [ ] **この時点で音MADが作れる状態**にする

### Phase 2: ボイスとポルタメント

- [ ] `PortamentoGenerator`（Time / Analog、curve ノブ）
- [ ] `VoiceManager`：Mono（last-note priority + レガート）/ Poly（同一ノート優先 → 空き → 最古を奪う）
- [ ] **ポリグライド (§3.4)**：即時発音 + グループ内オリジン再マッチング + 消費済みフラグ
- [ ] ボイススチール時の 5ms フェード (§3.5)
- [ ] ボイス終了判定（テールのドレイン, §3.6。この時点では整列バッファ0でも可）
- [ ] ピッチベンド、カーブ描画UI（読み取り専用）
- [ ] **受け入れ条件**:
      (a) `portaTime=1000ms, curve=0` で C3→C4 をレガート演奏 → 500ms地点が F#3 ±5cent
      (b) C(C,E,G) → F(F,A,C) を Poly + glide で弾き、
          **3声が交差せず昇順どうしで対応**する（マッチング結果を単体テストで検証）
      (c) **ノートオンが降順・混在順で届いても** (b) と同じ対応になる（到着順非依存）
      (d) 同一ノート連打でボイスが増殖しない
      (e) 最大ボイス数を超えて弾いてもクリックが出ない

### Phase 3: エンジン抽象化と長さ制御

- [ ] `IPitchEngine` / `EngineResources` / `PitchEngineHost` へのリファクタ（Phase 1 の Varispeed を移植）
- [ ] **エンジンはボイス所有・状態はボイス固有 (§2.1) を確定**。読み取り専用リソースのみ共有
- [ ] **`timeRatio` と `getTailSamples()`、整列バッファ込みの終了判定 (§3.6) を含む形で確定**
      （後から入れると全エンジン改修になる）
- [ ] `WsolaEngine` / `PhaseVocoderEngine`（位相ロック込み）/ フォルマントシフト
- [ ] `durationMode` の3モード + Varispeed 時のフォールバック (§4.7)
- [ ] 固定レイテンシ方式 (§5.5)
- [ ] パラメータのスムージング (§3.8)
- [ ] **受け入れ条件**:
      (a) 各エンジンで +7半音時、F0が期待値±10cent
      (b) `durationMode=Natural` で +7半音時、**出力の非ゼロ区間長が
          `原音長 + getTailSamples()` の ±1%**（テール込みで測る。§8.1 と統一）
      (c) `timeRatio=0.5` で出力長が原音の2倍（+テール）±1%、かつ F0 が変化しない
      (d) ピッチと長さが互いに干渉しない（+7半音 × 2倍長 の同時指定で両方成立）
      (e) `n == 1` のブロックを連続して渡しても、大きいブロックと出力が一致する
      (f) 再生中にアルゴリズム／durationMode を切り替えてもクリック/クラッシュなし
      (g) 素材末尾がテールごと出力される（内部テール＋整列バッファとも切れない）
      (h) 全ボイス同時発音でも各ボイスの位相状態が独立している（混信しない）

### Phase 4: REAPER 連携

- [ ] `host::ReaperApi`（VST3 経路をまず実装）
- [ ] `ReaperPitchShiftEngine`（プロービングによるレイテンシ実測込み）
- [ ] サブモードの動的列挙 UI
- [ ] フォールバック規約 (§5.3) の実装 + UI 警告表示
- [ ] **受け入れ条件**:
      (a) REAPER 上で élastique 各モードが選べて鳴る
      (b) **同じプロジェクトを REAPER 以外のホストで開いても、
          パラメータがズレず、代替エンジンで音が出て、警告が表示される**
      (c) REAPER API 非対応ホストでのプラグインスキャンでクラッシュしない
      (d) `FIXED_LATENCY` 超過サブモードがグレーアウトされ、実行時にレポート値が変わらない

### Phase 5: 仕上げ

- [ ] `GranularEngine`、Sinc補間
- [ ] ループ / リバース、サンプル埋め込み保存（原音SR, §3.9）、プリセット
- [ ] ベジェカーブエディタ
- [ ] ボイススチールのクロスフェード化（任意）
- [ ] `signalsmith-stretch` 統合（任意）
- [ ] **受け入れ条件**: ホストSRを変えて開き直しても二重リサンプルによる劣化が出ない

### Phase 6: CLAP + 最適化

- [ ] `clap-juce-extensions` 導入、CLAP版ビルド、`clap-validator` 通過
- [ ] CLAP 経路での REAPER API 取得（可能なら）
- [ ] SIMD最適化（Hermite補間、WSOLA の相関計算）
- [ ] `pluginval --strictness-level 10` 通過
- [ ] 各エンジンのCPU使用率とボイス数上限を計測して記録

---

## 8. テスト戦略

### 8.1 DSPユニットテスト (Catch2)

```cpp
TEST_CASE("VarispeedEngine shifts pitch correctly") {
    auto src = makeSineBuffer(440.0, 48000.0, 1.0);
    auto out = renderWithShift(EngineType::Varispeed, src, +12.0f);
    REQUIRE(estimateF0(out, 48000.0) == Approx(880.0).epsilon(0.001));
}
```

- 全エンジン × {-12, -5, 0, +5, +12} 半音のパラメータ化テスト。
- 全エンジン × `timeRatio` {0.5, 1.0, 2.0} で出力長を検証。
- 無音入力で出力が完全無音（NaN/Infなし）であることを全エンジンで確認。
- `SourceReader` の境界（トリム端、終端、ループ折返し、リバース）を単体で検証。
- **`isAvailable() == false` のエンジンを選択したときのフォールバック挙動**をテストする
  （テスト環境では REAPER API は常に取得できないので、これは自然に検証される）。

**ブロック分割不変性（重要）**
§2.2 のサブブロック分割により、エンジンは任意の `n` で呼ばれる。
同じ入力に対し、ブロックサイズを変えても出力が一致することを検証する。
これが通っていれば、フレーム処理系エンジンの状態管理が正しいと言える。

```cpp
TEST_CASE("engine output is independent of block size") {
    auto a = render (engine, src, /*blockSize*/ 512);
    auto b = render (engine, src, /*blockSize*/ 1);
    REQUIRE (maxAbsDiff (a, b) < 1.0e-5f);   // ← C++では指数表記でよい
}
```

**ボイス状態の独立性（§2.1 の確定に対する検証）**
2ボイスを同時発音し、各ボイスを単独発音したときの出力と一致することを確認する。
エンジンをボイスで共有していると位相配列が混信してこのテストが落ちる。

**ポリグライドのマッチング**
DSPを回さずロジックだけ検証できるので、ここは網羅的にやる。
**到着順（昇順・降順・混在）を変えても最終マッチングが同一**であることを必ず含める。

| 旧 | 新 | 期待 |
|---|---|---|
| {60,64,67} | {65,69,72} | 60→65, 64→69, 67→72（昇順対応、交差なし） |
| {60,64,67} | {62,65} | 2声のみグライド。余った旧ノートは無視 |
| {60} | {60,64,67} | 1声だけ 60→60、残り2声はグライドなし |
| {} | {60,64,67} | 全声グライドなし |
| {60,63} | {62,64}（到着順 62,64 と 64,62 の両方） | どちらも 60→62, 63→64（交差しない・到着順非依存） |

- 同じ旧ノートが2つの新ノートに割り当てられないこと（消費済みフラグの検証）。
- グループ判定間隔を超えて弾いた場合、プールが取り直されること。
- グループ内オリジン再割当が `progress() < ε` の間だけ起きること。

**トリムとゼロクロス吸着**
- 吸着後のトリム端が、必ず `SBUF[i] <= 0 && SBUF[i+1] > 0` を満たす位置の直後にあること。
- 探索範囲内にゼロクロスがなければ元の位置を返すこと（無限ループしない）。

**テール**
- 素材長 1.0 秒を長さ保持エンジンで再生 → 出力の非ゼロ区間が
  `1.0秒 + getTailSamples() + (FIXED_LATENCY − intrinsicLatency)` に収まり、
  かつ末尾 10ms が欠けていないこと（§3.6 と一致させる）。

**サンプルレート変更**
- 原音を 44.1kHz、ホストを 48kHz→96kHz と切り替えて再変換したとき、
  48kHz 直変換と 96kHz 直変換の結果が、二重変換ではなく原音基準で一致すること。

### 8.2 RTセーフティ検証

Debugビルドで `processBlock` 内のアロケーションを検出するグローバル `operator new`
フックを仕込む（`assert(!inAudioThread)`）。
同じフックで以下も検出する:

- `std::mutex::lock` の呼び出し（`JUCE_ASSERT_MESSAGE_THREAD` の逆版を自作）
- `host::ReaperApi` のうち、メッセージスレッド限定のメソッドの呼び出し

### 8.3 パフォーマンス計測

Phase 6 で行うが、**Phase 3 の時点で計測の器だけ作っておく**。
後から入れると比較対象の履歴が残らない。

- 各エンジン × ボイス数 1/4/8 で、1秒レンダリングに要する時間を記録
- ボイス所有によりエンジン実体が maxVoices 分あるので、メモリ使用量も記録しておく
- CI で回してリグレッションを検出（閾値は緩めでよい。桁が変わったら気づければ十分）

### 8.4 ホスト検証

- pluginval strictness 10
- REAPER / FL Studio / Ableton / Cubase で実機確認
- **REAPER で保存 → 他ホストで開く → 再度 REAPER で開く**、の往復でパラメータが保たれること
- サンプル差し替えを再生中に連打してクラッシュしないこと

---

## 9. `CLAUDE.md` に書くべき内容（雛形）

```markdown
# OtoMadSampler 開発規約

## ビルド
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo -j
ctest --test-dir build --output-on-failure
pluginval --strictness-level 10 --validate build/.../OtoMadSampler.vst3

## 不変条件 (これを破る変更は必ず事前に相談すること)

### スレッド
1. `processBlock` およびそこから呼ばれる全関数で、
   new/delete/malloc/lock/ファイルI/O/ログ出力/std::string操作を行わない。
   バッファは prepareToPlay で最大サイズ分を確保しきる。
2. REAPER API へのアクセスは `host::ReaperApi` の外で行わない。
   取得結果は常に null チェックしてから使う。REAPER 上でも null になりうる。
   プロジェクト状態を変える API はオーディオスレッドから呼ばない。
3. GUIからオーディオバッファを直接読まない。peaks配列 / atomic のみ参照。

### DSP
4. ピッチは常に半音（対数）ドメインで補間する。Hz直線補間は禁止。
5. **pitchRatio と timeRatio を癒着させない。** 長さ保持系エンジンの内部で
   `timeRatio = 1/pitchRatio` を決め打ちしてはいけない。
   解析hopは `hopSynth * timeRatio / pitchRatio`（掛けるのではなく割る）。
6. **エンジンはトリムを知らない。** trim/loop/reverse は `SourceReader` が吸収し、
   エンジンから見た位置0は常にトリム開始点。終端判定も原音長ではなくトリム長と比較する。
7. **エンジンは n == 1 で呼ばれても正しく動く。** また renderSlice を n == 0 で呼ばない。
   「ブロック先頭でフレームを1つ処理する」構造にしてはいけない。
8. 素材を読み切ってもボイスを即座に落とさない。`getTailSamples()` に
   固定レイテンシ整列バッファ分 `(FIXED_LATENCY − intrinsicLatency)` を足してドレインする。
9. **エンジンの可変状態（位相配列・OLA・FIFO・テンプレート）はボイス固有。**
   単一エンジンインスタンスを全ボイスで共有しない。共有してよいのは読み取り専用の
   `EngineResources`（sinc表・窓・FFTプラン・スクラッチ）だけ。
10. **フレーム系エンジンは pitchRatio をフレーム先頭値で固定**して1フレームを処理する
    （hop計算とリサンプルで同じ値を使う）。サンプル精度が要るなら Varispeed。

### パラメータ
11. パラメータは必ず APVTS 経由。生メンバ変数で状態を持たない。
12. **パラメータの個数・レンジ・選択肢をホストによって変えない。**
    REAPER 専用機能もパラメータ定義上は常に存在させ、UIでグレーアウトするだけにする。
13. トリムは正規化値 (0..1) で保持する。サンプル数で持つとサンプル差し替えで意味が壊れる。
14. スムージングは §3.8 の表に従う。全部に掛けるのは誤り。

### 挙動
15. エンジンが使えない場合は代替エンジンにフォールバックする。**無音を返さない。**
    フォールバック中であることを UI に表示する。
    REAPER Shifter のキャッシュ経路では「鍵ごと」にフォールバックが起きる（範囲外・生成待ちの
    半音だけ Varispeed で鳴る）ので、プラグイン単位の表示だけでは足りない。鍵盤の各鍵に
    範囲外 / 生成待ち を塗り分け、実際に Varispeed で鳴った鍵を一瞬点灯させる
    （`keyCacheState` / `fetchKeyActivity` → WebEditor の `keys` イベント）。
16. サンプルは原音SRで保持・埋め込みする。SR変更時は原音から一度だけ変換し、
    再生用バッファを再リサンプルしない（二重変換禁止）。
17. `setLatencySamples()` を鳴動中に変えない。固定レイテンシ方式を守る。
18. 新しい IPitchEngine を追加したら、tests/ のパラメータ化テストに必ず登録する
    （ピッチ精度・出力長・ブロック分割不変性・ボイス状態独立性の4点）。

## コーディングスタイル
- C++20。JUCEの命名規則に合わせる。
- コメントは日本語可。DSPの数式は式そのものをコメントに残す。
- 1ファイル400行を超えたら分割を検討。
- 「なぜそうしたか」が非自明な箇所には理由をコメントに残す。
  特に符号・順序を間違えやすい式（hop計算、位相アンラップ）は導出ごと書く。

## 設計書
docs/DESIGN.md が唯一の正。実装が設計と乖離したら、
コードを直すか設計書を更新するかを必ず明示して判断を仰ぐ。
```

---

## 10. リスクと対策

| リスク | 影響 | 対策 |
|---|---|---|
| 他ホストで REAPER API を無防備に呼んでクラッシュ | **スキャン時クラッシュ→ブラックリスト入り** | §5.2 の窓口クラスに閉じ込め、常に null チェック |
| ホストによってパラメータ構成を変えてしまう | プロジェクト互換崩壊・オートメーション破損 | 規約1 (§5.3)。選択肢は6個で固定 |
| フォールバック時に無音 | 「壊れている」と判断される | 規約2・3。代替再生 + UI警告 |
| ホストにD&Dが届かない | 主要機能が使えない | クリックでファイルダイアログのフォールバック必須 |
| **単一エンジンを全ボイスで共有** | **位相配列混信で音が壊れる／ブロック分割不変性テスト崩壊** | エンジンはボイス所有、状態はボイス固有 (§2.1, 規約9) |
| **テール判定で整列バッファを無視** | **語尾が最大 FIXED_LATENCY 分欠ける** | ドレイン量に `(FIXED_LATENCY − intrinsic)` を足す (§3.6, 規約8) |
| **pitchRatio をサンプル精度と誤解** | フレーム系で長さドリフト／期待外れ | フレーム先頭値で固定 (§4.1, 規約10)。UIで Varispeed 誘導 |
| ポリグライドの貪欲逐次が交差 | 声部が入れ替わり和声が濁る | 即時発音+グループ内DP再マッチ (§3.4)。到着順非依存テスト |
| 長さテストがテールを勘定しない | 正しいエンジンがテストで落ちる | 基準を `原音長+getTailSamples()+整列` に統一 (§7, §8.1) |
| ホストSR変更で二重リサンプル | 音質が累積劣化 | 原音を保持し原音から一度だけ変換 (§3.1, 規約16) |
| REAPERサブモードでレイテンシが変動 | 実行時レポート変更→グリッチ | FIXED_LATENCY を全サブモード超過分は選択不可に (§5.5) |
| アルゴリズム切替時のレイテンシ変動 | グリッチ・再生停止 | 固定レイテンシ方式 (§5.5) |
| `timeRatio` を後から追加しようとする | 全エンジンの改修が必要になる | Phase 3 のインターフェース確定時に必ず含める |
| `timeRatio` の急変でクリック | 音質不良 | `SmoothedValue` で20msスムージング後にエンジンへ渡す |
| 位相ボコーダで解析側hopをCOLA基準にしてしまう | timeRatio変更時に振幅変調 | 合成側hopを固定し解析側を可変にする (§4.4) |
| REAPERシフタのモード番号が版で変わる | 別モードが選ばれる | `enumModes`/`enumSubModes` で動的構築。番号直書き禁止 |
| Phase Vocoderのフェイジー | 音質不良 | identity phase locking。駄目なら外部ライブラリへ |
| Varispeedアップシフトのエイリアシング | 高域が汚い | Sincモードでカットオフを 1/ratio に |
| サンプル差し替え時のレース | クラッシュ | shared_ptr のアトミック交換 + 解放をGCスレッドへ |
| FLAC埋め込みでfloatがクリップ | 波形破損 | 正規化係数を保存 or 32bit WAV フォールバック (§3.9) |
| MIDIをブロック先頭にまとめる | 速いフレーズでタイミングが揺れる | Phase 0 でサブブロック分割 (§2.2) を入れる |
| フレーム処理を n>=frameSize 前提で書く | サブブロック分割で破綻 | ブロック分割不変性テスト (§8.1) |
| 素材終端でボイスを即落とす | ワンショットの語尾が欠ける | `getTailSamples()`+整列バッファ のドレイン (§3.6) |
| ボイススチールでクリック | 和音の連打で目立つ | 5msフェード。まずは maxVoices に余裕を持たせる |
| ポリグライドで旧ノートを重複割り当て | 声部が潰れる | 消費済みフラグ。単体テストで検証 (§8.1) |
| スムージングを全パラメータに掛ける | ピッチの効きが鈍る等 | §3.8 の表に従う |
| CPU過負荷 (PhaseVocoder × Poly) | ドロップアウト | 重いエンジンはボイス数上限4 |
| JUCE / VST3 SDK ライセンス | 配布不可 | 非商用ならAGPLv3+GPLv3で統一 |

---

## 付録A: ReaScript コンパニオンツール（独立した成果物）

プラグインとは別に、**今すぐ書けて今すぐ使える**アイテムベースのツール。
ReaScript はメインスレッドで動くためリアルタイム楽器にはなれないが、
オフラインなら élastique を制約なく使える。

```lua
-- scripts/otomad_item_generator.lua
-- 選択した MIDI アイテムを読み、指定サンプルを各音程のオーディオアイテムとして並べる
reaper.SetMediaItemTakeInfo_Value(take, "D_PITCH",    semitones)  -- ピッチオフセット
reaper.SetMediaItemTakeInfo_Value(take, "B_PPITCH",   1)          -- レート変えずピッチのみ
reaper.SetMediaItemTakeInfo_Value(take, "I_PITCHMODE", mode)      -- 上位ワード=シフタ, 下位=サブモード
```

`I_PITCHMODE` は -1 でプロジェクト既定、それ以外は上位ワードがシフタ、下位ワードがパラメータ。
モード値は `EnumPitchShiftModes` / `EnumPitchShiftSubModes` が ReaScript から
引けるか確認して動的に列挙する（直書き禁止）。
音MADの制作フローは元々アイテムベースなので、これ単体でもかなり実用的。
プラグイン本体の Phase 1 完成を待たずに着手できる。

---

## 付録B: 参考実装・資料

- JUCE: `juce::dsp::FFT`, `AudioProcessorValueTreeState`, `FileDragAndDropTarget`,
  `VST3ClientExtensions`, `ReaperEmbeddedViewPluginDemo`
- REAPER SDK: `reaper_plugin.h` (`IReaperPitchShift`, `REAPER_PITCHSHIFT_API_VER`),
  `reaper_vst3_interfaces.h` (`IReaperHostApplication::getReaperApi`)
- clap-juce-extensions (free-audio)
- iPlug2 の REAPER プラグイン例（VST3/CLAP 両方の REAPER コンテキスト取得の実例）
- Laroche & Dolson, "Improved Phase Vocoder Time-Scale Modification of Audio" (1999)
- Verhelst & Roelands, "An Overlap-Add Technique Based on Waveform Similarity (WSOLA)" (1993)
- Julius O. Smith, "Digital Audio Resampling Home Page"
- signalsmith-stretch (MIT)
