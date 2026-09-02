#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "SampleBuffer.h"
#include "Voice.h"
#include "PortamentoGenerator.h"
#include "pitch/EngineResources.h"

namespace otomad
{
namespace host { class ReaperApi; }

//==============================================================================
/**
    ボイス割り当てとポルタメント/ポリグライドの取りまとめ。DESIGN.md §3.4 / §3.5。
    Mono（last-note）/ Poly（同一ノート優先→空き→最古を奪う）。
    ポリグライドは「即時発音＋グループ内オリジン再マッチング」（§3.4）。
*/
class VoiceManager
{
public:
    static constexpr int kMaxVoices = 16;

    enum class PortaMode { Off = 0, Legato = 1, Always = 2 };

    void prepare (double sampleRate, int maxBlock, int numChannels, host::ReaperApi* reaperApi);

    void setVoiceParams (const Voice::Params& p) noexcept { baseParams = p; }
    void setAdsr (float a, float d, float s, float r) noexcept { aA = a; aD = d; aS = s; aR = r; }
    void setPortamento (PortaMode mode, PortamentoGenerator::Shape shape,
                        float timeMs, float curve, float groupMs) noexcept;
    void setPoly (bool p, int maxV) noexcept { poly = p; maxVoices = juce::jlimit (1, kMaxVoices, maxV); }
    void setPitchBendSemi (float s) noexcept { bendSemi = s; }
    void setEngineControl (const Voice::EngineControl& c) noexcept { engineControl = c; }
    bool isFallbackActive() const noexcept { return voices[0].isFallbackActive(); }
    int  getCurrentLatency() const noexcept { return voices[0].getReportedLatency (engineControl.algorithm); }
    // 指定アルゴリズムの申告レイテンシ。engineControl は processBlock でしか更新されないので、
    // prepareToPlay の時点では前回の値のまま。APVTS を正として問い合わせたいとき用。
    int  getLatencyFor (int algorithm) const noexcept { return voices[0].getReportedLatency (algorithm); }
    void reconfigureReaper (int mode, int submode) { for (auto& v : voices) v.reconfigureReaper (mode, submode); }

    // GUI表示用（voice0 から）
    const std::string& getReaperModeName() const noexcept    { return voices[0].getReaperModeName(); }
    const std::string& getReaperSubModeName() const noexcept { return voices[0].getReaperSubModeName(); }
    int  getReaperLatency() const noexcept      { return voices[0].getReaperLatency(); }
    int  getReaperModeCount() const noexcept    { return voices[0].getReaperModeCount(); }
    int  getReaperSubModeCount() const noexcept { return voices[0].getReaperSubModeCount(); }

    void noteOn (int note, float velocity, const SampleBuffer* sample,
                 float sampleStart01, float sampleEnd01, bool snapZeroCross,
                 Voice::NoteOptions opts = {});
    void noteOff (int note);
    void allNotesOff();

    void render (float* const* out, int numChannels, int n);

private:
    void snapshotGlidePool();
    void rematchGroupOrigins();
    std::uint64_t groupWindowSamples() const noexcept;

    std::array<Voice, kMaxVoices>          voices;
    std::array<std::uint64_t, kMaxVoices>  voiceOnTime { };

    bool  poly = false;
    int   maxVoices = 8;
    PortaMode portaMode = PortaMode::Legato;
    PortamentoGenerator::Shape portaShape = PortamentoGenerator::Shape::Time;
    float portaTimeMs = 80.0f;
    float portaCurve  = 0.0f;
    float groupMs     = 30.0f;
    float bendSemi    = 0.0f;

    Voice::Params baseParams;
    Voice::EngineControl engineControl;
    EngineResources resources;
    float aA = 0.001f, aD = 0.1f, aS = 1.0f, aR = 0.05f;

    double        sampleRate = 44100.0;
    std::uint64_t sampleCounter = 0;
    std::uint64_t lastNoteOnTime = 0;
    int           monoNote = -1;

    // Always モード用: 音が切れても直前のピッチから滑らせるための保持（reset/allNotesOff でのみクリア）。
    float                         lastMonoPitch = -1.0f;   // Mono: 直前ノートのピッチ（-1=無し）
    std::vector<float>            lastPitches;              // Poly: 直前グループのピッチ群
    std::vector<float>            glidePool;
    struct GroupEntry { int slot; int note; };
    std::vector<GroupEntry>       currentGroup;
};

} // namespace otomad
