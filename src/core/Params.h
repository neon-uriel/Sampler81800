#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace otomad::params
{

// パラメータID（DESIGN.md §3.7）。Phase 1 で使う分のみ定義。
// 規約10/12: 個数・レンジ・選択肢はホストによって変えない。以降のフェーズで追加はするが縮小はしない。
inline constexpr const char* pitchSemi     = "pitchSemi";
inline constexpr const char* pitchCents    = "pitchCents";
inline constexpr const char* octave        = "octave";
inline constexpr const char* rootKey       = "rootKey";
inline constexpr const char* interpQuality = "interpQuality";
inline constexpr const char* attack        = "attack";
inline constexpr const char* decay         = "decay";
inline constexpr const char* sustain       = "sustain";
inline constexpr const char* release       = "release";
inline constexpr const char* sampleStart   = "sampleStart";
inline constexpr const char* sampleEnd     = "sampleEnd";
inline constexpr const char* snapZeroCross = "snapZeroCross";
inline constexpr const char* gain          = "gain";
// Phase 2
inline constexpr const char* portaMode     = "portaMode";
inline constexpr const char* portaShape    = "portaShape";
inline constexpr const char* portaTime     = "portaTime";
inline constexpr const char* portaCurve    = "portaCurve";
inline constexpr const char* glideGroupMs  = "glideGroupMs";
inline constexpr const char* polyMode      = "polyMode";
inline constexpr const char* maxVoices     = "maxVoices";
inline constexpr const char* bendRange     = "bendRange";
// Phase 3
inline constexpr const char* algorithm     = "algorithm";
inline constexpr const char* durationMode  = "durationMode";
inline constexpr const char* syncLength    = "syncLength";
inline constexpr const char* stretchAmount = "stretchAmount";
inline constexpr const char* formant       = "formant";
inline constexpr const char* reaperMode    = "reaperMode";
inline constexpr const char* reaperSubMode = "reaperSubMode";
inline constexpr const char* phaseLock     = "phaseLock";
inline constexpr const char* elastiqueMode = "elastiqueMode";
inline constexpr const char* cacheFallback = "cacheFallback";
// ビブラート（発音から delay 経過後、fade で depth へ到達）
inline constexpr const char* vibDepth      = "vibDepth";
inline constexpr const char* vibRate       = "vibRate";
inline constexpr const char* vibDelay      = "vibDelay";
inline constexpr const char* vibFade       = "vibFade";

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto msRange = [] { NormalisableRange<float> r (0.0f, 5000.0f, 1.0f, 0.4f); return r; };

    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { pitchSemi, 1 },  "Pitch (semi)", -48, 48, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { pitchCents, 1 }, "Pitch (cent)",
                                                       NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { octave, 1 },     "Octave", -4, 4, 0));
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { rootKey, 1 },    "Root Key", 0, 127, 60));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { interpQuality, 1 }, "Interp",
                                                        StringArray { "Linear", "Hermite" }, 1));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { attack, 1 },  "Attack",  msRange(), 4.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { decay, 1 },   "Decay",   msRange(), 100.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { sustain, 1 }, "Sustain",
                                                       NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { release, 1 }, "Release", msRange(), 9.0f));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { sampleStart, 1 }, "Sample Start",
                                                       NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { sampleEnd, 1 },   "Sample End",
                                                       NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { snapZeroCross, 1 }, "Snap Zero-Cross", true));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { gain, 1 }, "Gain",
                                                       NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f));

    // Phase 2: ポルタメント / ポリ / ベンド
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { portaMode, 1 }, "Porta Mode",
                                                        StringArray { "Off", "Legato", "Always" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { portaShape, 1 }, "Porta Shape",
                                                        StringArray { "Time", "Analog" }, 0));
    // Glide(ポルタメント時間)はエンベロープ(最大5s)より狭い専用レンジ。0..200ms・skewで低域を細かく。
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { portaTime, 1 }, "Porta Time",
                                                       NormalisableRange<float> (0.0f, 200.0f, 0.1f, 0.5f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { portaCurve, 1 }, "Porta Curve",
                                                       NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { glideGroupMs, 1 }, "Glide Group",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 30.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { polyMode, 1 }, "Poly Mode",
                                                        StringArray { "Mono", "Poly" }, 1));
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { maxVoices, 1 }, "Max Voices", 1, 16, 8));
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { bendRange, 1 }, "Bend Range", 0, 48, 2));

    // Phase 3: アルゴリズム / 長さ制御 / フォルマント（規約10/12: 選択肢は6個で固定, 縮小しない）
    // 既定は Varispeed。テープ早回しそのもので、音MADで最も素直に鳴る（加工臭が出ない）。
    // Varispeed は timeRatio を無視して長さがピッチに従属するので、durationMode の既定が
    // Manual でも初期状態の挙動は変わらない（長さを保ちたい人は WSOLA 以降を選ぶ）。
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { algorithm, 1 }, "Algorithm",
                    StringArray { "Varispeed", "WSOLA", "Phase Vocoder", "Granular", "Stretch Library", "REAPER Shifter" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { durationMode, 1 }, "Duration",
                    StringArray { "Natural", "Sync", "Manual" }, 2));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { syncLength, 1 }, "Sync Length",
                    StringArray { "1/4", "1/2", "1", "2", "4" }, 2));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { stretchAmount, 1 }, "Stretch",
                    NormalisableRange<float> (0.25f, 4.0f, 0.01f, 0.5f), 1.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { formant, 1 }, "Formant",
                    NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    // REAPER ピッチモードを2段階で選ぶ: Mode(トップレベル) と Sub(その中のサブモード)。
    // 選択時にその組合せのレイテンシを実測してエンベロープ遅延/PDCが追従する。
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { reaperMode, 1 },    "REAPER Mode",    0, 127,   0));
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { reaperSubMode, 1 }, "REAPER SubMode", 0, 32767, 0));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { phaseLock, 1 }, "Phase Lock", true));
    // élastique 直読み（REAPER外）のアルゴリズム。REAPER のモード番号とは別系統なので独立させる。
    // 規約12: REAPER 上でも非REAPERでも常に存在させ、UIでグレーアウトするだけにする。
    // 既定は Elastique Pro（多声OK）。Soloist は単声専用で、和音だと片方の声部が消え、
    // 下方向シフトでレベルが pitch² 程度に落ちる（実測）。
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { elastiqueMode, 1 }, "elastique Mode",
                    StringArray { "Elastique Pro", "Soloist" }, 0));
    // キャッシュを外した音を鳴らすエンジン。index は algorithm の 0..2 と一致させる（そのまま渡すため）。
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { cacheFallback, 1 }, "Cache Fallback",
                    StringArray { "Varispeed", "WSOLA", "Phase Vocoder" }, 0));

    // ビブラート。Depth=0 で無効。Delay は効き始めるまで、Fade は最大振幅に達するまでの時間。
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { vibDepth, 1 }, "Vib Depth",
                    NormalisableRange<float> (0.0f, 200.0f, 0.1f, 0.5f), 0.0f));      // セント
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { vibRate, 1 },  "Vib Rate",
                    NormalisableRange<float> (0.1f, 20.0f, 0.01f, 0.5f), 5.0f));      // Hz
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { vibDelay, 1 }, "Vib Delay",
                    NormalisableRange<float> (0.0f, 2000.0f, 1.0f, 0.4f), 0.0f));     // ms
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { vibFade, 1 },  "Vib Fade",
                    NormalisableRange<float> (0.0f, 2000.0f, 1.0f, 0.4f), 200.0f));   // ms

    return layout;
}

} // namespace otomad::params
