# OtoMadSampler

音MAD用のワンショット・サンプラープラグイン（VST3 / Windows）。

音声ファイルをドラッグ＆ドロップして、鍵盤で音程を付けて鳴らします。
喋り声のように音程が揺れている素材を「一定の音程で歌う」状態に整える **FLATTEN**、
ピッチシフトアルゴリズム、ビブラート、複数サンプルの切り替えなどを備えています。

> `Granular` と `Stretch Library` は音が出ない問題があるため一時的に無効です（選ぶと
> Phase Vocoder で再生されます）。使えるのは Varispeed / WSOLA / Phase Vocoder /
> REAPER Shifter の4つです。

---

## 動作環境

- Windows 10 / 11（64bit）
- VST3 に対応した DAW（REAPER / Ableton Live / Cubase など）
- **WebView2 ランタイム**（任意）
  - 入っていれば Web UI 版のエディタになります。Windows 11 には標準で入っています。
  - 無い場合は自動的にネイティブ版エディタで開くので、そのままでも使えます。

---

## 導入

インストーラーと zip の2通りあります。どちらでも入るものは同じです。

### インストーラー（かんたん）

[Releases](https://github.com/neon-uriel/Sampler81800/releases/latest) から
`OtoMadSampler-x.y.z-Windows-Setup.exe` をダウンロードして実行します。
**先に DAW を終了してください**（起動中はファイルを置き換えられません）。

> **「Windows によって PC が保護されました」と出る場合**
>
> このインストーラーはコード署名をしていないので、SmartScreen が警告を出します。
> 「詳細情報」→「実行」で進めます。
> 気になる場合は下の zip 版を使ってください。そちらは警告が出ません
> （`.vst3` は DAW が読み込むだけで、実行ファイルとして起動しないため）。

インストール後、DAW を再起動するか、プラグインの再スキャンを実行します。
アンインストールは Windows の「アプリと機能」から行えます。

### zip（手動）

#### 1. ダウンロード

[Releases](https://github.com/neon-uriel/Sampler81800/releases/latest) から
`OtoMadSampler-Windows.zip` をダウンロードします。

#### 2. ブロックを解除する

ダウンロードした zip を右クリック →「プロパティ」→ 下部に **「セキュリティ: このファイルは
他のコンピューターから取得したものです」** と出ていたら **「許可する」にチェック** → OK。

先に解除しないと、展開後のファイルにも制限が引き継がれて DAW が読み込めないことがあります。

#### 3. 展開してコピー

zip の中に `OtoMadSampler.vst3` という**フォルダ**が入っています。これをそのまま

```
C:\Program Files\Common Files\VST3\
```

へコピーします（管理者権限の確認が出たら許可してください）。

> `.vst3` はファイルではなくフォルダです。中身だけを取り出さず、フォルダごとコピーしてください。

#### 4. DAW に読み込ませる

DAW を再起動するか、プラグインの再スキャンを実行します。
起動中の DAW にコピーしただけでは出てきません。

インストゥルメントとして `OtoMadSampler` が出てきたら成功です。

---

## 更新するとき

プラグイン側でバージョンを確認していて、新しいリリースがあるとエディタ右上にボタンが出ます。
押すと Releases ページが開きます。

**先に DAW を必ず終了してください。**
起動中は DLL がロックされていて置き換えに失敗します（失敗しても分かりにくいので注意）。

- **インストーラー版**: 新しいインストーラーをそのまま実行します。アンインストールは不要で、
  上書きされます。
- **zip 版**: 展開した `OtoMadSampler.vst3` フォルダを上書きコピーします。

インストーラー版と zip 版を混ぜても構いません。置き先が同じなので、
どちらで入れたものでも新しい方に置き換わります。
ただしインストーラーで入れたものを zip で上書きした場合、
アンインストール情報は古いバージョンのまま残ります。

設定（配色・背景画像・外部ツールのパス）は

```
%APPDATA%\OtoMadSampler\
```

に保存されます。上書き更新しても消えません。

---

## アンインストール

- **インストーラーで入れた場合**: Windows の「設定 → アプリ → インストールされているアプリ」
  から `OtoMadSampler` を削除します。プラグイン本体と、インストーラーが作ったフォルダの
  両方が消えます。
- **zip で入れた場合**: `C:\Program Files\Common Files\VST3\OtoMadSampler.vst3` フォルダを削除します。

どちらの場合も設定（配色・背景画像・外部ツールのパス）は残ります。消すなら
`%APPDATA%\OtoMadSampler\` を削除してください。

---

## 使いかた（最短）

1. 波形エリアに音声ファイルをドラッグ＆ドロップ
2. `DETECT ROOT` を押す（素材の音程を検出して、押した鍵盤の音程で鳴るようにします）
3. 鍵盤 or MIDI で演奏

覚えておくと便利なもの:

- **波形の操作** — ホイールでズーム / 右ドラッグでスクロール / 左ドラッグでトリム範囲の指定
- **鍵盤を右クリック** — その音を ROOT（原音の音程）に設定
- **鍵盤の点灯** — MIDI で押された鍵が光ります。`REAPER Shifter` のとき、赤い斜線の鍵は
  シフタの範囲外で、`FALLBACK` で選んだエンジン（既定 Varispeed）で鳴ります。赤く一瞬光った鍵は
  実際にフォールバックで鳴った鍵です。長さを保ちたいときは `FALLBACK` を WSOLA / Phase Vocoder に
  すると、そのエンジンの遅延がプラグイン全体の遅延になります
- **Space** — DAW の再生・停止（プラグイン画面にフォーカスがあっても効きます）
- **ノブをダブルクリック** — 初期値に戻す
- **VIBRATO タブ** — 発音から DELAY 待機 → FADE で立ち上げ → DEPTH の深さで揺れる
- **⚙ ボタン** — 配色・背景画像・外部ツールの設定
- **サンプルは複数持てます** — 続けてドロップすると SAMPLE 欄のプルダウンで切り替えられます。
  設定はサンプルごとに保存されるので、差し替えて聴き比べられます

### FLATTEN（音程を一定にする）

`PITCH ▲` で区間のピッチ曲線を波形に重ねて確認し、`FLATTEN` を押すと
その区間を1つの音程に揃えます。喋り声を「歌わせる」用途向けです。

- `AMOUNT` で元の抑揚をどれだけ残すか調整できます（100% で完全に平坦）
- `UNDO` で平坦化前に戻せます

---

## 外部ツール連携（任意・同梱していません）

⚙ の設定画面から、お使いの PC にあるツールを指定すると機能が増えます。
どちらも**指定しなければ従来どおりの動作**です。

### ffmpeg — mp4 なども読み込めるようにする

標準では wav / aiff / flac / mp3 / ogg を読めます。ffmpeg を指定すると、
内蔵デコーダで読めなかったファイルだけを ffmpeg に回すようになり、
**mp4 / m4a / webm / mkv / mov / opus** なども読み込めるようになります。

`winget install ffmpeg` などで入れて PATH が通っていれば、設定画面の「自動検出」で見つかります。

### élastique — REAPER 以外でも REAPER Shifter を使う（実験的）

`REAPER Shifter` は本来 REAPER 上でしか動きませんが、REAPER に同梱されている
`elastique3.dll` を指定すると、他の DAW でも使えるようになります。

> **この DLL は zplane 社のライセンス品で、再配布できません。**
> REAPER をインストール済みの環境で、ご自身の PC 上にあるものを指定してください。
> 動作は無保証です。

制限: `Duration` が Natural / Manual のときのみ有効。ストレッチ中は非対応。

モードは2つあります。**通常は `Elastique Pro` を使ってください。**

| | 範囲 | 性質 |
|---|---|---|
| `Elastique Pro` | -39〜+48 半音 | 多声OK。倍音もそのまま保たれる |
| `Soloist` | -17〜+41 半音 | **単声専用。** シフトすると倍音がほとんど落ちて音が籠る |

Soloist は単一の音程を追跡して作り直す方式なので、追跡できない成分は失われます
（実測: +7半音シフトで第2倍音以上がほぼ消失）。不具合ではなくアルゴリズムの性質です。

---

## ソースからビルドする

```
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo -j
ctest --test-dir build --output-on-failure
```

必要なもの: CMake 3.22 以降、MSVC（C++20）。
JUCE / Catch2 / signalsmith-stretch は FetchContent で自動取得されます。

Web UI を静的リンクするため、WebView2 SDK（NuGet の
`Microsoft.Web.WebView2`）が必要です。

VST3 に加えて Standalone もビルドされます（`build/OtoMadSampler_artefacts/`）。
現状 Standalone はリリースに同梱していないので、使いたい場合はビルドしてください。

### インストーラーを作る

[Inno Setup 6](https://jrsoftware.org/isinfo.php) が必要です。

```
ISCC.exe /DAppVersion=0.5.0 ^
         /DVst3Dir=<OtoMadSampler.vst3 の親フォルダ> ^
         installer\OtoMadSampler.iss
```

`dist\OtoMadSampler-<版>-Windows-Setup.exe` ができます。
タグを push すると CI が同じものを作って Release に添付します
（[.github/workflows/release.yml](.github/workflows/release.yml)）。

配布物には**コード署名をしていません**。署名を入れる場合は、CI の
`Sign installer` ステップを証明書サービスの action に差し替えてください。
2023年6月以降、OV コード署名証明書の秘密鍵はハードウェア（HSM / USBトークン）
必須になったため、`.pfx` を CI のシークレットに置く方式は使えません。

設計の詳細は [docs/DESIGN.md](docs/DESIGN.md)、開発上の不変条件は [CLAUDE.md](CLAUDE.md) にあります。

---

## ライセンス

**AGPL-3.0**（[LICENSE](LICENSE)）。

JUCE を**無償枠**で使用しており、その条件が AGPLv3 での公開であるためです。
同じ理由で、起動時に JUCE のスプラッシュが表示されます。

使用している外部ライブラリ:

- [JUCE](https://juce.com/) — AGPLv3
- [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) — MIT
- [Catch2](https://github.com/catchorg/Catch2)（テストのみ） — BSL-1.0

ffmpeg と élastique は**同梱しておらず**、ユーザーが指定したものを実行時に利用するだけです。
