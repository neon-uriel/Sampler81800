#include "Voice.h"
#include "host/ReaperApi.h"

#include <algorithm>
#include <cmath>

namespace otomad
{

void Voice::prepare (double sr, int maxBlock, int numChannels,
                     EngineResources& resources, host::ReaperApi* reaperApi)
{
    sampleRate       = sr;
    preparedChannels = juce::jmax (1, numChannels);
    preparedBlock    = juce::jmax (1, maxBlock);

    scratch.assign ((std::size_t) preparedChannels,
                    std::vector<float> ((std::size_t) preparedBlock, 0.0f));
    scratchPtrs.assign ((std::size_t) preparedChannels, nullptr);
    noteBuf.assign ((std::size_t) preparedBlock, 0.0f);
    ratioBuf.assign ((std::size_t) preparedBlock, 1.0f);

    const PitchEngineContext ctx { sr, preparedBlock, preparedChannels };
    reaper.setReaperApi (reaperApi);   // prepare より前（非REAPERなら isAvailable()==false のまま）

    // **個別に呼ばない。** 一覧を回すことで、エンジンを足したときの prepare 登録漏れを防ぐ。
    // 実際 Granular / StretchLib を追加したとき漏らして、選ぶと未初期化のまま再生して落ちた。
    for (auto* e : allEngines)
        e->prepare (ctx, resources);

    activeEngine = &varispeed;

    adsr.setSampleRate (sr);
    porta.setSampleRate (sr);
    timeRatioSmooth.reset (sr, 0.02);
    timeRatioSmooth.setCurrentAndTargetValue (1.0);
    vibDepthSmooth.reset (sr, 0.02);
    vibDepthSmooth.setCurrentAndTargetValue (0.0f);
    vibrato.prepare (sr);
    active = false;
    stealing = false;
    stealGain = 1.0f;
}

void Voice::setAdsr (float a, float d, float s, float r) noexcept
{
    adsrParams.attack  = juce::jmax (0.0f, a);
    adsrParams.decay   = juce::jmax (0.0f, d);
    adsrParams.sustain = juce::jlimit (0.0f, 1.0f, s);
    adsrParams.release = juce::jmax (0.001f, r);
}

void Voice::setPortamentoConfig (PortamentoGenerator::Shape shape, float timeMs, float curve) noexcept
{
    porta.setShape (shape);
    porta.setTime (timeMs);
    porta.setCurve (curve);
}

// algorithm 番号 → エンジン実体。**対応を知っているのはここだけ**にする。
// pickEngine と getReportedLatency の両方がこれを使うので、片方だけ更新して
// 食い違う（＝報告レイテンシと実体がズレる）事故が起きない。
IPitchEngine* Voice::engineForAlgorithm (int algorithm) noexcept
{
    switch (algorithm)
    {
        case 0: return &varispeed;
        case 1: return &wsola;
        case 2: return &phaseVocoder;
        // Granular / StretchLib は v0.4.2 時点で**プラグイン上だと無音になる**問題があるため
        // 一時的に無効化する（nullptr → 呼び出し側が Phase Vocoder へフォールバック, 規約15）。
        // **選択肢自体は消さない。** AudioParameterChoice は正規化値で保存されるので、
        // リストから抜くと REAPER Shifter が index 5→3 にずれ、既存プロジェクトの
        // アルゴリズム選択が別物に化ける（規約12: 選択肢は縮小しない）。
        // 原因が分かったらここを元に戻すだけでよい。エンジン本体とテストは残してある。
        case 3: return nullptr;   // Granular（無効化中）
        case 4: return nullptr;   // Stretch Library（無効化中）
        case 5: return reaper.isAvailable() ? &reaper : nullptr;   // REAPER上でのみ実動作
        default: return nullptr;
    }
}

IPitchEngine* Voice::pickEngine (int algorithm) noexcept
{
    auto* e = engineForAlgorithm (algorithm);
    fallbackActive = (e == nullptr);
    return e != nullptr ? e : &phaseVocoder;   // 規約15: 無音を返さず代替(PV)へ
}

IPitchEngine* Voice::engineOrFallback (int algorithm) noexcept
{
    auto* e = engineForAlgorithm (algorithm);
    return e != nullptr ? e : &phaseVocoder;
}

void Voice::setEngineControl (const EngineControl& c) noexcept
{
    control = c;
    reaper.setMode (c.reaperMode);
    reaper.setSubMode (c.reaperSubMode);
    // fallbackActive はアルゴリズム選択に対して判定し、ノート単位の強制エンジンはそれより優先
    auto* selected = pickEngine (c.algorithm);
    activeEngine = forcedAlgorithm >= 0 ? engineOrFallback (forcedAlgorithm) : selected;
    phaseVocoder.setPhaseLock (c.phaseLock);
}

double Voice::resolveTimeRatio() noexcept
{
    if (activeEngine == nullptr || ! activeEngine->preservesDuration())
        return 1.0;   // Varispeed

    switch (control.durationMode)
    {
        case 2: // Manual
            return control.stretchAmount > 0.0f ? 1.0 / (double) control.stretchAmount : 1.0;
        case 1: // Sync
        {
            if (! control.hostBpmValid) return 1.0;
            const double targetSec = (double) control.syncBeats * 60.0 / control.hostBpm;
            const double srcSec    = reader.getTrimmedLengthSeconds();
            if (srcSec <= 0.0 || targetSec <= 0.0) return 1.0;
            return juce::jlimit (0.25, 4.0, srcSec / targetSec);
        }
        default: return 1.0; // Natural
    }
}

void Voice::startNote (const Pending& p) noexcept
{
    if (p.sample == nullptr || p.sample->numSamples <= 0)
    {
        active = false;
        return;
    }

    midiNote = p.note;
    velocity = juce::jlimit (0.0f, 1.0f, p.vel);
    forcedAlgorithm = p.opts.forcedAlgorithm;
    prePitchedSemi  = p.opts.prePitchedSemi;

    const auto n = p.sample->numSamples;
    const auto s = (std::int64_t) std::floor ((double) juce::jlimit (0.0f, 1.0f, p.s01) * (double) n);
    const auto e = (std::int64_t) std::ceil  ((double) juce::jlimit (0.0f, 1.0f, p.e01) * (double) n);
    reader.configure (p.sample, s, e, p.snap);

    srcPos = 0.0;
    sourceReleaseTriggered = false;
    released = false;
    drainCounter = 0;


    varispeed.reset();
    wsola.reset();
    phaseVocoder.reset();
    reaper.reset();

    if (p.glide) porta.startGlide (p.originNote, (float) p.note);
    else         porta.startAt ((float) p.note);

    adsr.setParameters (adsrParams);

    // **エンジンをここで選び直す。** forcedAlgorithm はノートごとに決まる（キャッシュ経路か
    // フォールバックか）が、setEngineControl はホストの processBlock 冒頭でしか来ないので、
    // 選び直さないとこの音だけ前の設定のまま鳴る。
    // 具体的には、REAPER Shifter を非REAPERホストで使うと activeEngine が
    // フォールバック先の Phase Vocoder になっており、キャッシュ再生の1音目が
    // PV で鳴ってエンベロープも PV のレイテンシぶん遅れる（＝頭が欠ける）。
    activeEngine = forcedAlgorithm >= 0 ? engineOrFallback (forcedAlgorithm)
                                        : pickEngine (control.algorithm);

    // 申告レイテンシ(alignLatency)より固有遅延が小さい分は先頭無音で埋め、全ノートの出だしを揃える
    const int intrinsic = activeEngine ? activeEngine->getIntrinsicLatency() : 0;
    startDelay = juce::jmax (0, p.opts.alignLatency - intrinsic);

    // エンベロープ開始を実効レイテンシ（固有遅延 + 先頭無音）分だけ遅らせる
    const int lat = intrinsic + startDelay;

    // ビブラートの Delay/Fade も同じ基準にする。負から始めることで、
    // エンベロープと同じタイミング（＝実際に音が出る瞬間）を 0 とみなす。
    // 揃えないと高レイテンシのエンジンで「音より先に揺れ始める」ように聞こえる。
    vibrato.reset (-(double) lat);

    pendingOff = -1;
    if (lat <= 0)
    {
        adsr.noteOn();
        pendingOn = -1;
    }
    else
    {
        adsr.reset();       // 遅延中はアイドル（音も無音なので整合）
        pendingOn = lat;
    }

    active = true;
    stealing = false;
    stealGain = 1.0f;
}

void Voice::noteOn (const SampleBuffer* sample, int note, float vel,
                    float s01, float e01, bool snap, bool glide, float originNote,
                    NoteOptions opts)
{
    startNote (Pending { sample, note, vel, s01, e01, snap, glide, originNote, opts });
}

void Voice::requestSteal (const SampleBuffer* sample, int note, float vel,
                          float s01, float e01, bool snap, bool glide, float originNote,
                          NoteOptions opts)
{
    Pending p { sample, note, vel, s01, e01, snap, glide, originNote, opts };
    if (! active) { startNote (p); return; }
    pending   = p;
    stealing  = true;
    stealGain = 1.0f;
    stealStep = -(float) (1.0 / (0.005 * sampleRate));
}

void Voice::glideTo (int note) noexcept
{
    if (! active) return;
    porta.setTarget ((float) note);
    midiNote = note;
    released = false;
}

void Voice::setGlideOrigin (float originNote) noexcept { porta.setOrigin (originNote); }

void Voice::noteOff() noexcept
{
    if (active && ! stealing)
    {
        const int lat = activeEngine ? activeEngine->getIntrinsicLatency() + startDelay : 0;
        if (lat <= 0) adsr.noteOff();
        else          pendingOff = lat;   // リリースもレイテンシ分遅らせる
        released = true;
    }
}

void Voice::stop() noexcept
{
    active = false; stealing = false; stealGain = 1.0f;
    pendingOn = -1; pendingOff = -1;
    adsr.reset();
}

void Voice::render (float* const* out, int numChannels, int n) noexcept
{
    if (! active || n <= 0)
        return;

    const int nch = juce::jmin (numChannels, preparedChannels);

    // 遅延したエンベロープイベントの発火（ブロック粒度）。音の出力遅延に揃える。
    if (pendingOn >= 0)
    {
        if (pendingOn <= n) { adsr.noteOn(); pendingOn = -1; }
        else                  pendingOn -= n;
    }
    if (pendingOff >= 0)
    {
        if (pendingOff <= n) { adsr.noteOff(); pendingOff = -1; }
        else                   pendingOff -= n;
    }

    // 整列用の先頭無音。ポルタメント/ビブラート/エンベロープはこの間進めない（音が出ていないので）
    int off = 0;
    if (startDelay > 0)
    {
        const int k = juce::jmin (startDelay, n);
        startDelay -= k;
        if (k == n) return;    // 規約7: n == 0 でエンジンを呼ばない
        off = k;
        n  -= k;
    }

    // timeRatio（20msスムージング, §4.7）
    timeRatioSmooth.setTargetValue (resolveTimeRatio());
    const double tr = timeRatioSmooth.skip (n);

    activeEngine->setFormantShift (control.formantSemi);
    varispeed.setQuality (params.quality);

    // ピッチ（ノート番号 → 比）
    porta.process (noteBuf.data(), n);
    // prePitchedSemi は sample に既に焼き込まれたシフト量。二重に掛からないよう差し引く。
    const float base = params.pitchSemi + params.pitchCents * 0.01f
                     + params.pitchBendSemi - (float) params.rootKey - prePitchedSemi;

    // ビブラート: 発音から delay 経過後、fade をかけて depth（セント）へ到達する三角関数変調。
    // 規約4に従い半音（対数）ドメインで base に足してから比へ変換する。
    vibDepthSmooth.setTargetValue (params.vibDepthCents * 0.01f);   // セント → 半音
    VibratoLfo::Config vc;
    vc.rateHz       = params.vibRateHz;
    vc.delaySamples = (double) params.vibDelayMs * 0.001 * sampleRate;
    vc.fadeSamples  = (double) params.vibFadeMs  * 0.001 * sampleRate;

    for (int i = 0; i < n; ++i)
    {
        vc.depthSemi = vibDepthSmooth.getNextValue();               // 深さだけ平滑化（規約#14）
        const float mod = vibrato.next (vc);
        ratioBuf[(std::size_t) i] = std::exp2 ((noteBuf[(std::size_t) i] + base + mod) / 12.0f);
    }

    for (int ch = 0; ch < nch; ++ch)
        scratchPtrs[(std::size_t) ch] = scratch[(std::size_t) ch].data();

    activeEngine->process (reader, srcPos, scratchPtrs.data(), nch, n, ratioBuf.data(), tr);

    // 素材を読み切ったらリリース開始（レイテンシ分遅らせる）+ テールドレイン計測
    const bool srcDone = reader.isFinished (srcPos);
    if (! sourceReleaseTriggered && srcDone)
    {
        const int lat = activeEngine->getIntrinsicLatency();
        if (lat <= 0)            adsr.noteOff();
        else if (pendingOff < 0) pendingOff = lat;
        released = true; sourceReleaseTriggered = true;
    }
    if (srcDone) drainCounter += n; else drainCounter = 0;

    // エンベロープ + ゲイン（レイテンシ整列は廃止、エンジン出力をそのまま加算）
    for (int i = 0; i < n; ++i)
    {
        float g = adsr.getNextSample() * velocity * params.gainLin;
        if (stealing)
        {
            g *= stealGain;
            stealGain += stealStep;
            if (stealGain < 0.0f) stealGain = 0.0f;
        }
        for (int ch = 0; ch < nch; ++ch)
            out[ch][off + i] += scratch[(std::size_t) ch][(std::size_t) i] * g;
    }

    if (stealing && stealGain <= 0.0f)
    {
        startNote (pending);
        return;
    }

    // 素材を読み切った後、エンジン内部テール(getTailSamples)を全量ドレインしてから停止（切らない）。
    // 遅延エンベロープ発火待ち(pendingOn/Off)の間は落とさない（音がこれから出るため）。
    const bool envPending = (pendingOn >= 0 || pendingOff >= 0);
    const int  totalTail  = activeEngine->getTailSamples();
    if (! stealing && ! envPending && ! adsr.isActive() && (! srcDone || drainCounter >= totalTail))
        active = false;
}

int Voice::getReportedLatency (int algorithm) const noexcept
{
    // pickEngine と同じ対応表を使う。ここだけ別に書くと、実際に鳴るエンジンと
    // 報告するレイテンシが食い違って発音タイミングがズレる（実際にやらかした）。
    const auto* e = const_cast<Voice*> (this)->engineForAlgorithm (algorithm);
    return (e != nullptr ? e : &phaseVocoder)->getIntrinsicLatency();
}

} // namespace otomad
