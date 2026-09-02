#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

//==============================================================================
/**
    Web UI 版エディタ（feature/web-ui）。

    JUCE 8 の WebBrowserComponent + WebSliderRelay / WebComboBoxRelay / WebToggleButtonRelay を使い、
    HTML/CSS/JS で組んだ UI を APVTS と双方向バインドする。アセットは BinaryData に埋め込み、
    ResourceProvider でローカル配信する（外部ネットワークには一切アクセスしない）。

    パラメータ以外（波形データ・D&D・鍵盤・状態表示）は withNativeFunction / emitEvent で橋渡しする。
*/
class OtoMadSamplerWebEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit OtoMadSamplerWebEditor (OtoMadSamplerProcessor&);
    ~OtoMadSamplerWebEditor() override;

    // WebView2 ランタイムが無い環境では真っ白な窓になってしまうので、
    // 事前に判定してネイティブ版エディタへフォールバックできるようにする（規約#15の精神）。
    static bool isWebViewAvailable();

    void resized() override;

private:
    void timerCallback() override;

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url) const;

    // JS から呼ばれるネイティブ関数
    void nfLoadSample   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfGetWaveform  (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfNormalize    (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfDetectRoot   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfNote         (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfReaperModes  (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSetReaperMode(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfOpenReleases (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfResetParam   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfGetAppearance(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSetAppearance(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSetBgImage   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSaveAppearanceDefault (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfGetSamples   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSelectSample (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfRemoveSample (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfGetElastique (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSetElastique (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfBrowseElastique (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSpaceToHost   (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfGetFfmpeg     (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfSetFfmpeg     (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfBrowseFfmpeg  (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfFlatten       (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfRevertFlatten (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfPitchContour  (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfFlattenState  (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfParamHelp     (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    void nfReady         (const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);

    // スライダ/コンボ/トグルのリレー＋アタッチメント
    template <typename RelayT, typename AttachT>
    struct Bound
    {
        std::unique_ptr<RelayT>  relay;
        std::unique_ptr<AttachT> attach;
    };
    using SliderBound = Bound<juce::WebSliderRelay,       juce::WebSliderParameterAttachment>;
    using ComboBound  = Bound<juce::WebComboBoxRelay,     juce::WebComboBoxParameterAttachment>;
    using ToggleBound = Bound<juce::WebToggleButtonRelay, juce::WebToggleButtonParameterAttachment>;

    OtoMadSamplerProcessor& processor;
    std::vector<std::unique_ptr<SliderBound>> sliders;
    std::vector<std::unique_ptr<ComboBound>>  combos;
    std::vector<std::unique_ptr<ToggleBound>> toggles;
    std::unique_ptr<juce::WebBrowserComponent> web;
    std::unique_ptr<juce::FileChooser>         dllChooser;   // élastique DLL 選択（非同期なので保持）
    std::unique_ptr<juce::FileChooser>         exeChooser;   // ffmpeg 実行ファイル選択

    int lastSampleVersion = -1;   // 波形の再送判定
    int lastAppearanceVersion = -1;   // 外観（色/背景）の再送判定
    int lastReaperMode = -1, lastReaperSubMode = -1;   // モード変更→エンジン再設定の検知
    int lastCacheCount = -1;                          // ready/pending の変化検知（%が同じでも出す）
    juce::String lastCacheDebug;                      // 診断文字列の変化検知（世代だけ動く場合がある）
    // status イベントの早期棄却用（10Hz で毎回 JSON 化しないため）
    bool lastBusy = false, lastFallback = false, lastUpdateAvail = false;
    int  lastProgPct = -1, lastReaperKey = -1;
    juce::String lastStatusJson;  // 同じ内容の連投を避ける
    juce::String lastLoadErrorSeen;   // 読み込み失敗メッセージの再送判定
    juce::String lastShifterText;     // SHIFTER 欄の文言の再送判定
    juce::String lastKeyState, lastKeyHeld;   // 鍵盤表示（keys イベント）の再送判定

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OtoMadSamplerWebEditor)
};
