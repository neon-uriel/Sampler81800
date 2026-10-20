#include "PluginEditor.h"
#include "core/ParamHelp.h"
#include "BinaryData.h"

namespace P = otomad::params;

//==============================================================================
// ホバーヘルプの文言は core/ParamHelp.h に集約している（Web UI 版と共通）。
static juce::String helpForParam (const juce::String& id)
{
    return juce::String (juce::CharPointer_UTF8 (otomad::params::helpFor (id.toRawUTF8())));
}

//==============================================================================
OtoMadSamplerEditor::OtoMadSamplerEditor (OtoMadSamplerProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      waveform (p, waveView), dropZone (p, waveView),
      keyboard (p.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard),
      curveDisplay (p), adsrDisplay (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    addAndMakeVisible (dropZone);
    addAndMakeVisible (keyboard);
    addAndMakeVisible (curveDisplay);
    addAndMakeVisible (adsrDisplay);

    // 画面キーボードは固定ベロシティ（クリック位置依存で小さくなるのを防ぐ）
    keyboard.setVelocity (0.9f, false);
    keyboard.onSetRoot = [this] (int n) { setIntParam (P::rootKey, n); };   // 右クリックで Root 設定

    addAndMakeVisible (normalizeButton);
    normalizeButton.onClick = [this] { processor.normalizeSample(); };

    addAndMakeVisible (detectButton);
    detectButton.setTooltip (juce::String (juce::CharPointer_UTF8 ("読み込んだ音の基音を検出して Root に自動設定")));
    detectButton.onClick = [this]
    {
        const bool ok = processor.detectAndSetRoot();
        detectButton.setButtonText (ok ? "Detect \xe2\x9c\x93"
                                       : juce::String (juce::CharPointer_UTF8 ("\xe4\xb8\x8d\xe6\x98\x8e")));   // 不明
        juce::Timer::callAfterDelay (900, [this] { detectButton.setButtonText ("Detect"); });
    };

    const auto rotary = juce::Slider::RotaryHorizontalVerticalDrag;
    const auto linear = juce::Slider::LinearHorizontal;

    kSemi  = &addKnob (P::pitchSemi,   "Semi",  rotary);
    kCents = &addKnob (P::pitchCents,  "Cent",  rotary);
    kOct   = &addKnob (P::octave,      "Oct",   rotary);
    kRoot  = &addKnob (P::rootKey,     "Root",  rotary);
    kGain  = &addKnob (P::gain,        "Gain",  rotary);
    kAtk   = &addKnob (P::attack,      "A",     rotary);
    kDec   = &addKnob (P::decay,       "D",     rotary);
    kSus   = &addKnob (P::sustain,     "S",     rotary);
    kRel   = &addKnob (P::release,     "R",     rotary);
    kStart = &addKnob (P::sampleStart, "Start", linear);
    kEnd   = &addKnob (P::sampleEnd,   "End",   linear);
    kPTime = &addKnob (P::portaTime,   "Glide", rotary);
    kMaxV  = &addKnob (P::maxVoices,   "Voices",rotary);
    kBend  = &addKnob (P::bendRange,   "Bend",  rotary);
    kStretch = &addKnob (P::stretchAmount, "Stretch", linear);   // 横フェーダー
    // FORMANT は UI から外している（実装が使い物にならないため）。
    // パラメータは APVTS に残っているので state 互換は保たれる（規約12）。

    // Root は音名表示（例: C#4）。C4=60 表記。右クリック鍵盤や検出とも連動。
    kRoot->slider.textFromValueFunction = [] (double v)
    { return juce::MidiMessage::getMidiNoteName ((int) v, true, true, 4); };
    kRoot->slider.valueFromTextFunction = [] (const juce::String& t) { return (double) t.getIntValue(); };
    kRoot->slider.updateText();

    // REAPER Mode / Sub は動的名リストのコンボ（アタッチメント無し・手動でパラメータへ）
    rModeLbl.setText ("R.Mode", juce::dontSendNotification); rModeLbl.setJustificationType (juce::Justification::centred);
    rSubLbl.setText  ("R.Sub",  juce::dontSendNotification); rSubLbl.setJustificationType  (juce::Justification::centred);
    addAndMakeVisible (rModeLbl); addAndMakeVisible (rSubLbl);
    addAndMakeVisible (rModeBox); addAndMakeVisible (rSubBox);
    rModeBox.onChange = [this] { if (rModeBox.getSelectedId() > 0) setIntParam (P::reaperMode,    rModeBox.getSelectedId() - 1); };
    rSubBox.onChange  = [this] { if (rSubBox.getSelectedId()  > 0) setIntParam (P::reaperSubMode, rSubBox.getSelectedId()  - 1); };

    cPMode  = &addCombo (P::portaMode,     "Porta",  { "Off", "Legato", "Always" });
    cAlgo   = &addCombo (P::algorithm,     "Algo",
                         { "Varispeed", "WSOLA", "Phase Vocoder", "Granular", "Stretch Library", "REAPER Shifter" });
    cDur    = &addCombo (P::durationMode,  "Duration", { "Natural", "Sync", "Manual" });
    cSync   = &addCombo (P::syncLength,    "Sync",   { "1/4", "1/2", "1", "2", "4" });

    // Snap ZC は常時ON運用なので UI には出さない（パラメータは規約#12に従い存続）
    addChildComponent (snapButton);
    snapAttach = std::make_unique<ButtonAttach> (processor.getAPVTS(), P::snapZeroCross, snapButton);
    addAndMakeVisible (phaseLockButton);
    phaseLockButton.setTooltip (helpForParam (P::phaseLock));
    phaseLockAttach = std::make_unique<ButtonAttach> (processor.getAPVTS(), P::phaseLock, phaseLockButton);

    // 手動コントロールのホバーヘルプ
    {
        auto u = [] (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); };
        rModeBox.setTooltip (u ("REAPER ピッチシフタのモード（élastique / SoundTouch など）"));
        rSubBox.setTooltip  (u ("選択モードのサブモード"));
        normalizeButton.setTooltip (u ("原音のピークを 0dB 近くに正規化"));
        settingsButton.setTooltip  (u ("外観設定（背景画像・色）"));
    }

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible (statusLabel);

    reaperModeLabel.setJustificationType (juce::Justification::centredLeft);
    reaperModeLabel.setColour (juce::Label::textColourId, juce::Colours::aqua);
    reaperModeLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (reaperModeLabel);

    logo = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);

    // キャッシュ生成の進捗バー（生成中のみ表示）
    cacheBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff0f0f13));
    addChildComponent (cacheBar);   // 既定は非表示。busy 時に setVisible

    // 設定ボタン（ヘッダ右上）と設定オーバーレイ
    settingsButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x9a\x99"));   // ⚙
    settingsButton.onClick = [this] { settingsOverlay.refreshFromProcessor(); settingsOverlay.setVisible (true); };
    addAndMakeVisible (settingsButton);

    settingsOverlay.onChanged = [this] { applyMainColour(); repaint(); };
    addChildComponent (settingsOverlay);   // 既定は非表示

    // アップデート確認バナー（更新があるときだけ表示。クリックでリリースページを開く）
    updateButton.setColour (juce::TextButton::buttonColourId, juce::Colours::darkorange);
    updateButton.onClick = [] { OtoMadSamplerProcessor::getReleasesUrl().launchInDefaultBrowser(); };
    addChildComponent (updateButton);
    processor.checkForUpdatesAsync();   // 起動時に一度、背景で確認

    // UI スケール選択（50/100/125/150/200%）
    scaleBox.addItem ("50%",  1); scaleBox.addItem ("75%",  2); scaleBox.addItem ("100%", 3);
    scaleBox.addItem ("125%", 4); scaleBox.addItem ("150%", 5); scaleBox.addItem ("200%", 6);
    scaleBox.setTooltip (juce::String (juce::CharPointer_UTF8 ("UI 表示倍率")));
    scaleBox.onChange = [this]
    {
        const int idx = juce::jlimit (0, kNumScales - 1, scaleBox.getSelectedId() - 1);
        applyUiScale (kScales[idx]);
    };
    addAndMakeVisible (scaleBox);

    // 全コントロールをスケール用コンテンツ層へ移し替える（エディタ直下ではなく content の子にする）。
    {
        juce::Array<juce::Component*> kids (getChildren());
        for (auto* c : kids)
            content.addChildComponent (c);   // 可視状態は保持したまま親を content に
    }
    content.onPaint = [this] (juce::Graphics& g) { paintContent (g); };
    addAndMakeVisible (content);

    applyMainColour();
    lastAppearanceVersion = processor.getAppearanceVersion();

    startTimerHz (8);
    applyUiScale (processor.getUiScalePct());   // 保存された倍率で初期化（setSize もここで）
}

OtoMadSamplerEditor::~OtoMadSamplerEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void OtoMadSamplerEditor::applyMainColour()
{
    const juce::Colour c (processor.getMainColourARGB());

    // ノブ/フェーダー（LookAndFeel 経由で全スライダーに適用）
    lnf.setColour (juce::Slider::rotarySliderFillColourId, c);
    lnf.setColour (juce::Slider::thumbColourId,            c);
    lnf.setColour (juce::Slider::trackColourId,            c.withAlpha (0.55f));
    lnf.setColour (juce::ProgressBar::foregroundColourId,  c);

    // カスタム描画のコンポーネントはメインカラーを直接読むので repaint で十分
    cacheBar.setColour (juce::ProgressBar::foregroundColourId, c);
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, c.withAlpha (0.7f));
    keyboard.markerColour = c.withRotatedHue (0.5f);   // Root 鍵は補色でマーク

    repaint();
}

void OtoMadSamplerEditor::setIntParam (const juce::String& id, int value)
{
    if (auto* p = dynamic_cast<juce::AudioParameterInt*> (processor.getAPVTS().getParameter (id)))
        *p = value;
}

void OtoMadSamplerEditor::timerCallback()
{
    processor.serviceCache();   // ピッチキャッシュの保守（背景レンダリング要求の処理）

    // 外観が外部（状態復元など）で変わったら反映
    const int av = processor.getAppearanceVersion();
    if (av != lastAppearanceVersion)
    {
        lastAppearanceVersion = av;
        applyMainColour();      // 内部で repaint
    }

    auto& apvts = processor.getAPVTS();
    const int algo = (int) apvts.getRawParameterValue (P::algorithm)->load();
    const int dur  = (int) apvts.getRawParameterValue (P::durationMode)->load();

    keyboard.setRootNote ((int) apvts.getRawParameterValue (P::rootKey)->load());   // Root 鍵の色付け

    // アップデートあり → バナー表示（一度だけ）
    if (processor.isUpdateAvailable() && ! updateButton.isVisible())
    {
        updateButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\xac\x86 v"))
                                    + processor.getLatestVersion() + juce::String (juce::CharPointer_UTF8 (" \xe6\x9b\xb4\xe6\x96\xb0")));  // ⬆ vX 更新
        updateButton.setVisible (true);
        resized();
    }

    // モード名コンボを一度だけ埋める（REAPER利用可時）
    if (processor.isReaperAvailable() && ! modePopulated)
    {
        const auto names = processor.getReaperModeNames();
        rModeBox.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i)
            rModeBox.addItem (names[i].isEmpty() ? ("mode " + juce::String (i)) : names[i], i + 1);
        modePopulated = names.size() > 0;
    }

    const int rm  = (int) apvts.getRawParameterValue (P::reaperMode)->load();
    const int rsm = (int) apvts.getRawParameterValue (P::reaperSubMode)->load();

    // モードが変わったらサブモード名コンボを詰め直す
    if (processor.isReaperAvailable() && rm != subPopulatedForMode)
    {
        subPopulatedForMode = rm;
        const auto subs = processor.getReaperSubModeNames (rm);
        rSubBox.clear (juce::dontSendNotification);
        for (int i = 0; i < subs.size(); ++i)
            rSubBox.addItem (subs[i].isEmpty() ? ("sub " + juce::String (i)) : subs[i], i + 1);
    }

    // 現在のパラメータ値をコンボ選択に反映（onChangeを起こさない）
    if (rModeBox.getSelectedId() != rm + 1)
        rModeBox.setSelectedId (rm + 1, juce::dontSendNotification);
    if (rSubBox.getNumItems() > 0)
    {
        const int subId = juce::jlimit (1, rSubBox.getNumItems(), rsm + 1);
        if (rSubBox.getSelectedId() != subId)
            rSubBox.setSelectedId (subId, juce::dontSendNotification);
    }

    // Mode / Sub が変わったら安全に再設定＋レイテンシ再報告
    if (rm != lastReaperMode || rsm != lastReaperSubMode)
    {
        lastReaperMode = rm;
        lastReaperSubMode = rsm;
        processor.reconfigureReaperMode();
    }

    // 現在の REAPER モード名/レイテンシを表示（REAPER Shifter 選択時のみ）
    reaperModeLabel.setText (algo == 5 ? processor.getReaperModeText() : juce::String(),
                             juce::dontSendNotification);

    juce::String msg;
    if (processor.isEngineFallbackActive())   // 未実装/非対応(REAPER非対応ホスト等) → 代替再生 (規約3)
        msg = juce::CharPointer_UTF8 ("\xe2\x9a\xa0 \xe3\x81\x93\xe3\x81\xae\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3\xe3\x81\xaf\xe6\x9c\xac\xe7\x92\xb0\xe5\xa2\x83\xe3\x81\xa7\xe4\xbd\xbf\xe3\x81\x88\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93 \xe2\x80\x94 Phase Vocoder \xe3\x81\xa7\xe4\xbb\xa3\xe6\x9b\xbf\xe5\x86\x8d\xe7\x94\x9f\xe4\xb8\xad");
    else if (algo == 0 && dur != 0)   // Varispeed は長さが音程に従属 (§6.3)
        msg = juce::CharPointer_UTF8 ("Varispeed \xe3\x81\xa7\xe3\x81\xaf\xe9\x95\xb7\xe3\x81\x95\xe3\x81\x8c\xe9\x9f\xb3\xe7\xa8\x8b\xe3\x81\xab\xe5\xbe\x93\xe5\xb1\x9e\xef\xbc\x88Natural \xe6\x89\xb1\xe3\x81\x84\xef\xbc\x89");
    statusLabel.setText (msg, juce::dontSendNotification);

    // --- モードで有効でないコントロールをグレーアウト（規約#12: パラメータは常に存在, UIで無効化のみ）---
    auto enableComp = [] (juce::Component& c, bool on)
    { c.setEnabled (on); c.setAlpha (on ? 1.0f : 0.4f); };
    auto enableKnob = [&] (Knob* k, bool on)
    { if (k) { enableComp (k->slider, on); k->label.setAlpha (on ? 1.0f : 0.4f); } };
    auto enableCombo = [&] (Combo* c, bool on)
    { if (c) { enableComp (c->box, on); c->label.setAlpha (on ? 1.0f : 0.4f); } };

    const bool isReaper = (algo == 5);
    const bool isPV     = (algo == 2);
    const bool isVari   = (algo == 0);

    juce::ignoreUnused (isVari);
    enableKnob  (kStretch, dur == 2);                 // Stretch は Manual のみ
    enableCombo (cSync,    dur == 1);                 // Sync Length は Sync のみ
    enableComp  (phaseLockButton, isPV);              // Phase Lock は Phase Vocoder のみ

    const bool rc = isReaper && processor.isReaperAvailable();
    enableComp (rModeBox, rc); enableComp (rSubBox, rc);
    rModeLbl.setAlpha (rc ? 1.0f : 0.4f); rSubLbl.setAlpha (rc ? 1.0f : 0.4f);

    // REAPER Shifter は Sync 非対応 → Duration の "Sync"(id=2) を無効化。選択中なら Natural へ戻す。
    if (cDur)
    {
        cDur->box.setItemEnabled (2, ! isReaper);
        if (isReaper && dur == 1)
            if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (P::durationMode)))
                *cp = 0;   // Natural
    }

    // ピッチキャッシュ生成の進捗（生成中のみ表示）。ProgressBar は自前タイマで値を読み再描画する。
    const bool busy = processor.isCacheBusy();
    if (busy)
    {
        cacheProgressValue = (double) processor.getCacheProgress();
        cacheBar.setTextToDisplay (juce::String (juce::CharPointer_UTF8 ("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\x83\xe3\x82\xb7\xe3\x83\xa5\xe7\x94\x9f\xe6\x88\x90\xe4\xb8\xad "))   // "キャッシュ生成中 "
                                   + juce::String (juce::roundToInt (cacheProgressValue * 100.0)) + "%");
    }
    if (cacheBar.isVisible() != busy)
        cacheBar.setVisible (busy);
}

OtoMadSamplerEditor::Knob& OtoMadSamplerEditor::addKnob (const juce::String& paramID,
                                                         const juce::String& text,
                                                         juce::Slider::SliderStyle style)
{
    auto k = std::make_unique<Knob>();
    k->slider.setSliderStyle (style);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 15);
    k->slider.setTooltip (helpForParam (paramID));
    k->label.setText (text, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    k->attach = std::make_unique<SliderAttach> (processor.getAPVTS(), paramID, k->slider);
    knobs.push_back (std::move (k));
    return *knobs.back();
}

OtoMadSamplerEditor::Combo& OtoMadSamplerEditor::addCombo (const juce::String& paramID,
                                                           const juce::String& text,
                                                           const juce::StringArray& items)
{
    auto c = std::make_unique<Combo>();
    for (int i = 0; i < items.size(); ++i)
        c->box.addItem (items[i], i + 1);
    c->box.setTooltip (helpForParam (paramID));
    c->label.setText (text, juce::dontSendNotification);
    c->label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (c->box);
    addAndMakeVisible (c->label);
    c->attach = std::make_unique<ComboAttach> (processor.getAPVTS(), paramID, c->box);
    combos.push_back (std::move (c));
    return *combos.back();
}

//==============================================================================
void OtoMadSamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1b1f));   // 実描画は content 側（スケール対象）
}

void OtoMadSamplerEditor::paintContent (juce::Graphics& g)
{
    juce::Rectangle<int> full (0, 0, kBaseW, kBaseH);
    g.fillAll (juce::Colour (0xff1b1b1f));

    // 背景画像（設定した不透明度で全面にフィット。子コンポーネントの背面に描かれる）
    const auto& bg = processor.getBackgroundImage();
    if (bg.isValid())
    {
        g.setOpacity (processor.getBgOpacity());
        g.drawImage (bg, full.toFloat(), juce::RectanglePlacement::fillDestination);
        g.setOpacity (1.0f);
    }

    auto header = full.removeFromTop (82).reduced (8, 4);
    if (logo.isValid())
        g.drawImageWithin (logo, header.getX(), header.getY(), header.getWidth(), header.getHeight(),
                           juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid
                             | juce::RectanglePlacement::onlyReduceInSize);
    else
    {
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText ("OtoMadSampler", header, juce::Justification::centredLeft, false);
    }
}

//==============================================================================
void OtoMadSamplerEditor::applyUiScale (float pct)
{
    uiScale = juce::jlimit (0.25f, 4.0f, pct / 100.0f);
    processor.setUiScalePct (pct);

    int id = 3; float best = 1.0e9f;   // 既定は 100%
    for (int i = 0; i < kNumScales; ++i)
    { const float d = std::abs (pct - kScales[i]); if (d < best) { best = d; id = i + 1; } }
    if (scaleBox.getSelectedId() != id) scaleBox.setSelectedId (id, juce::dontSendNotification);

    setSize (juce::roundToInt (kBaseW * uiScale), juce::roundToInt (kBaseH * uiScale));  // → resized()
}

void OtoMadSamplerEditor::resized()
{
    content.setBounds (0, 0, kBaseW, kBaseH);
    content.setTransform (juce::AffineTransform::scale (uiScale));
    layoutContent();
}

void OtoMadSamplerEditor::layoutContent()
{
    auto place = [] (Knob* k, juce::Rectangle<int> a)
    {
        k->label.setBounds (a.removeFromTop (15));
        k->slider.setBounds (a);
    };
    auto placeCombo = [] (Combo* c, juce::Rectangle<int> a)
    {
        c->label.setBounds (a.removeFromTop (15));
        c->box.setBounds (a.removeFromTop (24));
    };

    // 設定オーバーレイは全面（基準座標）
    settingsOverlay.setBounds (0, 0, kBaseW, kBaseH);
    // 設定ボタン（右上隅）＋ 更新バナー（その左）＋ スケール選択（さらに左）
    settingsButton.setBounds (kBaseW - 34, 6, 28, 24);
    scaleBox.setBounds (kBaseW - 34 - 86, 6, 80, 24);   // "200%" が切れない幅
    updateButton.setBounds (kBaseW - 34 - 86 - 132, 6, 128, 24);
    // ヘッダ右側にキャッシュ進捗バー（ロゴは左寄せなので右側が空いている。ボタン群を避ける）
    cacheBar.setBounds (juce::Rectangle<int> (0, 0, kBaseW, 82).removeFromRight (300).reduced (14, 30).withTrimmedRight (110));

    juce::Rectangle<int> r (0, 0, kBaseW, kBaseH);
    r.removeFromTop (82);   // ロゴヘッダ

    auto waveArea = r.removeFromTop (124).reduced (8, 4);
    waveform.setBounds (waveArea);
    dropZone.setBounds (waveArea);

    auto trimRow = r.removeFromTop (40).reduced (8, 4);
    normalizeButton.setBounds (trimRow.removeFromRight (92).reduced (2, 4));
    detectButton.setBounds (trimRow.removeFromRight (66).reduced (2, 4));   // Root自動検出
    kStart->label.setBounds (trimRow.removeFromLeft (44));
    kStart->slider.setBounds (trimRow.removeFromLeft (trimRow.getWidth() / 2 - 4).withTrimmedRight (4));
    kEnd->label.setBounds (trimRow.removeFromLeft (40));
    kEnd->slider.setBounds (trimRow);

    // ADSR ブロック（トリム行の下）:
    //   左半分（波形幅の1/2）= ADSR表示 + その下に A D S R の4ノブ / 右半分 = Voices・Bend
    {
        const int dispH = 58;                                      // エンベロープ表示の高さ
        auto adsrArea = r.removeFromTop (150).reduced (8, 4);
        auto leftHalf = adsrArea.removeFromLeft (adsrArea.getWidth() / 2);

        adsrDisplay.setBounds (leftHalf.removeFromTop (dispH));    // 上: エンベロープ表示
        {                                                          // 下: A D S R
            auto krow = leftHalf; int cols = 4;
            for (auto* k : { kAtk, kDec, kSus, kRel })
                place (k, krow.removeFromLeft (krow.getWidth() / cols--).reduced (4));
        }

        adsrArea.removeFromTop (dispH);                            // 右半分も表示ぶん空けて高さを揃える
        {                                                          // 右: Voices・Bend（左のADSRと同サイズ）
            auto rrow = adsrArea; int cols = 4;
            place (kMaxV, rrow.removeFromLeft (rrow.getWidth() / cols--).reduced (4));
            place (kBend, rrow.removeFromLeft (rrow.getWidth() / cols--).reduced (4));
        }
    }

    keyboard.setBounds (r.removeFromBottom (68).reduced (8, 4));
    reaperModeLabel.setBounds (r.removeFromBottom (18).reduced (10, 0));
    statusLabel.setBounds (r.removeFromBottom (20).reduced (10, 0));

    auto grid = r.reduced (8, 4);
    const int rowH = grid.getHeight() / 3;   // 3行（row1 / row3 / row4）
    auto rowRect = [&] { return grid.removeFromTop (rowH); };
    auto cell = [] (juce::Rectangle<int>& row, int cols) { return row.removeFromLeft (row.getWidth() / cols).reduced (4); };

    // row1: Semi Cent Oct Root Gain
    {
        auto row = rowRect(); int c = 5;
        place (kSemi, cell (row, c)); place (kCents, cell (row, c - 1)); place (kOct, cell (row, c - 2));
        place (kRoot, cell (row, c - 3)); place (kGain, cell (row, c - 4));
    }
    // row3: Glide [Porta] + カーブGUI（ドラッグで編集, 残り幅いっぱい）
    {
        auto row = rowRect(); int c = 6;
        place (kPTime, cell (row, c));
        placeCombo (cPMode, cell (row, c - 1));
        curveDisplay.setBounds (row.reduced (4));
    }
    // row4: [Algo][Duration][Sync] Stretch [R.Mode combo][R.Sub combo] [PhaseLock]
    {
        auto row = rowRect(); int c = 8;
        placeCombo (cAlgo, cell (row, c)); placeCombo (cDur, cell (row, c - 1)); placeCombo (cSync, cell (row, c - 2));
        place (kStretch, cell (row, c - 3));
        auto placeRawCombo = [] (juce::Label& lbl, juce::ComboBox& box, juce::Rectangle<int> a)
        {
            lbl.setBounds (a.removeFromTop (15));
            box.setBounds (a.removeFromTop (24));
        };
        // FORMANT を外したぶん、以降を1つ詰める（空きセルを残さない）
        placeRawCombo (rModeLbl, rModeBox, cell (row, c - 4));
        placeRawCombo (rSubLbl,  rSubBox,  cell (row, c - 5));
        phaseLockButton.setBounds (row.reduced (4).withTrimmedTop (16));
    }
}

//==============================================================================
// 設定オーバーレイ
SettingsOverlay::SettingsOverlay (OtoMadSamplerProcessor& p) : processor (p)
{
    auto lbl = [this] (juce::Label& l, const char* utf8)
    {
        l.setText (juce::CharPointer_UTF8 (utf8), juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (l);
    };
    lbl (title,      "\xe8\xa8\xad\xe5\xae\x9a");                                    // 設定
    lbl (opacityLbl, "\xe8\x83\x8c\xe6\x99\xaf\xe3\x81\xae\xe4\xb8\x8d\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6");   // 背景の不透明度
    lbl (colourLbl,  "\xe3\x83\xa1\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xab\xe3\x83\xa9\xe3\x83\xbc (R/G/B)");        // メインカラー (R/G/B)
    title.setFont (juce::FontOptions (18.0f, juce::Font::bold));

    chooseBg.setButtonText (juce::CharPointer_UTF8 ("\xe8\x83\x8c\xe6\x99\xaf\xe7\x94\xbb\xe5\x83\x8f\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e..."));  // 背景画像を選択...
    clearBg.setButtonText  (juce::CharPointer_UTF8 ("\xe8\x83\x8c\xe6\x99\xaf\xe3\x82\x92\xe3\x82\xaf\xe3\x83\xaa\xe3\x82\xa2"));                // 背景をクリア
    saveDefault.setButtonText (juce::CharPointer_UTF8 ("\xe3\x81\x93\xe3\x81\xae\xe5\xa4\x96\xe8\xa6\xb3\xe3\x82\x92\xe5\x85\xa8\xe4\xbd\x93\xe3\x81\xae\xe6\x97\xa2\xe5\xae\x9a\xe3\x81\xab\xe3\x81\x99\xe3\x82\x8b"));   // この外観を全体の既定にする
    closeBtn.setButtonText (juce::CharPointer_UTF8 ("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));                                                   // 閉じる
    addAndMakeVisible (chooseBg);
    addAndMakeVisible (clearBg);
    addAndMakeVisible (saveDefault);
    addAndMakeVisible (closeBtn);

    opacity.setSliderStyle (juce::Slider::LinearHorizontal);
    opacity.setRange (0.0, 1.0, 0.01);
    opacity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 18);
    addAndMakeVisible (opacity);

    for (auto* s : { &rSl, &gSl, &bSl })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setRange (0.0, 255.0, 1.0);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
        addAndMakeVisible (*s);
        s->onValueChange = [this] { if (! updating) pushColour(); };
    }

    opacity.onValueChange = [this]
    {
        if (updating) return;
        processor.setBgOpacity ((float) opacity.getValue());
        if (onChanged) onChanged();
    };

    chooseBg.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Select a background image",
                                                       juce::File{}, "*.png;*.jpg;*.jpeg;*.gif;*.bmp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile())
            {
                processor.setBackgroundImageFromFile (f);
                if (onChanged) onChanged();
            }
        });
    };
    clearBg.onClick  = [this] { processor.clearBackgroundImage(); if (onChanged) onChanged(); };
    saveDefault.onClick = [this]
    {
        processor.saveAppearanceAsDefault();
        saveDefault.setButtonText (juce::CharPointer_UTF8 ("\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f \xe2\x9c\x93"));   // 保存しました ✓
    };
    closeBtn.onClick = [this] { setVisible (false); };
}

void SettingsOverlay::pushColour()
{
    const juce::uint8 r = (juce::uint8) juce::roundToInt (rSl.getValue());
    const juce::uint8 g = (juce::uint8) juce::roundToInt (gSl.getValue());
    const juce::uint8 b = (juce::uint8) juce::roundToInt (bSl.getValue());
    processor.setMainColourARGB (juce::Colour (r, g, b).getARGB());
    if (onChanged) onChanged();
    repaint();   // スウォッチ更新
}

void SettingsOverlay::refreshFromProcessor()
{
    const juce::Colour c (processor.getMainColourARGB());
    updating = true;
    rSl.setValue (c.getRed(),   juce::dontSendNotification);
    gSl.setValue (c.getGreen(), juce::dontSendNotification);
    bSl.setValue (c.getBlue(),  juce::dontSendNotification);
    opacity.setValue (processor.getBgOpacity(), juce::dontSendNotification);
    updating = false;
    saveDefault.setButtonText (juce::CharPointer_UTF8 ("\xe3\x81\x93\xe3\x81\xae\xe5\xa4\x96\xe8\xa6\xb3\xe3\x82\x92\xe5\x85\xa8\xe4\xbd\x93\xe3\x81\xae\xe6\x97\xa2\xe5\xae\x9a\xe3\x81\xab\xe3\x81\x99\xe3\x82\x8b"));   // ラベルを戻す
    repaint();
}

void SettingsOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.72f));   // スクリム

    auto panel = getLocalBounds().withSizeKeepingCentre (420, 356).toFloat();
    g.setColour (juce::Colour (0xff26262c));
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawRoundedRectangle (panel, 8.0f, 1.0f);

    // メインカラーのスウォッチ（colourLbl の右）
    const juce::uint8 r = (juce::uint8) juce::roundToInt (rSl.getValue());
    const juce::uint8 gg = (juce::uint8) juce::roundToInt (gSl.getValue());
    const juce::uint8 b = (juce::uint8) juce::roundToInt (bSl.getValue());
    g.setColour (juce::Colour (r, gg, b));
    g.fillRoundedRectangle (swatchBounds.toFloat(), 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.3f));
    g.drawRoundedRectangle (swatchBounds.toFloat(), 3.0f, 1.0f);
}

void SettingsOverlay::resized()
{
    auto panel = getLocalBounds().withSizeKeepingCentre (420, 356).reduced (16);
    title.setBounds (panel.removeFromTop (28));
    panel.removeFromTop (6);

    auto row = [&] (int h) { auto rr = panel.removeFromTop (h); panel.removeFromTop (6); return rr; };

    // 背景画像ボタン
    {
        auto rr = row (26);
        chooseBg.setBounds (rr.removeFromLeft (rr.getWidth() - 96).reduced (0, 1));
        clearBg.setBounds  (rr.reduced (0, 1));
    }
    // 不透明度
    {
        auto rr = row (24);
        opacityLbl.setBounds (rr.removeFromLeft (120));
        opacity.setBounds (rr);
    }
    panel.removeFromTop (4);
    // メインカラー見出し＋スウォッチ
    {
        auto rr = row (22);
        colourLbl.setBounds (rr.removeFromLeft (150));
        swatchBounds = rr.removeFromRight (40).reduced (2);
    }
    rSl.setBounds (row (24));
    gSl.setBounds (row (24));
    bSl.setBounds (row (24));

    panel.removeFromTop (6);
    saveDefault.setBounds (row (28));
    panel.removeFromTop (2);
    closeBtn.setBounds (panel.removeFromTop (26).withSizeKeepingCentre (120, 26));
}
