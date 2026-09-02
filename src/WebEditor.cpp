#include "WebEditor.h"
#include "BinaryData.h"
#include "core/FfmpegDecoder.h"
#include "core/ParamHelp.h"
#include "core/Utf8.h"
#include "core/Params.h"
#include "core/SampleLoader.h"

namespace
{
    namespace P = otomad::params;

    // JS 側 getSliderState(name) の name。APVTS のパラメータIDをそのまま使う。
    const char* const kSliderParams[] =
    {
        P::pitchSemi, P::pitchCents, P::octave, P::rootKey, P::gain,
        P::attack, P::decay, P::sustain, P::release,
        P::sampleStart, P::sampleEnd,
        P::portaTime, P::portaCurve, P::maxVoices, P::bendRange,
        P::stretchAmount, P::formant,
        P::vibDepth, P::vibRate, P::vibDelay, P::vibFade,
        P::glideGroupMs,
    };

    // getComboBoxState(name)
    const char* const kComboParams[] =
    {
        P::algorithm, P::durationMode, P::syncLength, P::portaMode,
        P::polyMode, P::portaShape, P::interpQuality, P::elastiqueMode, P::cacheFallback,
    };

    // getToggleState(name)
    const char* const kToggleParams[] = { P::phaseLock, P::snapZeroCross };

    juce::String mimeForPath (const juce::String& path)
    {
        // テキスト系は charset を明示しないと日本語が化ける
        if (path.endsWithIgnoreCase (".html")) return "text/html; charset=utf-8";
        if (path.endsWithIgnoreCase (".js"))   return "text/javascript; charset=utf-8";
        if (path.endsWithIgnoreCase (".css"))  return "text/css; charset=utf-8";
        if (path.endsWithIgnoreCase (".png"))  return "image/png";
        return "application/octet-stream";
    }

    // "/style.css" → BinaryData の "style_css" を引く。
    // juce_add_binary_data の識別子はハイフン等が「除去」される（juce-index.js → juceindex_js）ため、
    // originalFilenames と照合する。resolvedName には実際に引いたファイル名を返す（MIME 判定用）。
    // Web に出してよいアセットだけを明示的に許可する。
    // 埋め込み資産すべてを basename で引くと、将来 juce_add_binary_data に足したものが
    // 自動的に Web から到達可能になってしまうため。
    bool isServable (const juce::String& name)
    {
        static const char* const allowed[] =
            { "index.html", "style.css", "app.js", "juce-index.js", "check_native_interop.js", "logo.png" };
        for (auto* a : allowed)
            if (name.equalsIgnoreCase (a))
                return true;
        return false;
    }

    const char* findBinaryResource (const juce::String& path, int& sizeOut, juce::String& resolvedName)
    {
        auto name = path.upToFirstOccurrenceOf ("?", false, false)
                        .upToFirstOccurrenceOf ("#", false, false)
                        .fromLastOccurrenceOf ("/", false, false);
        if (name.isEmpty())
            name = "index.html";   // ルート "/" は index.html を返す

        if (! isServable (name))
            return nullptr;

        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (name.equalsIgnoreCase (BinaryData::originalFilenames[i]))
            {
                resolvedName = name;
                return BinaryData::getNamedResource (BinaryData::namedResourceList[i], sizeOut);
            }
        return nullptr;
    }
}

//==============================================================================
bool OtoMadSamplerWebEditor::isWebViewAvailable()
{
    // WebView2 ランタイムの有無を実際に問い合わせる（未導入だと生成に失敗し、
    // JUCE は黙って IE バックエンドへ落ちて真っ白な窓になる）。
    const auto userData = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("OtoMadSampler-WebView2");
    userData.createDirectory();

    return juce::WebBrowserComponent::areOptionsSupported (
        juce::WebBrowserComponent::Options{}
            .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                         .withUserDataFolder (userData)));
}

OtoMadSamplerWebEditor::OtoMadSamplerWebEditor (OtoMadSamplerProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    // プラグインではホスト実行ファイル基準の既定ユーザーデータフォルダ（Program Files 等）に
    // 書き込めず WebView2 の生成に失敗する。失敗すると JUCE は黙って IE バックエンドへ
    // フォールバックしてしまうので、書き込める場所を明示する。
    const auto userData = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("OtoMadSampler-WebView2");
    userData.createDirectory();

    auto options = juce::WebBrowserComponent::Options{}
                      .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                      .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                   .withUserDataFolder (userData)
                                                   .withBackgroundColour (juce::Colour (0xff23272b)))
                      .withNativeIntegrationEnabled()
                      .withResourceProvider ([this] (const auto& url) { return getResource (url); });

    // --- リレー（WebBrowserComponent 生成前に Options へ登録する必要がある） ---
    for (auto* id : kSliderParams)
    {
        auto b = std::make_unique<SliderBound>();
        b->relay = std::make_unique<juce::WebSliderRelay> (id);
        options  = options.withOptionsFrom (*b->relay);
        sliders.push_back (std::move (b));
    }
    for (auto* id : kComboParams)
    {
        auto b = std::make_unique<ComboBound>();
        b->relay = std::make_unique<juce::WebComboBoxRelay> (id);
        options  = options.withOptionsFrom (*b->relay);
        combos.push_back (std::move (b));
    }
    for (auto* id : kToggleParams)
    {
        auto b = std::make_unique<ToggleBound>();
        b->relay = std::make_unique<juce::WebToggleButtonRelay> (id);
        options  = options.withOptionsFrom (*b->relay);
        toggles.push_back (std::move (b));
    }

    // --- ネイティブ関数（パラメータでは表せないもの） ---
    auto fn = [this] (auto method)
    {
        return [this, method] (const juce::Array<juce::var>& args,
                               juce::WebBrowserComponent::NativeFunctionCompletion complete)
        { (this->*method) (args, std::move (complete)); };
    };

    options = options
        .withNativeFunction ("loadSample",    fn (&OtoMadSamplerWebEditor::nfLoadSample))
        .withNativeFunction ("getWaveform",   fn (&OtoMadSamplerWebEditor::nfGetWaveform))
        .withNativeFunction ("normalize",     fn (&OtoMadSamplerWebEditor::nfNormalize))
        .withNativeFunction ("detectRoot",    fn (&OtoMadSamplerWebEditor::nfDetectRoot))
        .withNativeFunction ("note",          fn (&OtoMadSamplerWebEditor::nfNote))
        .withNativeFunction ("reaperModes",   fn (&OtoMadSamplerWebEditor::nfReaperModes))
        .withNativeFunction ("setReaperMode", fn (&OtoMadSamplerWebEditor::nfSetReaperMode))
        .withNativeFunction ("openReleases",  fn (&OtoMadSamplerWebEditor::nfOpenReleases))
        .withNativeFunction ("resetParam",    fn (&OtoMadSamplerWebEditor::nfResetParam))
        .withNativeFunction ("getAppearance", fn (&OtoMadSamplerWebEditor::nfGetAppearance))
        .withNativeFunction ("setAppearance", fn (&OtoMadSamplerWebEditor::nfSetAppearance))
        .withNativeFunction ("setBgImage",    fn (&OtoMadSamplerWebEditor::nfSetBgImage))
        .withNativeFunction ("saveAppearanceDefault", fn (&OtoMadSamplerWebEditor::nfSaveAppearanceDefault))
        .withNativeFunction ("getSamples",    fn (&OtoMadSamplerWebEditor::nfGetSamples))
        .withNativeFunction ("selectSample",  fn (&OtoMadSamplerWebEditor::nfSelectSample))
        .withNativeFunction ("removeSample",  fn (&OtoMadSamplerWebEditor::nfRemoveSample))
        .withNativeFunction ("getElastique",  fn (&OtoMadSamplerWebEditor::nfGetElastique))
        .withNativeFunction ("setElastique",  fn (&OtoMadSamplerWebEditor::nfSetElastique))
        .withNativeFunction ("browseElastique", fn (&OtoMadSamplerWebEditor::nfBrowseElastique))
        .withNativeFunction ("spaceToHost",   fn (&OtoMadSamplerWebEditor::nfSpaceToHost))
        .withNativeFunction ("getFfmpeg",     fn (&OtoMadSamplerWebEditor::nfGetFfmpeg))
        .withNativeFunction ("setFfmpeg",     fn (&OtoMadSamplerWebEditor::nfSetFfmpeg))
        .withNativeFunction ("browseFfmpeg",  fn (&OtoMadSamplerWebEditor::nfBrowseFfmpeg))
        .withNativeFunction ("flatten",       fn (&OtoMadSamplerWebEditor::nfFlatten))
        .withNativeFunction ("revertFlatten", fn (&OtoMadSamplerWebEditor::nfRevertFlatten))
        .withNativeFunction ("pitchContour",  fn (&OtoMadSamplerWebEditor::nfPitchContour))
        .withNativeFunction ("flattenState",  fn (&OtoMadSamplerWebEditor::nfFlattenState))
        .withNativeFunction ("paramHelp",     fn (&OtoMadSamplerWebEditor::nfParamHelp))
        .withNativeFunction ("ready",         fn (&OtoMadSamplerWebEditor::nfReady));

    web = std::make_unique<juce::WebBrowserComponent> (options);
    addAndMakeVisible (*web);

    // リレー生成後にパラメータへアタッチ（値の双方向同期＋ホストのジェスチャ通知）
    auto& apvts = processor.getAPVTS();
    for (std::size_t i = 0; i < sliders.size(); ++i)
        if (auto* param = apvts.getParameter (kSliderParams[i]))
            sliders[i]->attach = std::make_unique<juce::WebSliderParameterAttachment> (
                                     *param, *sliders[i]->relay, apvts.undoManager);
    for (std::size_t i = 0; i < combos.size(); ++i)
        if (auto* param = apvts.getParameter (kComboParams[i]))
            combos[i]->attach = std::make_unique<juce::WebComboBoxParameterAttachment> (
                                    *param, *combos[i]->relay, apvts.undoManager);
    for (std::size_t i = 0; i < toggles.size(); ++i)
        if (auto* param = apvts.getParameter (kToggleParams[i]))
            toggles[i]->attach = std::make_unique<juce::WebToggleButtonParameterAttachment> (
                                     *param, *toggles[i]->relay, apvts.undoManager);

    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    processor.checkForUpdatesAsync();
    // 20Hz。鍵盤の点灯を運ぶので 10Hz だと押してから光るまでが目に見えて遅れる。
    // status 側は内容比較で早期棄却するので、周期を上げても JSON 化の回数は増えない。
    startTimerHz (20);

    // 全パネル（SAMPLE / ENV+PITCH / ENGINE+REAPER / KEYBOARD）が収まる既定サイズ。
    // 幅が狭いと ENGINE のセレクトが折り返して縦に伸び、鍵盤が切れる。
    // 折り返さない幅を下限にする。
    //
    // 高さの内訳（100% スケール時、いずれも実測）:
    //   上部バー 28 / SAMPLE 190 / AMP ENV+PITCH 180 / ENGINE+REAPER 232
    //   / KEYBOARD 83（見出し19 + 余白6 + 鍵盤52 + 下余白6）/ パネル間の隙間 24
    //   / body の上下余白 12  = 合計 約 749
    // 旧値は下限 660・既定 755 で、下限では鍵盤が確実に切れ、既定でもぎりぎりだった。
    // 鍵盤が切れない値を下限にし、既定には余裕を持たせる。
    static constexpr int kMinW = 860, kMinH = 760;
    setResizable (true, true);
    setResizeLimits (kMinW, kMinH, 2400, 1600);

    // 前回のサイズがあれば復元（無ければ既定）。
    // 下限を上げたので、以前の state に入っている小さい高さは既定へ引き上げる
    // （そのまま渡しても setResizeLimits に丸められるが、意図を明示しておく）。
    const int sw = processor.getEditorW(), sh = processor.getEditorH();
    setSize (sw >= kMinW ? sw : 890, sh >= kMinH ? sh : 800);
}

OtoMadSamplerWebEditor::~OtoMadSamplerWebEditor() { stopTimer(); }

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource>
OtoMadSamplerWebEditor::getResource (const juce::String& url) const
{
    int size = 0;
    juce::String resolved;
    if (const auto* data = findBinaryResource (url, size, resolved))
    {
        const auto* bytes = reinterpret_cast<const std::byte*> (data);
        // MIME は「解決後のファイル名」から決める。ルート "/" のまま判定すると
        // application/octet-stream になり、WebView2 が表示ではなくダウンロード扱いにしてしまう。
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (bytes, bytes + size), mimeForPath (resolved) };
    }
    return std::nullopt;
}

//==============================================================================
// JS: loadSample(base64, name) — D&D / ファイル選択で得たバイト列を読み込む
void OtoMadSamplerWebEditor::nfLoadSample (const juce::Array<juce::var>& args,
                                           juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() < 1)
    { complete (false); return; }

    // JS の btoa() は「標準」base64。MemoryBlock::fromBase64Encoding は JUCE 独自形式なので使えない。
    juce::MemoryBlock mb;
    {
        juce::MemoryOutputStream out (mb, false);
        if (! juce::Base64::convertFromBase64 (out, args[0].toString()))
        { complete (false); return; }
    }
    if (mb.getSize() == 0)
    { complete (false); return; }

    const auto name = args.size() > 1 ? args[1].toString() : juce::String ("sample");
    processor.loadSampleFromMemory (std::move (mb), name);
    complete (true);
}

// JS: getWaveform() — 波形ピーク（min,max を平坦化した配列）とメタ情報を返す
void OtoMadSamplerWebEditor::nfGetWaveform (const juce::Array<juce::var>&,
                                            juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    auto* obj = new juce::DynamicObject();
    const auto* sb = processor.getActiveSample();

    if (sb != nullptr && ! sb->peaks.empty())
    {
        juce::Array<juce::var> flat;
        flat.ensureStorageAllocated ((int) sb->peaks.size() * 2);
        for (const auto& mm : sb->peaks) { flat.add (mm.first); flat.add (mm.second); }

        obj->setProperty ("peaks",   flat);
        obj->setProperty ("name",    juce::String (sb->name));
        obj->setProperty ("seconds", sb->sampleRate > 0.0 ? (double) sb->numSamples / sb->sampleRate : 0.0);
        obj->setProperty ("normGain", processor.getNormGain());
    }
    obj->setProperty ("version", processor.getSampleVersion());
    complete (juce::var (obj));
}

void OtoMadSamplerWebEditor::nfNormalize (const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    processor.normalizeSample();
    complete (true);
}

void OtoMadSamplerWebEditor::nfDetectRoot (const juce::Array<juce::var>&,
                                           juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (processor.detectAndSetRoot());
}

// JS: note(midiNote, isOn) — 画面鍵盤
void OtoMadSamplerWebEditor::nfNote (const juce::Array<juce::var>& args,
                                     juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() >= 2)
    {
        const int  n  = juce::jlimit (0, 127, (int) args[0]);
        const bool on = (bool) args[1];
        auto& ks = processor.getKeyboardState();
        if (on) ks.noteOn  (1, n, 0.9f);
        else    ks.noteOff (1, n, 0.0f);
    }
    complete (true);
}

// JS: reaperModes() — モード名/サブモード名の一覧（動的なのでパラメータでは表せない）
void OtoMadSamplerWebEditor::nfReaperModes (const juce::Array<juce::var>&,
                                            juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    auto* obj = new juce::DynamicObject();
    juce::Array<juce::var> modes;
    const auto names = processor.getReaperModeNames();
    for (const auto& n : names) modes.add (n);

    const int cur = (int) processor.getAPVTS().getRawParameterValue (P::reaperMode)->load();
    juce::Array<juce::var> subs;
    for (const auto& s : processor.getReaperSubModeNames (cur)) subs.add (s);

    obj->setProperty ("available", processor.isReaperAvailable());
    obj->setProperty ("modes", modes);
    obj->setProperty ("subs",  subs);
    obj->setProperty ("mode",  cur);
    obj->setProperty ("sub",   (int) processor.getAPVTS().getRawParameterValue (P::reaperSubMode)->load());
    complete (juce::var (obj));
}

// JS: setReaperMode(mode, sub)
void OtoMadSamplerWebEditor::nfSetReaperMode (const juce::Array<juce::var>& args,
                                              juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    // ホストのオートメーション記録が正しく動くよう、ジェスチャで挟む
    auto setInt = [this] (const char* id, int v)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*> (processor.getAPVTS().getParameter (id)))
        {
            p->beginChangeGesture();
            *p = v;
            p->endChangeGesture();
        }
    };
    if (args.size() >= 1) setInt (P::reaperMode,    (int) args[0]);
    if (args.size() >= 2) setInt (P::reaperSubMode, (int) args[1]);
    complete (true);
}

void OtoMadSamplerWebEditor::nfOpenReleases (const juce::Array<juce::var>&,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    OtoMadSamplerProcessor::getReleasesUrl().launchInDefaultBrowser();
    complete (true);
}

// JS: resetParam(id) — ダブルクリックでパラメータを既定値へ戻す。
// 既定値はパラメータ自身が持っているので、JS 側に持たせず C++ で解決する。
void OtoMadSamplerWebEditor::nfResetParam (const juce::Array<juce::var>& args,
                                           juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() >= 1)
        if (auto* param = processor.getAPVTS().getParameter (args[0].toString()))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getDefaultValue());
            param->endChangeGesture();
            complete (true);
            return;
        }
    complete (false);
}

// JS: getSamples() — 読み込み済みサンプル名の一覧と選択中インデックス
void OtoMadSamplerWebEditor::nfGetSamples (const juce::Array<juce::var>&,
                                           juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> names;
    for (int i = 0; i < processor.getSampleCount(); ++i)
        names.add (processor.getSampleName (i));
    o->setProperty ("names",  names);
    o->setProperty ("active", processor.getActiveIndex());
    complete (juce::var (o));
}

void OtoMadSamplerWebEditor::nfSelectSample (const juce::Array<juce::var>& args,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() >= 1)
        processor.selectSample ((int) args[0]);
    complete (true);
}

void OtoMadSamplerWebEditor::nfRemoveSample (const juce::Array<juce::var>& args,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() >= 1)
        processor.removeSample ((int) args[0]);
    complete (true);
}

// JS: spaceToHost() — WebView が食った Space をホスト窓へ投げ直す（DAW の再生/停止）。
void OtoMadSamplerWebEditor::nfSpaceToHost (const juce::Array<juce::var>&,
                                            juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    // ホスト窓を探す起点はプラグインのエディタ窓（WebView の HWND ではなく、その親側）。
    complete (processor.forwardSpaceKeyToHost (getWindowHandle()));
}

namespace
{
    // ffmpeg 設定の状態を JSON 化（available / path / found）
    juce::var makeFfmpegState (const OtoMadSamplerProcessor& p)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("available", p.isFfmpegAvailable());
        o->setProperty ("path",      p.getFfmpegPath());
        // 未設定のときだけ「自動検出でここが見つかりますよ」を出す
        o->setProperty ("found", p.isFfmpegAvailable() ? juce::String()
                                                       : otomad::FfmpegDecoder::find().getFullPathName());
        return juce::var (o);
    }
}

// JS: flatten(strength01) — 区間内のピッチを検出して単一の音程へ平坦化する。
// 実処理は背景スレッドなので、完了は sampleChanged イベントで分かる。
void OtoMadSamplerWebEditor::nfFlatten (const juce::Array<juce::var>& args,
                                        juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    const float st = args.size() >= 1 ? (float) (double) args[0] : 1.0f;
    processor.flattenActiveSample (st);
    complete (true);
}

// JS: revertFlatten() — 平坦化前のバッファへ戻す
void OtoMadSamplerWebEditor::nfRevertFlatten (const juce::Array<juce::var>&,
                                              juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (processor.revertFlatten());
}

// JS: pitchContour() — 波形に重ねるピッチ曲線（音は変えない）
void OtoMadSamplerWebEditor::nfPitchContour (const juce::Array<juce::var>&,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (processor.analysePitchContour());
}

// JS: flattenState() — 平坦化の結果と undo 可否。音名の整形は JS 側で行う。
void OtoMadSamplerWebEditor::nfFlattenState (const juce::Array<juce::var>&,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (processor.getFlattenState());
}

// JS: ready() — ページの初期化が終わった合図。
// status は「変わったときだけ」送るが、エディタを開いた直後の 1 回目はページの JS が
// まだ無くて捨てられる（送った記録だけ残る）。以後何も変わらないと二度と届かず、開き直した
// エディタで elastique 判定や Shifter 欄の文言が初期値のまま止まる。ここで送信済み記録を捨てて再送させる。
void OtoMadSamplerWebEditor::nfReady (const juce::Array<juce::var>&,
                                      juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    lastStatusJson.clear(); lastShifterText.clear(); lastCacheDebug.clear();
    lastProgPct = -1; lastReaperKey = -1; lastCacheCount = -1;
    complete (true);
}

// JS: paramHelp() — パラメータIDごとのホバーヘルプ（{id: 文言}）。
// 文言は core/ParamHelp.h にあり、ネイティブ版エディタの setTooltip と同じものを使う。
void OtoMadSamplerWebEditor::nfParamHelp (const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    auto* o = new juce::DynamicObject();
    std::size_t n = 0;
    const auto* t = otomad::params::helpTable (n);
    for (std::size_t i = 0; i < n; ++i)
        o->setProperty (juce::Identifier (t[i].id),
                        juce::String (juce::CharPointer_UTF8 (t[i].text)));
    complete (juce::var (o));
}

// JS: getFfmpeg()
void OtoMadSamplerWebEditor::nfGetFfmpeg (const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (makeFfmpegState (processor));
}

// JS: setFfmpeg(path) — "-" で解除、空文字なら自動探索、それ以外は明示パス。
void OtoMadSamplerWebEditor::nfSetFfmpeg (const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    const auto path = args.size() >= 1 ? args[0].toString() : juce::String();
    if (path == "-")
        processor.clearFfmpegPath();
    else
        processor.setFfmpegPath (path);

    complete (makeFfmpegState (processor));
}

// JS: browseFfmpeg() — ネイティブのファイル選択（WebView からは実パスが取れない）
void OtoMadSamplerWebEditor::nfBrowseFfmpeg (const juce::Array<juce::var>&,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    exeChooser = std::make_unique<juce::FileChooser> (otomad::u8 ("ffmpeg の実行ファイルを選択"),
                                                      juce::File(), "ffmpeg*");
    juce::Component::SafePointer<OtoMadSamplerWebEditor> safe (this);
    exeChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe, complete] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        if (const auto f = fc.getResult(); f.existsAsFile())
            safe->processor.setFfmpegPath (f.getFullPathName());
        complete (makeFfmpegState (safe->processor));
    });
}

namespace
{
    // élastique 実験機能の状態を JSON 化（loaded / path / reaperHosted / found[]）
    juce::var makeElastiqueState (const OtoMadSamplerProcessor& p)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("loaded",       p.isElastiqueLoaded());
        o->setProperty ("path",         p.getElastiquePath());
        o->setProperty ("reaperHosted", p.isReaperAvailable());

        juce::Array<juce::var> cands;
        for (const auto& c : otomad::ElastiqueDirect::defaultCandidates())
            if (juce::File (juce::String (c)).existsAsFile())
                cands.add (juce::String (c));
        o->setProperty ("found", cands);
        return juce::var (o);
    }
}

// JS: getElastique() — 実験機能の状態（REAPER外で Shifter を使うための DLL 直叩き）
void OtoMadSamplerWebEditor::nfGetElastique (const juce::Array<juce::var>&,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    complete (makeElastiqueState (processor));
}

// JS: setElastique(path) — "-" で解除、空文字なら既定候補を自動探索、それ以外は明示パス。
void OtoMadSamplerWebEditor::nfSetElastique (const juce::Array<juce::var>& args,
                                             juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    const auto path = args.size() >= 1 ? args[0].toString() : juce::String();
    if (path == "-")
        processor.unloadElastiqueDll();
    else
        processor.loadElastiqueDll (path);

    complete (makeElastiqueState (processor));
}

// JS: browseElastique() — ネイティブのファイル選択。WebView 側の <input type=file> では
// フルパスが取れない（セキュリティ上意図的に隠される）ので、DLL だけはネイティブで選ぶ。
void OtoMadSamplerWebEditor::nfBrowseElastique (const juce::Array<juce::var>&,
                                                juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    dllChooser = std::make_unique<juce::FileChooser> (otomad::u8 ("elastique3.dll を選択"),
                                                      juce::File ("C:\\Program Files"), "*.dll");
    // エディタが先に閉じられてもコールバックは飛んでくるので SafePointer で自身の生存を確認する。
    juce::Component::SafePointer<OtoMadSamplerWebEditor> safe (this);
    dllChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe, complete] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const auto f = fc.getResult();
        if (f.existsAsFile())
            safe->processor.loadElastiqueDll (f.getFullPathName());
        complete (makeElastiqueState (safe->processor));
    });
}

// JS: getAppearance() — 色 / 背景の不透明度 / 背景画像(data URL)
void OtoMadSamplerWebEditor::nfGetAppearance (const juce::Array<juce::var>&,
                                              juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    auto* o = new juce::DynamicObject();
    const juce::Colour c (processor.getMainColourARGB());
    o->setProperty ("colour",  "#" + c.toDisplayString (false));   // #rrggbb
    o->setProperty ("opacity", processor.getBgOpacity());
    o->setProperty ("panel",   processor.getPanelOpacity());

    const auto& png = processor.getBackgroundPng();
    if (png.getSize() > 0)
    {
        juce::MemoryOutputStream b64;
        juce::Base64::convertToBase64 (b64, png.getData(), png.getSize());
        o->setProperty ("bg", "data:image/png;base64," + b64.toString());
    }
    complete (juce::var (o));
}

// JS: setAppearance(colourHex, opacity)
void OtoMadSamplerWebEditor::nfSetAppearance (const juce::Array<juce::var>& args,
                                              juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    if (args.size() >= 1)
    {
        const auto hex = args[0].toString().removeCharacters ("#");
        if (hex.isNotEmpty())
            processor.setMainColourARGB (juce::Colour::fromString ("ff" + hex).getARGB());
    }
    if (args.size() >= 2)
        processor.setBgOpacity ((float) (double) args[1]);
    if (args.size() >= 3)
        processor.setPanelOpacity ((float) (double) args[2]);
    complete (true);
}

// JS: setBgImage(base64 | null) — null / 空でクリア
void OtoMadSamplerWebEditor::nfSetBgImage (const juce::Array<juce::var>& args,
                                           juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    const auto b64 = args.size() >= 1 ? args[0].toString() : juce::String();
    if (b64.isEmpty())
    {
        processor.clearBackgroundImage();
        complete (true);
        return;
    }

    juce::MemoryBlock mb;
    {
        juce::MemoryOutputStream out (mb, false);
        if (! juce::Base64::convertFromBase64 (out, b64))
        { complete (false); return; }
    }
    processor.setBackgroundImageFromMemory (mb.getData(), mb.getSize());
    complete (true);
}

void OtoMadSamplerWebEditor::nfSaveAppearanceDefault (const juce::Array<juce::var>&,
                                                      juce::WebBrowserComponent::NativeFunctionCompletion complete)
{
    processor.saveAppearanceAsDefault();
    complete (true);
}

//==============================================================================
void OtoMadSamplerWebEditor::timerCallback()
{
    processor.serviceCache();

    // REAPER モード/サブモードが変わったらエンジンを再設定する。
    // パラメータを変えるだけでは反映されず、選択と実際のモードがズレる（SoundTouch のままになる）。
    {
        auto& apvts = processor.getAPVTS();
        const int rm  = (int) apvts.getRawParameterValue (P::reaperMode)->load();
        const int rsm = (int) apvts.getRawParameterValue (P::reaperSubMode)->load();
        if (rm != lastReaperMode || rsm != lastReaperSubMode)
        {
            lastReaperMode = rm; lastReaperSubMode = rsm;
            processor.reconfigureReaperMode();
        }
    }

    if (web == nullptr)
        return;

    // サンプルが差し替わったら JS 側へ通知（JS が getWaveform を呼び直す）
    const int ver = processor.getSampleVersion();
    if (ver != lastSampleVersion)
    {
        lastSampleVersion = ver;
        auto* o = new juce::DynamicObject();
        o->setProperty ("version", ver);
        web->emitEventIfBrowserIsVisible ("sampleChanged", juce::var (o));
    }

    // 外観（色/背景）が変わったら通知（他インスタンスからのブロードキャストや state 復元を反映）
    const int av = processor.getAppearanceVersion();
    if (av != lastAppearanceVersion)
    {
        lastAppearanceVersion = av;
        auto* o = new juce::DynamicObject();
        o->setProperty ("version", av);
        web->emitEventIfBrowserIsVisible ("appearanceChanged", juce::var (o));
    }

    // 読み込み失敗の通知。デコードは背景スレッドなので戻り値では返せない。
    // status とは別イベントにする（status は安価なスカラー比較で早期棄却するため
    // 文字列だけ変わったケースを取りこぼす）。
    {
        const auto err = processor.getLastLoadError();
        if (err != lastLoadErrorSeen)
        {
            lastLoadErrorSeen = err;
            if (err.isNotEmpty())
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("message", err);
                web->emitEventIfBrowserIsVisible ("loadError", juce::var (o));
            }
        }
    }

    // 鍵盤表示。status と別イベントにするのは、点灯は頻繁に変わり status の早期棄却を潰すため。
    // 128 鍵を 1 文字ずつの文字列で送る（JS は 64bit 整数を扱えない）。
    //   state: '0' 通常 / '1' 生成待ち / '2' 作れない   held: '0' 離 / '1' 押 / '2' 前回以降に鳴って離された
    //   miss : '1' 前回以降にキャッシュを外した
    {
        std::uint64_t held[2], latched[2], miss[2];
        processor.fetchKeyActivity (held, latched, miss);
        juce::String state, heldStr, missStr;
        state.preallocateBytes (128); heldStr.preallocateBytes (128); missStr.preallocateBytes (128);
        for (int n = 0; n < 128; ++n)
        {
            const std::uint64_t bit = 1ull << (n & 63);
            const bool h = (held[n >> 6] & bit) != 0;
            const bool l = (latched[n >> 6] & bit) != 0;
            state   += (juce::juce_wchar) ('0' + processor.keyCacheState (n));
            heldStr += (juce::juce_wchar) (h ? '1' : (l ? '2' : '0'));
            missStr += (juce::juce_wchar) ((miss[n >> 6] & bit) != 0 ? '1' : '0');
        }
        // latched/miss は読んだ時点で消えるので、立っていたら必ず送る（比較で落とさない）。
        const bool transient = (latched[0] | latched[1] | miss[0] | miss[1]) != 0;
        if (transient || state != lastKeyState || heldStr != lastKeyHeld)
        {
            lastKeyState = state; lastKeyHeld = heldStr;
            auto* k = new juce::DynamicObject();
            k->setProperty ("state", state);
            k->setProperty ("held",  heldStr);
            k->setProperty ("miss",  missStr);
            web->emitEventIfBrowserIsVisible ("keys", juce::var (k));
        }
    }

    // 状態（キャッシュ進捗・REAPERモード名・更新有無）。
    // 20Hz で回るので、まず安価なスカラー比較で早期に抜ける（毎回 JSON 化しない）。
    {
        const bool  busy = processor.isCacheBusy();
        const int   prog = juce::roundToInt (processor.getCacheProgress() * 100.0f);
        const bool  fb   = processor.isEngineFallbackActive();
        const bool  upd  = processor.isUpdateAvailable();
        const int   rmv  = lastReaperMode * 1000 + lastReaperSubMode;
        const int   cnt  = processor.getCacheReadyCount() * 1000 + processor.getCachePendingCount();
        // SHIFTER 欄の文言は durationMode / stretch でも変わる。これらは上のスカラーに
        // 含まれないので、文字列そのものを比較対象に入れないと更新を取りこぼす。
        const auto  shifter = processor.getShifterStatusText();
        // 診断文字列も比較に入れる。入れないと ready/pending が動かないまま
        // 世代だけが上がっている状態（＝まさに切り分けたい状態）で status が飛ばず、
        // 表示が固まって「世代は止まっている」と誤読させる。
        const auto  dbg     = processor.getCacheDebugText();
        if (busy == lastBusy && prog == lastProgPct && fb == lastFallback
            && upd == lastUpdateAvail && rmv == lastReaperKey && shifter == lastShifterText
            && cnt == lastCacheCount && dbg == lastCacheDebug)
            return;
        lastBusy = busy; lastProgPct = prog; lastFallback = fb;
        lastUpdateAvail = upd; lastReaperKey = rmv; lastShifterText = shifter;
        lastCacheCount = cnt; lastCacheDebug = dbg;
    }

    auto* s = new juce::DynamicObject();
    s->setProperty ("cacheBusy",     processor.isCacheBusy());
    s->setProperty ("cacheProgress", processor.getCacheProgress());
    s->setProperty ("cacheReady",    processor.getCacheReadyCount());
    s->setProperty ("cachePending",  processor.getCachePendingCount());
    s->setProperty ("cacheDebug",    processor.getCacheDebugText());
    s->setProperty ("reaper",        processor.getShifterStatusText());
    s->setProperty ("elastique",     processor.isElastiqueLoaded());
    s->setProperty ("fallback",      processor.isEngineFallbackActive());
    s->setProperty ("updateAvail",   processor.isUpdateAvailable());
    s->setProperty ("latest",        processor.getLatestVersion());
    s->setProperty ("version",       OtoMadSamplerProcessor::getCurrentVersion());

    const juce::var status (s);
    const auto json = juce::JSON::toString (status, true);
    if (json != lastStatusJson)
    {
        lastStatusJson = json;
        web->emitEventIfBrowserIsVisible ("status", status);
    }
}

void OtoMadSamplerWebEditor::resized()
{
    if (web != nullptr)
        web->setBounds (getLocalBounds());

    processor.setEditorSize (getWidth(), getHeight());   // 次回同じ大きさで開く
}
