#pragma once

#include <array>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

#include "SampleBuffer.h"
#include "SourceReader.h"
#include "PortamentoGenerator.h"
#include "VibratoLfo.h"
#include "pitch/IPitchEngine.h"
#include "pitch/VarispeedEngine.h"
#include "pitch/WsolaEngine.h"
#include "pitch/PhaseVocoderEngine.h"
#include "pitch/GranularEngine.h"
#include "pitch/StretchLibEngine.h"
#include "pitch/ReaperPitchShiftEngine.h"

namespace otomad
{
namespace host { class ReaperApi; }

//==============================================================================
/**
    ボイス。DESIGN.md §3.6。エンジンをボイス所有（規約9）、timeRatio(durationMode)。
    レイテンシは**アクティブエンジンの実レイテンシをそのまま採用**する（固定整列は廃止）。
    Varispeed=0（生演奏で低遅延）、WSOLA/PV=frame/fft、REAPER=élastique実測。
    テールは getTailSamples() 全量をドレインする（切らない, §3.6）。
*/
class Voice
{
public:
    struct Params
    {
        float pitchSemi     = 0.0f;
        float pitchCents    = 0.0f;
        int   rootKey       = 60;
        float gainLin       = 1.0f;
        float pitchBendSemi = 0.0f;
        VarispeedEngine::Quality quality = VarispeedEngine::Quality::Hermite;

        // ビブラート（発音から delay 経過後、fade をかけて depth に到達する）
        float vibDepthCents = 0.0f;   // 振幅（セント）。0 で無効
        float vibRateHz     = 5.0f;
        float vibDelayMs    = 0.0f;   // 発音から効き始めるまで
        float vibFadeMs     = 0.0f;   // 最大振幅に達するまで（ADSR の A 相当）
    };

    struct EngineControl
    {
        int   algorithm   = 0;    // 0 Varispeed / 1 WSOLA / 2 PhaseVocoder /（3-5は未実装→PVへ）
        int   durationMode = 0;   // 0 Natural / 1 Sync / 2 Manual
        float stretchAmount = 1.0f;
        float syncBeats     = 1.0f;
        double hostBpm      = 120.0;
        bool  hostBpmValid  = false;
        float formantSemi   = 0.0f;
        bool  phaseLock     = true;
        int   reaperMode    = 0;
        int   reaperSubMode = 0;
    };

    void prepare (double sampleRate, int maxBlock, int numChannels,
                  EngineResources& resources, host::ReaperApi* reaperApi);
    void setParams (const Params& p) noexcept { params = p; }
    void setAdsr (float attackSec, float decaySec, float sustain, float releaseSec) noexcept;
    void setPortamentoConfig (PortamentoGenerator::Shape shape, float timeMs, float curve) noexcept;
    void setEngineControl (const EngineControl& c) noexcept;   // fallback判定込み
    bool isFallbackActive() const noexcept { return fallbackActive; }

    struct NoteOptions
    {
        int   forcedAlgorithm = -1;     // このノートだけ使うエンジン。-1 で EngineControl::algorithm
        float prePitchedSemi  = 0.0f;   // sample に焼き込み済みのシフト量（二重シフト防止）
        int   alignLatency    = 0;      // 出力を揃える遅延。固有遅延との差分を先頭無音にする（申告値を渡す）
    };

    void noteOn (const SampleBuffer* sample, int midiNote, float velocity,
                 float sampleStart01, float sampleEnd01, bool snapZeroCross,
                 bool glide, float originNote, NoteOptions opts = {});
    void glideTo (int midiNote) noexcept;
    void setGlideOrigin (float originNote) noexcept;
    void requestSteal (const SampleBuffer* sample, int midiNote, float velocity,
                       float sampleStart01, float sampleEnd01, bool snapZeroCross,
                       bool glide, float originNote, NoteOptions opts = {});
    void noteOff() noexcept;
    void stop() noexcept;

    bool  isActive() const noexcept    { return active; }
    bool  isReleasing() const noexcept { return active && released; }
    int   getNote() const noexcept     { return midiNote; }
    float currentPitchNote() const noexcept { return porta.current(); }

    // 指定アルゴリズムを選んだ場合に報告すべきレイテンシ（pickEngine と整合）。
    int   getReportedLatency (int algorithm) const noexcept;

    // REAPERシフタのモード再設定（非RT・suspend下でメッセージスレッドから呼ぶ）。
    void  reconfigureReaper (int mode, int submode)
    { reaper.setMode (mode); reaper.setSubMode (submode); reaper.reconfigure(); }

    // GUI表示用（メッセージスレッドからのみ）
    const std::string& getReaperModeName() const noexcept    { return reaper.getModeName(); }
    const std::string& getReaperSubModeName() const noexcept { return reaper.getSubModeName(); }
    int  getReaperModeCount() const noexcept    { return reaper.getModeCount(); }
    int  getReaperSubModeCount() const noexcept { return reaper.getSubModeCount(); }
    int  getReaperLatency() const noexcept      { return reaper.getIntrinsicLatency(); }

    void render (float* const* out, int numChannels, int n) noexcept;

private:
    struct Pending
    {
        const SampleBuffer* sample = nullptr;
        int note = 0; float vel = 0.0f;
        float s01 = 0.0f, e01 = 1.0f; bool snap = false;
        bool glide = false; float originNote = 0.0f;
        NoteOptions opts;
    };
    void startNote (const Pending& p) noexcept;
    double resolveTimeRatio() noexcept;
    IPitchEngine* pickEngine (int algorithm) noexcept;
    IPitchEngine* engineOrFallback (int algorithm) noexcept;   // pickEngine と同じ対応、fallbackActive は触らない

    bool   active = false;
    bool   released = false;
    int    forcedAlgorithm = -1;      // このノートだけ使うエンジン（-1 = control.algorithm に従う）
    float  prePitchedSemi = 0.0f;     // sample が既にシフト済みの半音
    int    startDelay = 0;            // 整列用の前置無音の残り（NoteOptions::alignLatency 参照）
    int    midiNote = -1;
    float  velocity = 1.0f;
    double srcPos = 0.0;
    bool   sourceReleaseTriggered = false;
    // エンベロープをエンジンのレイテンシ分だけ遅らせて音と揃える（高レイテンシ系で短音が消えるのを防ぐ）
    int    pendingOn  = -1;   // adsr.noteOn() 発火までの残りサンプル（-1=発火済/無し）
    int    pendingOff = -1;   // adsr.noteOff() 発火までの残りサンプル
    long   drainCounter = 0;

    // ビブラートはボイス固有の状態（位相・発音からの経過）。規約9に従い共有しない。
    VibratoLfo vibrato;
    double sampleRate = 44100.0;

    SourceReader        reader;
    VarispeedEngine        varispeed;
    WsolaEngine            wsola;
    PhaseVocoderEngine     phaseVocoder;
    GranularEngine         granular;
    StretchLibEngine       stretchLib;
    ReaperPitchShiftEngine reaper;
    IPitchEngine*          activeEngine = &varispeed;
    bool                   fallbackActive = false;

    // エンジンを1つ追加すると触る場所が「生成」「prepare」「algorithm→実体の対応」と
    // 分散しがちで、実際に prepare の登録漏れ（＝未初期化のまま再生してクラッシュ）と
    // レイテンシ報告漏れを起こした。**対応表と一覧をここに集約する。**
    //
    // - lifecycle（prepare/reset）は allEngines を回す
    // - algorithm → 実体 は engineForAlgorithm() だけが知る
    //   （pickEngine と getReportedLatency の両方がこれを使う）
    std::array<IPitchEngine*, 6> allEngines
        { &varispeed, &wsola, &phaseVocoder, &granular, &stretchLib, &reaper };

    // algorithm 番号に対応するエンジン。未実装／使用不可なら nullptr（呼び出し側が PV へ）。
    IPitchEngine* engineForAlgorithm (int algorithm) noexcept;

    juce::ADSR          adsr;
    juce::ADSR::Parameters adsrParams;
    PortamentoGenerator porta;
    Params              params;
    EngineControl       control;
    juce::SmoothedValue<double> timeRatioSmooth;
    // ビブラート深さは自動化されうるので平滑化する（規約#14: 段差はピッチのジッパーノイズになる）。
    // Rate は位相の増分にしか効かず段差が可聴でないため平滑化しない。
    juce::SmoothedValue<float>  vibDepthSmooth;

    bool    stealing = false;
    float   stealGain = 1.0f;
    float   stealStep = 0.0f;
    Pending pending;

    std::vector<std::vector<float>> scratch;
    std::vector<float>              noteBuf, ratioBuf;
    std::vector<float*>             scratchPtrs;
    int  preparedChannels = 0;
    int  preparedBlock    = 0;
};

} // namespace otomad
