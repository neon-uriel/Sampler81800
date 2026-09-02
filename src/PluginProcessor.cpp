#include "PluginProcessor.h"
#include "PluginEditor.h"
#if OTOMAD_WEB_UI
 #include "WebEditor.h"
#endif
#include "core/FfmpegDecoder.h"
#include "core/PitchDetect.h"
#include "core/PitchFlattener.h"
#include "core/Params.h"
#include "core/Utf8.h"
#include "core/SampleLoader.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX      // windows.h の min/max マクロが std::min/max を壊すのを防ぐ
 #endif
 #include <windows.h>
#endif

using otomad::SampleBuffer;

//==============================================================================
// 外観ブロードキャスト用のプロセス内レジストリ。同一プロセスで動く全インスタンスを束ね、
// 「既定にする」押下時に現在の外観を全インスタンスへ即時反映する（メッセージスレッド）。
namespace
{
    struct AppearanceHub
    {
        std::mutex m;
        std::vector<OtoMadSamplerProcessor*> instances;
        static AppearanceHub& get() { static AppearanceHub h; return h; }
    };
}

//==============================================================================
OtoMadSamplerProcessor::OtoMadSamplerProcessor()
    : AudioProcessor (BusesProperties()
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", otomad::params::createLayout())
{
    formatManager.registerBasicFormats();
    pitchCache.setApi (&reaperApi);
    pitchCache.setElastique (&elastique);

    pPitchSemi   = apvts.getRawParameterValue (otomad::params::pitchSemi);
    pOctave      = apvts.getRawParameterValue (otomad::params::octave);
    pPitchCents  = apvts.getRawParameterValue (otomad::params::pitchCents);
    pRootKey     = apvts.getRawParameterValue (otomad::params::rootKey);
    pInterp      = apvts.getRawParameterValue (otomad::params::interpQuality);
    pAttack      = apvts.getRawParameterValue (otomad::params::attack);
    pDecay       = apvts.getRawParameterValue (otomad::params::decay);
    pSustain     = apvts.getRawParameterValue (otomad::params::sustain);
    pRelease     = apvts.getRawParameterValue (otomad::params::release);
    pSampleStart = apvts.getRawParameterValue (otomad::params::sampleStart);
    pSampleEnd   = apvts.getRawParameterValue (otomad::params::sampleEnd);
    pSnap        = apvts.getRawParameterValue (otomad::params::snapZeroCross);
    pGain        = apvts.getRawParameterValue (otomad::params::gain);
    pPortaMode   = apvts.getRawParameterValue (otomad::params::portaMode);
    pPortaShape  = apvts.getRawParameterValue (otomad::params::portaShape);
    pPortaTime   = apvts.getRawParameterValue (otomad::params::portaTime);
    pPortaCurve  = apvts.getRawParameterValue (otomad::params::portaCurve);
    pGlideGroup  = apvts.getRawParameterValue (otomad::params::glideGroupMs);
    pPolyMode    = apvts.getRawParameterValue (otomad::params::polyMode);
    pMaxVoices   = apvts.getRawParameterValue (otomad::params::maxVoices);
    pBendRange   = apvts.getRawParameterValue (otomad::params::bendRange);
    pAlgorithm   = apvts.getRawParameterValue (otomad::params::algorithm);
    pDurationMode = apvts.getRawParameterValue (otomad::params::durationMode);
    pSyncLength  = apvts.getRawParameterValue (otomad::params::syncLength);
    pStretch     = apvts.getRawParameterValue (otomad::params::stretchAmount);
    pFormant     = apvts.getRawParameterValue (otomad::params::formant);
    pPhaseLock   = apvts.getRawParameterValue (otomad::params::phaseLock);
    pReaperMode    = apvts.getRawParameterValue (otomad::params::reaperMode);
    pReaperSubMode = apvts.getRawParameterValue (otomad::params::reaperSubMode);
    pElastiqueMode = apvts.getRawParameterValue (otomad::params::elastiqueMode);
    pCacheFallback = apvts.getRawParameterValue (otomad::params::cacheFallback);
    pVibDepth    = apvts.getRawParameterValue (otomad::params::vibDepth);
    pVibRate     = apvts.getRawParameterValue (otomad::params::vibRate);
    pVibDelay    = apvts.getRawParameterValue (otomad::params::vibDelay);
    pVibFade     = apvts.getRawParameterValue (otomad::params::vibFade);

    loadDefaultAppearance();   // 全インスタンス共通の外観既定（あれば）。state復元があれば後で上書きされる。

    // élastique 直叩き（実験機能）: 以前に設定したパスがあれば復元する
    if (const auto f = elastiqueSettingsFile(); f.existsAsFile())
        if (const auto p = f.loadFileAsString().trim(); p.isNotEmpty())
            loadElastiqueDll (p);

    // ffmpeg: 保存済みパスを復元する。verify() は子プロセスを起こして遅いので
    // ここではやらない（保存時に検証済み。消えていれば existsAsFile で弾かれる）。
    if (const auto f = ffmpegSettingsFile(); f.existsAsFile())
        if (const auto p = f.loadFileAsString().trim(); p.isNotEmpty())
            ffmpegExe = juce::File (p);

    // ブロードキャスト用レジストリに登録
    {
        auto& hub = AppearanceHub::get();
        std::lock_guard<std::mutex> lk (hub.m);
        hub.instances.push_back (this);
    }

    startTimerHz (6);   // UI非依存でキャッシュを駆動（窓を閉じても貯まる）
}

OtoMadSamplerProcessor::~OtoMadSamplerProcessor()
{
    stopTimer();
    auto& hub = AppearanceHub::get();
    std::lock_guard<std::mutex> lk (hub.m);
    hub.instances.erase (std::remove (hub.instances.begin(), hub.instances.end(), this),
                         hub.instances.end());
}

//==============================================================================
void OtoMadSamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate.store (sampleRate);

    // 再初期化で noteOff が届かないまま止まった鍵を「押されたまま」で光らせ続けない。
    for (auto* bits : { heldNoteBits, noteOnLatchBits, missLatchBits })
        for (int w = 0; w < 2; ++w) bits[w].store (0, std::memory_order_relaxed);

    // 再生用バッファ(data)がホストSRと違うSRで作られていたら作り直す（規約16）。
    // 再生側は data が既にホストSRである前提で読むので、ズレるとそのまま音程が狂う
    // （44.1k のバッファを 48k で読むと +147cent）。次の2つで実際に食い違う:
    //   - setStateInformation は prepareToPlay より前に走るため、プロジェクトを開き直すと
    //     まだ既定値(44100)で data が作られる
    //   - デバイスSRの変更や、REAPER のレンダリング(エンコード)SR がプレビューと違う場合
    rebuildSamplesForSampleRate (sampleRate);

    voices.prepare (sampleRate, samplesPerBlock, 2, &reaperApi);
    lastReportedLatency = desiredLatency();   // processBlock と必ず同じ式で出す
    setLatencySamples (lastReportedLatency);

    prepared.store (true);   // ここで state 復元の有無が確定（setStateInformation は prepare より前）
}

bool OtoMadSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (! layouts.getMainInputChannelSet().isDisabled())
        return false;

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono()
        || out == juce::AudioChannelSet::stereo();
}

//==============================================================================
void OtoMadSamplerProcessor::updateVoiceParams() noexcept
{
    otomad::Voice::Params vp;
    vp.pitchSemi  = pPitchSemi->load() + 12.0f * pOctave->load();   // オクターブを半音に換算して合算
    vp.pitchCents = pPitchCents->load();
    vp.rootKey    = (int) pRootKey->load();
    vp.gainLin    = juce::Decibels::decibelsToGain (pGain->load()) * normGain.load();
    vp.quality    = otomad::VarispeedEngine::Quality::Hermite;   // 補間は Hermite 固定（良い方）
    vp.vibDepthCents = pVibDepth->load();
    vp.vibRateHz     = pVibRate->load();
    vp.vibDelayMs    = pVibDelay->load();
    vp.vibFadeMs     = pVibFade->load();
    voices.setVoiceParams (vp);

    voices.setAdsr (pAttack->load()  * 0.001f,
                    pDecay->load()   * 0.001f,
                    pSustain->load(),
                    pRelease->load() * 0.001f);

    voices.setPortamento ((otomad::VoiceManager::PortaMode) (int) pPortaMode->load(),
                          otomad::PortamentoGenerator::Shape::Time,   // Shape は Time 固定
                          pPortaTime->load(), pPortaCurve->load(), pGlideGroup->load());

    // Poly/Mono は Voices で決める: 1 なら Mono（ラストノート・グライド）、2以上で Poly。
    const int maxV = (int) pMaxVoices->load();
    voices.setPoly (maxV > 1, maxV);

    otomad::Voice::EngineControl ec;
    ec.algorithm    = (int) pAlgorithm->load();
    ec.durationMode = (int) pDurationMode->load();
    ec.stretchAmount = pStretch->load();
    static constexpr float syncBeatsTable[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
    ec.syncBeats    = syncBeatsTable[juce::jlimit (0, 4, (int) pSyncLength->load())];
    ec.hostBpm      = hostBpm;
    ec.hostBpmValid = hostBpmValid;
    ec.formantSemi  = activeFormantSemi();   // UI から外している間は常に 0
    ec.phaseLock    = pPhaseLock->load() > 0.5f;
    ec.reaperMode    = (int) pReaperMode->load();
    ec.reaperSubMode = (int) pReaperSubMode->load();
    voices.setEngineControl (ec);
}

void OtoMadSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // ホストテンポ（Sync 用, §4.7）
    hostBpmValid = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
            {
                hostBpm = *bpm;
                hostBpmValid = true;
            }

    // 画面上のキーボードからの入力を MIDI にマージ
    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);

    updateVoiceParams();

    // レイテンシ報告は processBlock で行わない（規約#1: ロック/ホスト通知が走る。規約#17: 鳴動中に変えない）。
    // 望ましい値だけを atomic で伝え、実際の setLatencySamples はメッセージスレッド側で行う。
    pendingLatency.store (desiredLatency(), std::memory_order_relaxed);

    // ---- §2.2 : MIDIイベント位置でブロックを分割してレンダリング ----
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, buffer.getNumSamples(), meta.samplePosition);
        if (t > pos)
        {
            renderSlice (buffer, pos, t - pos);
            pos = t;
        }
        handleMidiMessage (meta.getMessage());
    }

    const int tail = buffer.getNumSamples() - pos;
    if (tail > 0)
        renderSlice (buffer, pos, tail);
}

void OtoMadSamplerProcessor::renderSlice (juce::AudioBuffer<float>& buffer,
                                          int startSample, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    float* ptrs[2] = { nullptr, nullptr };
    for (int ch = 0; ch < numCh; ++ch)
        ptrs[ch] = buffer.getWritePointer (ch) + startSample;

    voices.render (ptrs, numCh, numSamples);
}

void OtoMadSamplerProcessor::handleMidiMessage (const juce::MidiMessage& msg) noexcept
{
    if (msg.isNoteOn())
    {
        const int   note = msg.getNoteNumber();
        const float vel  = msg.getFloatVelocity();
        const float s = pSampleStart->load(), e = pSampleEnd->load();
        const bool  snap = pSnap->load() > 0.5f;

        // 鍵盤表示用（atomic のみ）。held は現在押されている鍵、latch は前回 UI が読んでから
        // 一度でも鳴った鍵。UI のポーリングより短いノートも 1 フレームは光らせるため。
        if (note >= 0 && note < 128)
        {
            const std::uint64_t bit = 1ull << (note & 63);
            heldNoteBits[note >> 6].fetch_or (bit, std::memory_order_relaxed);
            noteOnLatchBits[note >> 6].fetch_or (bit, std::memory_order_relaxed);
        }

        if (useCachePath())
        {
            const int semi = juce::jlimit (otomad::PitchCache::kMin, otomad::PitchCache::kMax,
                                           note - (int) pRootKey->load() + (int) pPitchSemi->load()
                                             + 12 * (int) pOctave->load());
            const int fbAlgo = cacheFallbackAlgorithm();
            const int align  = voices.getLatencyFor (fbAlgo);   // desiredLatency と同じ値。全ノートをこれに揃える
            if (const auto* cached = pitchCache.lookup (semi))
                // キャッシュはトリム範囲だけをレンダ済み → 全体(0..1)を Varispeed で等速再生（二重トリム防止）
                voices.noteOn (note, vel, cached, 0.0f, 1.0f, snap, { 0, (float) semi, align });
            else
            {
                pitchCache.request (semi);                                           // 背景でレンダリング要求
                voices.noteOn (note, vel, activeSample.load(), s, e, snap, { fbAlgo, 0.0f, align });   // 原音をフォールバック先で
                if (note >= 0 && note < 128)   // 鍵盤表示用（規約15）
                    missLatchBits[note >> 6].fetch_or (1ull << (note & 63), std::memory_order_relaxed);
            }
        }
        else
        {
            voices.noteOn (note, vel, activeSample.load(), s, e, snap);
        }
    }
    else if (msg.isNoteOff())
    {
        const int note = msg.getNoteNumber();
        if (note >= 0 && note < 128)
            heldNoteBits[note >> 6].fetch_and (~(1ull << (note & 63)), std::memory_order_relaxed);
        voices.noteOff (note);
    }
    else if (msg.isPitchWheel())
    {
        const float norm = ((float) msg.getPitchWheelValue() - 8192.0f) / 8192.0f;
        voices.setPitchBendSemi (norm * (float) (int) pBendRange->load());
    }
    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
    {
        heldNoteBits[0].store (0, std::memory_order_relaxed);
        heldNoteBits[1].store (0, std::memory_order_relaxed);
        voices.allNotesOff();
    }
}

int OtoMadSamplerProcessor::keyCacheState (int note) const noexcept
{
    if (! useCachePath())
        return 0;   // キャッシュを使わない経路。鍵ごとの差は無い
    const int semi = note - (int) pRootKey->load() + (int) pPitchSemi->load()
                   + 12 * (int) pOctave->load();
    int lo = 0, hi = 0;
    pitchCache.usableRange (lo, hi);
    // handleMidiMessage は semi を ±96 に clamp してから lookup するので、ここも同じ値で判定する。
    const int clamped = juce::jlimit (otomad::PitchCache::kMin, otomad::PitchCache::kMax, semi);
    if (clamped < lo || clamped > hi || pitchCache.isFailed (clamped))
        return 2;   // この設定では作れない → 常にフォールバック
    return pitchCache.lookup (clamped) != nullptr ? 0 : 1;
}

//==============================================================================
void OtoMadSamplerProcessor::publishSample (std::shared_ptr<const otomad::SampleBuffer> sb)
{
    if (sb == nullptr)
        return;

    // 読み込みは背景スレッドで走る。サンプルリストと APVTS はメッセージスレッド専用なので、
    // そちらへ回す（WeakReference でプラグイン破棄後の実行を防ぐ）。
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        addSampleImpl (std::move (sb));
        return;
    }
    juce::WeakReference<OtoMadSamplerProcessor> weak (this);
    juce::MessageManager::callAsync ([weak, sb]() mutable
    {
        if (auto* p = weak.get())
            p->addSampleImpl (std::move (sb));
    });
}

// スロットと「今の状態(APVTS/normGain)」の同期はこの2つだけが担う。
// 各所で個別に退避/復元を書くと取りこぼしが起きるため（復元時にスロットを上書きする等）、
// 必ずこの2関数を通す。どちらもメッセージスレッド専用。
void OtoMadSamplerProcessor::saveActiveToSlot()
{
    const int cur = activeIndex.load();
    if (cur < 0 || cur >= (int) sampleParams.size())
        return;
    sampleNorm[(std::size_t) cur]   = normGain.load();
    sampleParams[(std::size_t) cur] = apvts.copyState();
}

void OtoMadSamplerProcessor::loadSlotToActive (int index)
{
    if (index < 0 || index >= (int) sampleList.size())
        return;
    activeIndex.store (index);
    activeSample.store (sampleList[(std::size_t) index].get());
    normGain.store (sampleNorm[(std::size_t) index]);

    if (index < (int) sampleParams.size() && sampleParams[(std::size_t) index].isValid())
        apvts.replaceState (sampleParams[(std::size_t) index]);

    sampleVersion.fetch_add (1);   // → キャッシュは再設定され、この素材で作り直される
}

// 復元専用。APVTS には一切触れずスロットを積むだけ。
// （読み込みと同じ経路を通すと「直前スロットを現在の APVTS で上書き」が走り、
//   復元したスロットごとの設定が次のスロット追加で壊れる）
void OtoMadSamplerProcessor::appendRestoredSlot (std::shared_ptr<const otomad::SampleBuffer> sb,
                                                 float norm, juce::ValueTree params)
{
    if (sb == nullptr)
        return;
    {
        const juce::ScopedLock sl (graveyardLock);
        sampleGraveyard.push_back (sb);
    }
    const juce::ScopedLock sl2 (slotLock);
    sampleList.push_back (std::move (sb));
    sampleNorm.push_back (norm);
    sampleParams.push_back (std::move (params));
}

// 保存用 FLAC を取得（初回だけエンコードしてキャッシュ）
const OtoMadSamplerProcessor::FlacBlob& OtoMadSamplerProcessor::getFlacFor (const otomad::SampleBuffer& sb)
{
    const juce::ScopedLock sl (flacLock);
    auto it = flacCache.find (&sb);
    if (it != flacCache.end())
        return it->second;

    FlacBlob blob;
    blob.ok = otomad::SampleLoader::encodeOriginalToFlac (sb, blob.data, blob.normScale);
    return flacCache.emplace (&sb, std::move (blob)).first->second;
}

void OtoMadSamplerProcessor::addSampleImpl (std::shared_ptr<const otomad::SampleBuffer> sb)
{
    if (sb == nullptr)
        return;
    {
        const juce::ScopedLock sl (graveyardLock);
        sampleGraveyard.push_back (sb);
    }

    saveActiveToSlot();   // 今のスロットの設定を退避してから追加する

    // 新スロットは「今の設定」を引き継ぐ（差し替えて聴き比べるとき同条件で始められる）
    {
        const juce::ScopedLock sl2 (slotLock);
        sampleList.push_back (sb);
        sampleNorm.push_back (1.0f);
        sampleParams.push_back (apvts.copyState());
    }

    activeIndex.store ((int) sampleList.size() - 1);
    activeSample.store (sb.get());
    normGain.store (1.0f);
    sampleVersion.fetch_add (1);
}

// DLL の場所は「このPCに REAPER がどこに入っているか」というマシン固有の事実なので、
// プロジェクト state だけでなくユーザー設定として保存し、新規インスタンスでも復元する。
juce::File OtoMadSamplerProcessor::elastiqueSettingsFile()
{
    return defaultAppearanceFile().getSiblingFile ("elastique.txt");
}

bool OtoMadSamplerProcessor::loadElastiqueDll (const juce::String& path)
{
    const bool ok = elastique.load (path.toStdString());
    elastiquePath = ok ? juce::String (elastique.getLoadedPath()) : path;

    if (ok)
    {
        const auto f = elastiqueSettingsFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (elastiquePath);
        sampleVersion.fetch_add (1);   // バックエンドが変わったのでキャッシュを作り直させる
    }
    return ok;
}

void OtoMadSamplerProcessor::unloadElastiqueDll()
{
    elastique.unload();
    elastiquePath.clear();
    elastiqueSettingsFile().deleteFile();
    sampleVersion.fetch_add (1);
}

//==============================================================================
// Space をホストのウィンドウへ投げ直す。
//
// WebView2 はネイティブの子ウィンドウなので、フォーカスがあると Space を全部食ってしまい
// DAW まで届かない（さらにフォーカスの残ったボタンが押されてしまう）。そこで JS 側で
// preventDefault し、ここでホスト窓へ WM_KEYDOWN/WM_KEYUP を送り直す。
//
// REAPER でも Main_OnCommand を直接叩かず、同じくキーを投げ直す。理由:
//   - Space の既定アクションは REAPER でも Play/stop (40044) であって Play/pause (40073)
//     ではない。アクションIDを決め打ちすると DAW 本来の挙動とズレる。
//   - ユーザーが Space を別アクションに割り当てていても、キーを渡せばそれがそのまま効く。
// VST3 には「ホストのトランスポートを操作する」標準的な手段が無い
// （JUCE の AudioPlayHead::canControlTransport も VST3 ラッパでは常に false）ので、
// どのみち非REAPERホストではこの方法しかない。経路を一本にしておく。
bool OtoMadSamplerProcessor::forwardSpaceKeyToHost (void* editorNativeHandle)
{
#if JUCE_WINDOWS
    if (auto hwnd = (HWND) editorNativeHandle)
    {
        // エディタ窓の祖先をたどってホスト側のトップレベル窓を探す。
        // DAW によっては中間のフローティングFX窓で止まるが、その窓がキーを
        // ホストのメッセージループへ流してくれるので実害はない。
        HWND target = GetAncestor (hwnd, GA_ROOT);
        if (target == nullptr) target = hwnd;

        PostMessage (target, WM_KEYDOWN, VK_SPACE, 0);
        PostMessage (target, WM_KEYUP,   VK_SPACE, 0xC0000000);
        return true;
    }
#else
    juce::ignoreUnused (editorNativeHandle);
#endif
    return false;
}

juce::String OtoMadSamplerProcessor::getSampleName (int index) const
{
    const juce::ScopedLock sl (slotLock);
    if (index < 0 || index >= (int) sampleList.size() || sampleList[(std::size_t) index] == nullptr)
        return {};
    return juce::String (sampleList[(std::size_t) index]->name);
}

void OtoMadSamplerProcessor::selectSample (int index)
{
    if (index < 0 || index >= (int) sampleList.size() || index == activeIndex.load())
        return;
    saveActiveToSlot();
    loadSlotToActive (index);
}

void OtoMadSamplerProcessor::removeSample (int index)
{
    if (index < 0 || index >= (int) sampleList.size())
        return;

    // 削除対象以外は編集内容を失わないよう、先に現在の設定を退避しておく
    saveActiveToSlot();

    // 再生中の可能性があるので解放せず graveyard に退避する
    {
        const juce::ScopedLock sl (graveyardLock);
        sampleGraveyard.push_back (sampleList[(std::size_t) index]);
    }
    {
        const juce::ScopedLock sl2 (slotLock);
        sampleList.erase (sampleList.begin() + index);
        sampleNorm.erase (sampleNorm.begin() + index);
        if (index < (int) sampleParams.size())
            sampleParams.erase (sampleParams.begin() + index);
    }

    if (sampleList.empty())
    {
        activeIndex.store (-1);
        activeSample.store (nullptr);
        normGain.store (1.0f);
        sampleVersion.fetch_add (1);
        return;
    }

    // 削除に伴い選択インデックスを詰め直す。
    // 削除したのが選択中でなければ、選択は同じサンプルのまま維持する。
    const int cur = activeIndex.load();
    int next;
    if (index == cur)      next = juce::jmin (index, (int) sampleList.size() - 1);
    else if (index < cur)  next = cur - 1;
    else                   next = cur;

    activeIndex.store (-1);          // 退避は済んでいるので二重に走らせない
    loadSlotToActive (juce::jlimit (0, (int) sampleList.size() - 1, next));
}

void OtoMadSamplerProcessor::loadSampleFromMemory (juce::MemoryBlock bytes, juce::String displayName)
{
    const double sr = hostSampleRate.load();
    loadPool.addJob ([this, bytes = std::move (bytes), displayName, sr]
    {
        { const juce::ScopedLock sl (ffmpegLock); lastLoadError.clear(); }

        // まず JUCE のデコーダで試す。読めればここで終わり（従来どおり）。
        if (auto sb = otomad::SampleLoader::loadFromMemory (bytes.getData(), bytes.getSize(),
                                                            displayName, sr, formatManager))
        { publishSample (sb); return; }

        // 読めなかったものだけ ffmpeg へ回す。拡張子で振り分けないので、
        // ffmpeg が読める形式（mp4 / m4a / webm / mkv / mov / opus …）は自動的に通る。
        juce::File exe;
        { const juce::ScopedLock sl (ffmpegLock); exe = ffmpegExe; }

        if (! exe.existsAsFile())
        {
            const juce::ScopedLock sl (ffmpegLock);
            lastLoadError = otomad::u8 ("デコードできませんでした: ") + displayName
                          + otomad::u8 ("（設定画面で ffmpeg を指定すると mp4 等も読めます）");
            return;
        }

        juce::String err;
        const auto wav = otomad::FfmpegDecoder::decodeToWav (exe, bytes.getData(), bytes.getSize(),
                                                             displayName, err);
        if (! wav.existsAsFile())
        {
            const juce::ScopedLock sl (ffmpegLock);
            lastLoadError = "ffmpeg: " + err.upToFirstOccurrenceOf ("\n", false, false);
            return;
        }

        auto sb = otomad::SampleLoader::loadFile (wav, sr, formatManager);
        wav.deleteFile();

        if (sb == nullptr)
        {
            const juce::ScopedLock sl (ffmpegLock);
            lastLoadError = otomad::u8 ("ffmpeg の出力を読めませんでした: ") + displayName;
            return;
        }
        // ffmpeg 経由でも UI にはドロップしたファイル名を出す（中間wavの名前ではなく）
        sb->name = displayName.toStdString();
        publishSample (sb);
    });
}

//==============================================================================
// SHIFTER 欄に出す説明。REAPER 上か、élastique 直読みか、どちらも無いかを区別する。
// 「REAPER 非対応ホスト」としか出ないと、élastique を設定しても効いていないように見える。
juce::String OtoMadSamplerProcessor::getShifterStatusText() const
{
    if (reaperApi.isAvailable())
        return getReaperModeText();

    if (! elastique.isAvailable())
        return {};   // JS 側が「設定で élastique を指定すると使えます」を出す

    // 直読み経路が実際に効く条件かどうかまで出す。効かないときに理由が分からないのが一番困る。
    if (! useCachePath())
        return otomad::u8 ("elastique 直読み（実験）: Duration を Natural / Manual にすると有効");

    if ((int) pDurationMode->load() == 2)
    {
        const float st = pStretch->load();
        if (std::abs ((double) st - 1.0) > 1.0e-3)
            return otomad::u8 ("elastique 直読み（実験）: ストレッチ中は非対応 → Varispeed 再生");
    }

    const bool solo = (int) pElastiqueMode->load() == 1;
    int lo = 0, hi = 0;
    otomad::ElastiqueDirect::usableSemitoneRange (solo ? otomad::ElastiqueDirect::Soloist
                                                       : otomad::ElastiqueDirect::Pro, lo, hi);
    return otomad::u8 ("elastique 直読み（実験） / ")
         + otomad::u8 (solo ? "Soloist（単声専用）" : "Elastique Pro")
         + "  " + juce::String (lo) + otomad::u8 ("〜+") + juce::String (hi) + otomad::u8 (" 半音");
}

juce::File OtoMadSamplerProcessor::ffmpegSettingsFile()
{
    return defaultAppearanceFile().getSiblingFile ("ffmpeg.txt");
}

bool OtoMadSamplerProcessor::setFfmpegPath (const juce::String& path)
{
    // 空なら自動探索。指定があってもそのまま信じず -version で実際に動くか確かめる。
    const auto f = path.isEmpty() ? otomad::FfmpegDecoder::find() : juce::File (path);
    if (! otomad::FfmpegDecoder::verify (f))
        return false;

    { const juce::ScopedLock sl (ffmpegLock); ffmpegExe = f; }

    const auto s = ffmpegSettingsFile();
    s.getParentDirectory().createDirectory();
    s.replaceWithText (f.getFullPathName());
    return true;
}

void OtoMadSamplerProcessor::clearFfmpegPath()
{
    { const juce::ScopedLock sl (ffmpegLock); ffmpegExe = juce::File(); }
    ffmpegSettingsFile().deleteFile();
}

bool OtoMadSamplerProcessor::isFfmpegAvailable() const
{
    const juce::ScopedLock sl (ffmpegLock);
    return ffmpegExe.existsAsFile();
}

juce::String OtoMadSamplerProcessor::getFfmpegPath() const
{
    const juce::ScopedLock sl (ffmpegLock);
    return ffmpegExe.getFullPathName();
}

juce::String OtoMadSamplerProcessor::getLastLoadError() const
{
    const juce::ScopedLock sl (ffmpegLock);
    return lastLoadError;
}

void OtoMadSamplerProcessor::loadSampleFromFile (const juce::File& file)
{
    const double sr = hostSampleRate.load();
    loadPool.addJob ([this, file, sr]
    {
        if (auto sb = otomad::SampleLoader::loadFile (file, sr, formatManager))
            publishSample (sb);
    });
}

juce::StringArray OtoMadSamplerProcessor::getReaperModeNames() const
{
    juce::StringArray a;
    auto fn = reinterpret_cast<bool (*) (int, const char**)> (reaperApi.getFunction ("EnumPitchShiftModes"));
    if (fn == nullptr) return a;
    const char* mn = nullptr;
    for (int m = 0; m < 256 && fn (m, &mn); ++m)
        a.add (mn ? juce::String::fromUTF8 (mn) : ("mode " + juce::String (m)));
    return a;
}

juce::StringArray OtoMadSamplerProcessor::getReaperSubModeNames (int mode) const
{
    juce::StringArray a;
    auto fn = reinterpret_cast<const char* (*) (int, int)> (reaperApi.getFunction ("EnumPitchShiftSubModes"));
    if (fn == nullptr) return a;
    for (int s = 0; s < 100000; ++s)
    {
        const char* sn = fn (mode, s);
        if (sn == nullptr) break;
        a.add (juce::String::fromUTF8 (sn));
    }
    return a;
}

void OtoMadSamplerProcessor::maybeApplyDefaultReaperMode()
{
    if (reaperDefaultChecked || ! prepared.load() || ! reaperApi.isAvailable())
        return;
    reaperDefaultChecked = true;
    if (stateWasRestored.load())
        return;   // 復元済みインスタンスはユーザ設定を尊重

    // 新規インスタンス: 既定を élastique Soloist に。名前で解決（版によりインデックスが変わるため）。
    const auto modes = getReaperModeNames();
    int soloist = -1;
    for (int i = 0; i < modes.size(); ++i)               // 新しい版(3.x)の Soloist を優先
        if (modes[i].containsIgnoreCase ("soloist") && modes[i].contains ("3.")) { soloist = i; break; }
    if (soloist < 0)
        for (int i = 0; i < modes.size(); ++i)           // 無ければ任意の Soloist
            if (modes[i].containsIgnoreCase ("soloist")) { soloist = i; break; }
    if (soloist < 0)
        return;   // Soloist が見つからないホスト/版では既定のまま

    int sub = 0;                                         // サブモードは Monophonic 優先
    const auto subs = getReaperSubModeNames (soloist);
    for (int i = 0; i < subs.size(); ++i)
        if (subs[i].containsIgnoreCase ("mono")) { sub = i; break; }

    if (auto* pm = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (otomad::params::reaperMode)))
        *pm = soloist;
    if (auto* ps = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (otomad::params::reaperSubMode)))
        *ps = sub;
}

void OtoMadSamplerProcessor::serviceCache()
{
    maybeApplyDefaultReaperMode();   // 新規インスタンスの初期モード（Soloist）を一度だけ適用

    // レイテンシ報告はここ（メッセージスレッド）で行う。processBlock からは atomic で希望値だけ受け取る。
    const int lat = pendingLatency.load (std::memory_order_relaxed);
    if (lat >= 0 && lat != lastReportedLatency)
    {
        lastReportedLatency = lat;
        setLatencySamples (lat);
    }

    // Manual のときは stretchAmount から timeRatio を算出（Natural は 1.0）
    double timeRatio = 1.0;
    if ((int) pDurationMode->load() == 2)
    {
        const float st = pStretch->load();
        timeRatio = st > 0.0f ? 1.0 / (double) st : 1.0;
    }

    // 素材/モード/フォルマント/ストレッチ/トリム を反映（変わっていれば ready を無効化して作り直す）
    const bool changed = pitchCache.configure (activeSample.load(), sampleVersion.load(),
                          (int) pReaperMode->load(), (int) pReaperSubMode->load(),
                          hostSampleRate.load(), activeFormantSemi(), timeRatio,
                          pSampleStart->load(), pSampleEnd->load(),
                          (int) pElastiqueMode->load());

    // プリウォーム: 現在の pitchSemi + octave を中心に ±48 半音をまとめて背景生成
    // （停止中に貯める）。キャッシュ自体は ±96 まで持てるが、全部先読みすると
    // 0.5秒ステレオで約37MBになるので窓は ±48 のまま。窓の外は初回発音時にオンデマンドで入る。
    //
    // **`changed` だけを条件にしてはいけない。** configure が見ているのは素材/モード/SR等で、
    // **algorithm も移調量も含まれていない**。プロジェクトを開き直したとき、状態復元より前の
    // タイマー1発目で changed==true を消費してしまうと（そのときはまだ既定の Varispeed なので
    // useCachePath()==false）、以後 configure は false を返し続け、先読みが永久に走らない。
    // その結果どの音もキャッシュミスになり、REAPER Shifter を選んでいるのに全部
    // Varispeed で鳴る（＝アルゴリズムを選び直すまで音がおかしい）。
    // 経路に入った瞬間と移調量が動いたときにも貼り直す。requestRange は ready 済みを
    // 飛ばすので、貼り直しても余計なレンダリングは起きない。
    const bool cachePath = useCachePath();
    const int  centre    = (int) pPitchSemi->load() + 12 * (int) pOctave->load();
    if (changed || ! cachePath || centre != prewarmCentre)
        prewarmDone = false;
    if (cachePath && ! prewarmDone)
    {
        prewarmDone   = true;
        prewarmCentre = centre;
        pitchCache.requestRange (centre - 48, centre + 48);
    }

    // 保留中の音程があれば背景スレッドで並列レンダリング。cacheThreads 本まで稼働数を補充する。
    // 各ジョブは renderPending を空になるまで回し、完了したら稼働数を減らす。
    if (pitchCache.hasPending())
    {
        while (cacheJobsActive.load() < cacheThreads)
        {
            cacheJobsActive.fetch_add (1);
            loadPool.addJob ([this]
            {
                while (pitchCache.renderPending()) {}
                cacheJobsActive.fetch_sub (1);
            });
        }
    }
}

void OtoMadSamplerProcessor::reconfigureReaperMode()
{
    const int m  = (int) pReaperMode->load();
    const int sm = (int) pReaperSubMode->load();
    suspendProcessing (true);          // オーディオコールバックを止めてから非RT処理
    voices.reconfigureReaper (m, sm);
    lastReportedLatency = desiredLatency();
    setLatencySamples (lastReportedLatency);
    suspendProcessing (false);
}


bool OtoMadSamplerProcessor::detectAndSetRoot()
{
    const auto* sb = activeSample.load();
    if (sb == nullptr || sb->numSamples <= 0 || sb->numChannels <= 0)
        return false;

    const double sr = sb->sampleRate > 0.0 ? sb->sampleRate : hostSampleRate.load();
    const std::int64_t total = sb->numSamples;
    std::int64_t s = (std::int64_t) std::llround ((double) juce::jlimit (0.0f, 1.0f, pSampleStart->load()) * (double) total);
    std::int64_t e = (std::int64_t) std::llround ((double) juce::jlimit (0.0f, 1.0f, pSampleEnd->load())   * (double) total);
    s = juce::jlimit<std::int64_t> (0, total, s);
    e = juce::jlimit<std::int64_t> (0, total, e);

    // アタックを少し飛ばした解析領域
    const std::int64_t skip   = std::min<std::int64_t> ((e - s) / 20, (std::int64_t) (0.01 * sr));
    const std::int64_t rStart = s + skip;
    const std::int64_t rEnd   = e;
    if (rEnd - rStart < 1024)
        return false;

    // 領域全体に複数窓を敷いて、各窓のYIN結果(信頼できるもの)の音程を中央値で採る。
    // → 単一窓のオクターブ誤検出・トランジェント・ノイズに強くする。
    const int W = (int) std::min<std::int64_t> (4096, rEnd - rStart);
    const float inv = 1.0f / (float) sb->numChannels;
    const int maxWin = 24;
    std::int64_t hop = (rEnd - rStart - W) / std::max (1, maxWin - 1);
    if (hop < W / 4) hop = std::max<std::int64_t> (W / 4, 1);   // 領域が短ければ窓は少数

    std::vector<float> buf ((std::size_t) W);
    std::vector<float> notes;      // 信頼できた窓の音程（MIDI, float）
    notes.reserve (32);

    for (std::int64_t ws = rStart; ws + 1024 <= rEnd; ws += hop)
    {
        const int w = (int) std::min<std::int64_t> (W, rEnd - ws);
        if (w < 1024)
            break;

        // モノ化 ＋ DC除去 ＋ 実効レベルチェック（無音窓は捨てる）
        double mean = 0.0, energy = 0.0;
        for (int i = 0; i < w; ++i)
        {
            float m = 0.0f;
            for (int ch = 0; ch < sb->numChannels; ++ch)
                m += sb->data[(std::size_t) ch][(std::size_t) (ws + i)];
            m *= inv;
            buf[(std::size_t) i] = m;
            mean += m;
        }
        mean /= (double) w;
        for (int i = 0; i < w; ++i) { buf[(std::size_t) i] -= (float) mean; energy += (double) buf[(std::size_t) i] * buf[(std::size_t) i]; }
        if (energy / (double) w < 1.0e-6)   // ほぼ無音 → スキップ
            continue;

        double freq = 0.0;
        const double aper = otomad::pitchdetect::yinWindow (buf.data(), w, sr, freq);
        if (aper < 0.2 && freq > 0.0)
            notes.push_back ((float) otomad::pitchdetect::hzToMidi (freq));

        if ((int) notes.size() >= maxWin)
            break;
    }

    if (notes.size() < 3)   // 信頼できる窓が少なすぎ → 明確な音程なしと判断
        return false;

    std::sort (notes.begin(), notes.end());
    const double P = (double) notes[notes.size() / 2];      // 検出したサンプルの実ピッチ（MIDI）

    // 50セント(=0.5半音)グリッドに合わせる: 実ピッチ P を最寄りのグリッド G に補正する。
    //   Root = P に最も近い鍵盤（そのキーを押すと G が鳴る）
    //   Cent = P→G の補正量（最大 ±25 セント）
    const double G = std::round (P * 2.0) / 2.0;
    const int    root  = juce::jlimit (0, 127, (int) std::lround (P));
    const double cents = juce::jlimit (-100.0, 100.0, (G - P) * 100.0);

    if (auto* pr = dynamic_cast<juce::AudioParameterInt*>   (apvts.getParameter (otomad::params::rootKey)))
        *pr = root;
    if (auto* pc = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (otomad::params::pitchCents)))
        *pc = (float) cents;
    return true;
}

//==============================================================================
// 区間内のピッチを検出して、その区間を単一の音程へ平坦化する（Melodyne 的な処理）。
//
// 書き換える対象は **original（原音SR）**。data(ホストSR) だけ書き換えると、SRが変わったときに
// original から作り直されて平坦化が消える（規約16）。書き換え後に rebuildFromOriginal で
// data と peaks を作り直す。
//
// 解析も合成も重いので背景スレッドで走らせ、スロットへの反映はメッセージスレッドへ戻して行う
// （sampleList はメッセージスレッド専用）。
void OtoMadSamplerProcessor::flattenActiveSample (float strength)
{
    auto* sb = activeSample.load();
    if (sb == nullptr || sb->numChannels <= 0 || sb->originalSampleRate <= 0.0
        || sb->original.empty() || sb->original[0].empty())
        return;

    const int    slot = activeIndex.load();
    const double osr  = sb->originalSampleRate;
    const auto   olen = (std::int64_t) sb->original[0].size();

    // トリムは正規化値なので、原音側のインデックスへそのまま写せる（規約13）。
    const double s01 = juce::jlimit (0.0f, 1.0f, pSampleStart->load());
    const double e01 = juce::jlimit (0.0f, 1.0f, pSampleEnd->load());
    const auto rs = (std::int64_t) std::llround (s01 * (double) olen);
    const auto re = (std::int64_t) std::llround (e01 * (double) olen);

    otomad::FlattenOptions opt;
    opt.strength = juce::jlimit (0.0f, 1.0f, strength);

    // 走っている間にサンプルが差し替えられても安全なようにコピーを渡す
    auto snapshot = std::make_shared<std::vector<std::vector<float>>> (sb->original);
    const int numCh = sb->numChannels;

    loadPool.addJob ([this, snapshot, numCh, olen, osr, rs, re, opt, slot]
    {
        auto r = otomad::flattenToSinglePitch (*snapshot, numCh, olen, osr, rs, re, opt);
        juce::WeakReference<OtoMadSamplerProcessor> safe (this);
        juce::MessageManager::callAsync ([safe, r = std::move (r), slot]() mutable
        {
            if (safe != nullptr)
                safe->applyFlattenResult (std::move (r), slot);
        });
    });
}

// 指定SRの data を持つ新しいバッファを作って返す（原音は共有せずコピー）。
// **data を再リサンプルせず、必ず original から1回だけ変換する**（規約16: 二重変換禁止）。
// バッファは shared_ptr<const> で不変なので、作り直す = 新しく作って差し替える。
static std::shared_ptr<otomad::SampleBuffer>
makeResampledCopy (const otomad::SampleBuffer& cur, double sampleRate)
{
    auto next = std::make_shared<otomad::SampleBuffer>();
    next->numChannels        = cur.numChannels;
    next->originalSampleRate = cur.originalSampleRate;
    next->name               = cur.name;
    next->path               = cur.path;
    next->original           = cur.original;
    otomad::SampleLoader::rebuildFromOriginal (*next, sampleRate);
    return next;
}

// そのバッファが今のホストSRで作られていなければ true。
static bool needsRebuildFor (const otomad::SampleBuffer* sb, double sampleRate) noexcept
{
    return sb != nullptr && sb->numChannels > 0
        && ! sb->original.empty() && sb->originalSampleRate > 0.0
        && std::abs (sb->sampleRate - sampleRate) > 1.0e-6;
}

// スロットのバッファを差し替える。バッファは不変なので「作り直して入れ替える」。
// 旧バッファはオーディオスレッドがまだ読んでいる可能性があるので graveyard で寿命を延ばす。
void OtoMadSamplerProcessor::replaceSlotBuffer (int slot, std::shared_ptr<const otomad::SampleBuffer> next)
{
    if (next == nullptr)
        return;

    // 入れようとしているバッファが今のホストSRで作られていなければ作り直す。
    // 例: 平坦化 → SR変更（既存スロットは作り直される）→ UNDO で、
    // undo 用に取っておいた「SR変更前の」バッファが戻ってきてしまう。
    if (const double sr = hostSampleRate.load(); needsRebuildFor (next.get(), sr))
        next = makeResampledCopy (*next, sr);
    {
        const juce::ScopedLock sl (graveyardLock);
        sampleGraveyard.push_back (next);
    }
    {
        const juce::ScopedLock sl (slotLock);
        if (slot < 0 || slot >= (int) sampleList.size())
            return;
        // 差し替えで参照されなくなる側の FLAC キャッシュを捨てる（ポインタをキーにしているため）
        {
            const juce::ScopedLock fl (flacLock);
            flacCache.erase (sampleList[(std::size_t) slot].get());
        }
        sampleList[(std::size_t) slot] = next;
    }
    if (activeIndex.load() == slot)
        activeSample.store (next.get());

    sampleVersion.fetch_add (1);   // ピッチキャッシュ再生成 & UI 更新
}

// ホストSRが変わったら、原音(original)から data を作り直す。
// **data を再リサンプルしない**（規約16: 二重変換禁止）。原音は不変なので常にそこから1回だけ変換する。
// prepareToPlay から呼ばれる＝オーディオは停止中なので、確保とバッファ差し替えを行ってよい。
void OtoMadSamplerProcessor::rebuildSamplesForSampleRate (double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    bool rebuiltAny = false;

    const juce::ScopedLock sl (slotLock);
    for (std::size_t i = 0; i < sampleList.size(); ++i)
    {
        const auto& cur = sampleList[i];
        if (cur == nullptr || cur->numChannels <= 0
            || cur->original.empty() || cur->originalSampleRate <= 0.0)
            continue;
        if (! needsRebuildFor (cur.get(), sampleRate))
            continue;                       // 既に正しいSRで作られている

        auto next = makeResampledCopy (*cur, sampleRate);

        {
            const juce::ScopedLock gl (graveyardLock);
            sampleGraveyard.push_back (next);
        }
        {
            const juce::ScopedLock fl (flacLock);
            flacCache.erase (cur.get());   // ポインタキーなので差し替え前に捨てる
        }

        if (activeIndex.load() == (int) i)
            activeSample.store (next.get());
        sampleList[i] = next;
        rebuiltAny = true;
    }

    // **何も作り直していないなら version を上げない。**
    // prepareToPlay から毎回呼ばれるので、無条件に上げるとホストが再初期化するたびに
    // ピッチキャッシュが丸ごと無効化され、生成中のものも世代違いで捨てられる。
    if (rebuiltAny)
        sampleVersion.fetch_add (1);   // ピッチキャッシュを作り直させ、UI も更新させる
}

void OtoMadSamplerProcessor::applyFlattenResult (otomad::FlattenResult r, int slot)
{
    if (! r.ok || r.audio.empty())
    {
        const juce::ScopedLock sl (ffmpegLock);
        lastLoadError = otomad::u8 ("ピッチを検出できませんでした（音程のある区間を長めに選んでください）");
        return;
    }

    std::shared_ptr<const otomad::SampleBuffer> prev;
    {
        const juce::ScopedLock sl (slotLock);
        if (slot < 0 || slot >= (int) sampleList.size())
            return;
        prev = sampleList[(std::size_t) slot];
    }
    if (prev == nullptr || prev->numChannels != (int) r.audio.size())
        return;

    // 平坦化した原音から新しいバッファを作る（既存バッファは書き換えない）
    auto next = std::make_shared<otomad::SampleBuffer>();
    next->numChannels        = prev->numChannels;
    next->originalSampleRate = prev->originalSampleRate;
    next->name               = prev->name;
    next->path               = prev->path;
    next->original           = std::move (r.audio);
    otomad::SampleLoader::rebuildFromOriginal (*next, hostSampleRate.load());

    flattenUndo.slot = slot;
    flattenUndo.prev = prev;

    flattenTarget   = r.targetNote;
    flattenDetected = r.detectedMidi;
    flattenFrames   = r.voicedFrames;
    flattenResult   = r.resultMidi;

    replaceSlotBuffer (slot, next);

    // Root / Cent は **平坦化後の実測音程** から決める（DETECT ROOT と同じ 50cent グリッド）。
    // targetNote を信じて Cent=0 にしてはいけない: AMOUNT<100% では目標まで寄り切らないため、
    // 実際の音と最大 50cent ずれ、直後に DETECT を押すと違う値が出る（＝壊れて見える）。
    const double P = r.resultMidi > 0.0 ? r.resultMidi : (double) r.targetNote;
    const double G = std::round (P * 2.0) / 2.0;
    if (auto* pr = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (otomad::params::rootKey)))
        *pr = juce::jlimit (0, 127, (int) std::lround (P));
    if (auto* pc = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (otomad::params::pitchCents)))
        *pc = (float) juce::jlimit (-100.0, 100.0, (G - P) * 100.0);
}

bool OtoMadSamplerProcessor::canRevertFlatten() const
{
    const juce::ScopedLock sl (slotLock);
    return flattenUndo.prev != nullptr
        && flattenUndo.slot >= 0 && flattenUndo.slot < (int) sampleList.size();
}

bool OtoMadSamplerProcessor::revertFlatten()
{
    if (! canRevertFlatten())
        return false;

    const int slot = flattenUndo.slot;
    auto prev = flattenUndo.prev;
    flattenUndo = FlattenUndo{};
    flattenTarget = -1; flattenDetected = 0.0; flattenFrames = 0; flattenResult = 0.0;

    replaceSlotBuffer (slot, prev);
    return true;
}

juce::var OtoMadSamplerProcessor::getFlattenState() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("targetNote", flattenTarget);
    o->setProperty ("detected",   flattenDetected);
    o->setProperty ("frames",     flattenFrames);
    o->setProperty ("result",     flattenResult);
    o->setProperty ("canRevert",  canRevertFlatten());
    return juce::var (o);
}

// UI のピッチ曲線プレビュー（音は変えない）。解析は数十msかかるのでメッセージスレッドで
// 呼ぶが、押したときだけなので許容する。
juce::var OtoMadSamplerProcessor::analysePitchContour() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", false);

    const auto* sb = activeSample.load();
    if (sb == nullptr || sb->original.empty() || sb->originalSampleRate <= 0.0)
        return juce::var (o);

    const auto olen = (std::int64_t) sb->original[0].size();
    const double s01 = juce::jlimit (0.0f, 1.0f, pSampleStart->load());
    const double e01 = juce::jlimit (0.0f, 1.0f, pSampleEnd->load());

    const auto r = otomad::analyseOnly (sb->original, sb->numChannels, olen, sb->originalSampleRate,
                                        (std::int64_t) std::llround (s01 * (double) olen),
                                        (std::int64_t) std::llround (e01 * (double) olen));
    if (! r.ok)
        return juce::var (o);

    juce::Array<juce::var> midi;
    midi.ensureStorageAllocated ((int) r.contour.midi.size());
    for (float m : r.contour.midi) midi.add (m);

    o->setProperty ("ok",           true);
    o->setProperty ("midi",         midi);
    o->setProperty ("hopSeconds",   r.contour.hopSeconds);
    o->setProperty ("startSeconds", r.contour.startSeconds);
    o->setProperty ("detected",     r.detectedMidi);
    o->setProperty ("targetNote",   r.targetNote);
    return juce::var (o);
}

//==============================================================================
juce::String OtoMadSamplerProcessor::getCurrentVersion()
{
   #ifdef OTOMAD_VERSION
    return juce::String (OTOMAD_VERSION);
   #else
    return "0.0.0";
   #endif
}

juce::URL OtoMadSamplerProcessor::getReleasesUrl()
{
    return juce::URL ("https://github.com/neon-uriel/Sampler81800/releases");
}

juce::String OtoMadSamplerProcessor::getLatestVersion() const
{
    const juce::ScopedLock sl (updateStrLock);
    return latestVersionStr;
}

// "0.2.0" > "0.1.9" などをドット区切り整数で比較（先頭の 'v' は無視）。
static bool versionIsNewer (const juce::String& cand, const juce::String& base)
{
    auto toks = [] (juce::String s)
    {
        juce::StringArray t;
        t.addTokens (s.retainCharacters ("0123456789."), ".", "");
        return t;
    };
    const auto a = toks (cand), b = toks (base);
    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int va = i < a.size() ? a[i].getIntValue() : 0;
        const int vb = i < b.size() ? b[i].getIntValue() : 0;
        if (va != vb) return va > vb;
    }
    return false;
}

void OtoMadSamplerProcessor::checkForUpdatesAsync (bool force)
{
    if (! force && updateCheckStarted.exchange (true))
        return;   // 自動確認は一度だけ
    if (force)
        updateCheckStarted.store (true);

    loadPool.addJob ([this]
    {
        juce::URL url ("https://api.github.com/repos/neon-uriel/Sampler81800/releases/latest");
        const auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                            .withConnectionTimeoutMs (5000)
                            .withExtraHeaders ("User-Agent: OtoMadSampler\nAccept: application/vnd.github+json");

        std::unique_ptr<juce::InputStream> stream (url.createInputStream (opts));
        if (stream == nullptr)
            return;

        const auto text = stream->readEntireStreamAsString();
        const auto json = juce::JSON::parse (text);
        const auto tag  = json.getProperty ("tag_name", juce::var()).toString();
        if (tag.isEmpty())
            return;

        {
            const juce::ScopedLock sl (updateStrLock);
            latestVersionStr = tag.retainCharacters ("0123456789.");
        }
        updateAvailable.store (versionIsNewer (tag, getCurrentVersion()));
    });
}

void OtoMadSamplerProcessor::normalizeSample()
{
    const auto* sb = activeSample.load();
    if (sb == nullptr || sb->numChannels <= 0)
        return;

    float peak = 0.0f;
    for (int ch = 0; ch < sb->numChannels; ++ch)
        for (float v : sb->data[(std::size_t) ch])
            peak = std::max (peak, std::abs (v));

    normGain.store (peak > 1.0e-6f ? std::min (0.99f / peak, 64.0f) : 1.0f);
    sampleVersion.fetch_add (1);   // 波形表示を更新させる
}

void OtoMadSamplerProcessor::setBackgroundImageFromMemory (const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return;
    auto img = juce::ImageFileFormat::loadFrom (data, size);
    if (! img.isValid())
        return;

    // 保存は PNG に統一（state 埋め込み・既定ファイルと同じ扱いにする）
    juce::MemoryBlock png;
    {
        juce::MemoryOutputStream os (png, false);
        juce::PNGImageFormat fmt;
        if (! fmt.writeImageToStream (img, os))
            return;
    }
    bgImage = img;
    bgPng   = png;
    appearanceVersion.fetch_add (1);
}

void OtoMadSamplerProcessor::setBackgroundImageFromFile (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return;
    juce::Image img = juce::ImageFileFormat::loadFrom (in);
    if (! img.isValid())
        return;

    // 保存用に PNG へ再エンコード（形式非依存で state に埋め込める）
    juce::MemoryBlock png;
    {
        juce::MemoryOutputStream os (png, false);
        juce::PNGImageFormat fmt;
        if (! fmt.writeImageToStream (img, os))
            return;
    }
    bgImage = img;
    bgPng   = png;
    appearanceVersion.fetch_add (1);
}

//==============================================================================
void OtoMadSamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto root = std::make_unique<juce::XmlElement> ("OtoMadState");
    // 1 = 単一 <sample> / 2 = <samples> リスト（スロットごとのパラメータ付き）。
    // 古いビルドで開かれたとき「サンプル無し」と「壊れている」を区別できるようにする。
    root->setAttribute ("stateVersion", 2);
    root->setAttribute ("uiScalePct", (double) uiScalePct.load());   // UI 表示倍率
    root->setAttribute ("editorW", editorW.load());                  // エディタサイズ
    root->setAttribute ("editorH", editorH.load());
    if (elastiquePath.isNotEmpty())
        root->setAttribute ("elastiqueDll", elastiquePath);          // 実験機能のDLLパス

    if (auto apvtsXml = apvts.copyState().createXml())
        root->addChildElement (apvtsXml.release());

    // 読み込み済みサンプルを全て埋め込み保存（FLAC）＋パス（§3.9）。
    // 切り替えて聴き比べるための機能なので、リスト全体を保存して次回もそのまま比較できるようにする。
    // 走査中に再確保されないよう、まずロック下でスナップショットを取り、
    // 重い FLAC エンコード／base64 はロックを離してから行う。
    struct SlotSnap { std::shared_ptr<const otomad::SampleBuffer> buf; float norm; juce::ValueTree params; };
    std::vector<SlotSnap> snap;
    int act = -1;
    {
        const juce::ScopedLock sl (slotLock);
        act = activeIndex.load();
        snap.reserve (sampleList.size());
        for (std::size_t i = 0; i < sampleList.size(); ++i)
        {
            // 選択中のスロットは現在値、それ以外は退避済みの値
            const bool isActive = ((int) i == act);
            snap.push_back ({ sampleList[i],
                              isActive ? normGain.load() : sampleNorm[i],
                              isActive ? apvts.copyState()
                                       : (i < sampleParams.size() ? sampleParams[i] : juce::ValueTree()) });
        }
    }

    if (! snap.empty())
    {
        auto* list = root->createNewChildElement ("samples");
        list->setAttribute ("active", act);

        for (const auto& s : snap)
        {
            if (s.buf == nullptr) continue;

            auto* se = list->createNewChildElement ("sample");
            se->setAttribute ("name", juce::String (s.buf->name));
            se->setAttribute ("path", juce::String (s.buf->path));
            se->setAttribute ("normGain", (double) s.norm);

            if (s.params.isValid())
                if (auto px = s.params.createXml())
                    se->addChildElement (px.release());

            const auto& flac = getFlacFor (*s.buf);   // 初回のみエンコード（以降はキャッシュ）
            if (flac.ok)
            {
                se->setAttribute ("embedded", 1);
                se->setAttribute ("format", "flac");
                se->setAttribute ("srcSampleRate", s.buf->originalSampleRate);
                se->setAttribute ("normScale", (double) flac.normScale);
                se->addTextElement (flac.data.toBase64Encoding());
            }
            else
            {
                se->setAttribute ("embedded", 0);
            }
        }

        // 旧ビルド互換: ルート直下にも選択中スロットを <sample> として書いておく。
        // 新形式しか書かないと、古いビルドで開いたとき無言でサンプル無しになる。
        if (act >= 0 && act < (int) snap.size() && snap[(std::size_t) act].buf != nullptr)
        {
            const auto& s = snap[(std::size_t) act];
            auto* se = root->createNewChildElement ("sample");
            se->setAttribute ("name", juce::String (s.buf->name));
            se->setAttribute ("path", juce::String (s.buf->path));
            se->setAttribute ("normGain", (double) s.norm);

            const auto& flac = getFlacFor (*s.buf);
            if (flac.ok)
            {
                se->setAttribute ("embedded", 1);
                se->setAttribute ("format", "flac");
                se->setAttribute ("srcSampleRate", s.buf->originalSampleRate);
                se->setAttribute ("normScale", (double) flac.normScale);
                se->addTextElement (flac.data.toBase64Encoding());
            }
            else
            {
                se->setAttribute ("embedded", 0);
            }
        }
    }

    // 外観設定（メインカラー / 背景透過率 / 背景画像PNG）
    writeAppearance (*root->createNewChildElement ("appearance"));

    copyXmlToBinary (*root, destData);
}

void OtoMadSamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    if (xml->hasAttribute ("uiScalePct"))
        uiScalePct.store ((float) xml->getDoubleAttribute ("uiScalePct", 100.0));
    editorW.store (xml->getIntAttribute ("editorW", 0));
    editorH.store (xml->getIntAttribute ("editorH", 0));
    if (const auto p = xml->getStringAttribute ("elastiqueDll"); p.isNotEmpty())
        loadElastiqueDll (p);   // 失敗しても従来経路にフォールバックするだけ

    juce::XmlElement* apvtsXml  = nullptr;
    juce::XmlElement* sampleXml = nullptr;

    if (xml->hasTagName (apvts.state.getType()))
        apvtsXml = xml.get();                                   // 旧形式（APVTSのみ）
    else if (xml->hasTagName ("OtoMadState"))
    {
        apvtsXml  = xml->getChildByName (apvts.state.getType());
        sampleXml = xml->getChildByName ("sample");
    }

    if (apvtsXml != nullptr)
    {
        apvts.replaceState (juce::ValueTree::fromXml (*apvtsXml));
        stateWasRestored.store (true);   // 復元済み → 初期モード自動設定はしない
    }

    // 旧形式（単一 <sample>）。スロット0として積む。
    // 新形式では旧ビルド互換のため同じ内容が <samples> とルート直下の両方にあるので、
    // <samples> があるときはそちらだけを使う（二重読み込み防止）。
    const bool hasNewList = (xml->getChildByName ("samples") != nullptr);
    if (hasNewList)
        sampleXml = nullptr;

    if (sampleXml != nullptr)
        if (auto sb = restoreSample (*sampleXml))
            appendRestoredSlot (sb,
                                (float) sampleXml->getDoubleAttribute ("normGain", 1.0),
                                apvts.copyState());

    // 複数サンプル（新形式）。順に積み、最後に保存時の選択スロットへ戻す。
    // appendRestoredSlot は APVTS に触れないので、既に積んだスロットが上書きされない。
    if (auto* list = xml->getChildByName ("samples"))
    {
        for (auto* se : list->getChildWithTagNameIterator ("sample"))
        {
            auto sb = restoreSample (*se);
            if (sb == nullptr)
                continue;

            auto* px = se->getChildByName (apvts.state.getType());
            appendRestoredSlot (sb,
                                (float) se->getDoubleAttribute ("normGain", 1.0),
                                px != nullptr ? juce::ValueTree::fromXml (*px) : apvts.copyState());
        }
        const int act = list->getIntAttribute ("active", (int) sampleList.size() - 1);
        if (act >= 0 && act < (int) sampleList.size())
            loadSlotToActive (act);   // activeIndex はまだ -1 なので退避不要
    }

    // 旧形式や active 属性が壊れている場合でも、必ずどれかを選択状態にする
    if (activeIndex.load() < 0 && ! sampleList.empty())
        loadSlotToActive (0);

    // 外観設定の復元（state に無ければコンストラクタで読んだ共通既定のまま）
    if (auto* ae = xml->getChildByName ("appearance"))
        readAppearance (*ae);
}

//==============================================================================
juce::File OtoMadSamplerProcessor::defaultAppearanceFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("OtoMadSampler").getChildFile ("appearance.xml");
}

void OtoMadSamplerProcessor::writeAppearance (juce::XmlElement& ae) const
{
    ae.setAttribute ("mainColour", juce::String::toHexString ((int) mainColour.load()));
    ae.setAttribute ("bgOpacity", (double) bgOpacity.load());
    ae.setAttribute ("panelOpacity", (double) panelOpacity.load());
    if (bgPng.getSize() > 0)
        ae.addTextElement (bgPng.toBase64Encoding());
}

void OtoMadSamplerProcessor::readAppearance (const juce::XmlElement& ae)
{
    if (ae.hasAttribute ("mainColour"))
        mainColour.store ((juce::uint32) ae.getStringAttribute ("mainColour").getHexValue64());
    bgOpacity.store (juce::jlimit (0.0f, 1.0f, (float) ae.getDoubleAttribute ("bgOpacity", (double) bgOpacity.load())));
    panelOpacity.store (juce::jlimit (0.1f, 1.0f, (float) ae.getDoubleAttribute ("panelOpacity", (double) panelOpacity.load())));

    const auto b64 = ae.getAllSubText().trim();
    bgImage = juce::Image();
    bgPng.reset();
    if (b64.isNotEmpty() && bgPng.fromBase64Encoding (b64) && bgPng.getSize() > 0)
        bgImage = juce::ImageFileFormat::loadFrom (bgPng.getData(), bgPng.getSize());

    appearanceVersion.fetch_add (1);
}

void OtoMadSamplerProcessor::loadDefaultAppearance()
{
    const auto f = defaultAppearanceFile();
    if (! f.existsAsFile())
        return;
    if (auto xml = juce::XmlDocument::parse (f))
        if (xml->hasTagName ("appearance"))
            readAppearance (*xml);
}

void OtoMadSamplerProcessor::applyBroadcastAppearance (juce::uint32 argb, float opacity, float panelOp,
                                                       const juce::MemoryBlock& png)
{
    mainColour.store (argb);
    bgOpacity.store (juce::jlimit (0.0f, 1.0f, opacity));
    panelOpacity.store (juce::jlimit (0.1f, 1.0f, panelOp));
    bgPng   = png;
    bgImage = png.getSize() > 0 ? juce::ImageFileFormat::loadFrom (png.getData(), png.getSize())
                                : juce::Image();
    appearanceVersion.fetch_add (1);   // 各インスタンスのエディタがタイマで拾って再描画する
}

void OtoMadSamplerProcessor::saveAppearanceAsDefault()
{
    juce::XmlElement ae ("appearance");
    writeAppearance (ae);
    const auto f = defaultAppearanceFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (ae.toString());

    // 即時ブロードキャスト: 同一プロセス内の全インスタンスへ現在の外観を反映
    const auto argb = mainColour.load();
    const auto op   = bgOpacity.load();
    const auto pop  = panelOpacity.load();
    auto& hub = AppearanceHub::get();
    std::lock_guard<std::mutex> lk (hub.m);
    for (auto* inst : hub.instances)
        if (inst != this)
            inst->applyBroadcastAppearance (argb, op, pop, bgPng);
}

// 読み込むだけで公開はしない。呼び出し側（setStateInformation）が同期でスロットへ積む。
// publishSample 経由にすると非メッセージスレッドから呼ばれたとき追加が遅延し、
// 直後の sampleParams.back() 等が空/古いリストを触ってしまう。
std::shared_ptr<otomad::SampleBuffer> OtoMadSamplerProcessor::restoreSample (const juce::XmlElement& se)
{
    const double sr = hostSampleRate.load();
    std::shared_ptr<otomad::SampleBuffer> sb;

    // 埋め込み優先
    if ((int) se.getIntAttribute ("embedded", 0) == 1)
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (se.getAllSubText()))
            sb = otomad::SampleLoader::loadFromFlacMemory (mb.getData(), mb.getSize(), sr);

        if (sb != nullptr)
        {
            const float scale = (float) se.getDoubleAttribute ("normScale", 1.0);
            if (scale != 1.0f)   // 保存時に正規化した分を戻す
            {
                for (auto& ch : sb->original) for (auto& v : ch) v *= scale;
                for (auto& ch : sb->data)     for (auto& v : ch) v *= scale;
                for (auto& p : sb->peaks) { p.first *= scale; p.second *= scale; }
            }
        }
    }

    // ダメならパスから
    if (sb == nullptr)
    {
        const juce::String path = se.getStringAttribute ("path");
        const juce::File f (path);
        if (path.isNotEmpty() && f.existsAsFile())
            sb = otomad::SampleLoader::loadFile (f, sr, formatManager);
    }

    if (sb != nullptr)
    {
        sb->name = se.getStringAttribute ("name").toStdString();
        sb->path = se.getStringAttribute ("path").toStdString();
    }
    // 両方失敗 → nullptr を返す（サンプル無しのまま。GUI表示のみでクラッシュしない, §3.9）
    return sb;
}

//==============================================================================
juce::AudioProcessorEditor* OtoMadSamplerProcessor::createEditor()
{
   #if OTOMAD_WEB_UI
    // WebView2 ランタイムが無い環境では真っ白な窓になるので、ネイティブ版へ落とす（規約#15の精神）
    if (OtoMadSamplerWebEditor::isWebViewAvailable())
        return new OtoMadSamplerWebEditor (*this);
   #endif
    return new OtoMadSamplerEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OtoMadSamplerProcessor();
}
