#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#include "ElastiqueDirect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace otomad
{

namespace
{
    // 素材の前に流す無音の長さ。プライミングで捨てられるぶんを吸収する。
    // feedChunk のチャンク長(1024)の整数倍で、実測の固有遅延(4610)より十分大きく取る。
    constexpr std::int64_t kFeedPadding = 8192;
}

namespace
{
    // RE で復元した ABI。公式 SDK が無いのでここに固めておく。
    using CreateInstanceE3  = void* (*) (int blockSize, int channels, float sampleRate, int mode);
    using DestroyInstanceE3 = void  (*) (void*);

    // vtable スロット（probe 実測）
    using ProcessDataFn  = int  (*) (void*, const float* const*, int, const float* const*);
    using SetStretchQFn  = int  (*) (void*, const float*, float);
    using ResetFn        = void (*) (void*);

    constexpr std::size_t kProcessSlot    = 1;
    constexpr std::size_t kSetStretchSlot = 5;
    constexpr std::size_t kResetSlot      = 9;

    // object 先頭の vtable から slot を取り出す
    template <typename T>
    T vtableFn (void* inst, std::size_t idx)
    {
        auto vtable = *reinterpret_cast<void***> (inst);
        return reinterpret_cast<T> (vtable[idx]);
    }
}

//==============================================================================
std::vector<std::string> ElastiqueDirect::defaultCandidates()
{
    return {
        R"(C:\Program Files\REAPER (x64)\Plugins\elastique3.dll)",
        R"(C:\Program Files\REAPER\Plugins\elastique3.dll)",
        R"(C:\Program Files (x86)\REAPER\Plugins\elastique3.dll)",
    };
}

ElastiqueDirect::~ElastiqueDirect() { unload(); }

bool ElastiqueDirect::load (const std::string& dllPath)
{
#ifdef _WIN32
    // レンダ中に差し替えられると module が消えて落ちるので、同じロックで排他する
    // （unload() を呼ぶと再帰ロックになるため、解放はここに展開する）。
    std::lock_guard<std::mutex> lk (renderLock);
    if (lib != nullptr)
        FreeLibrary ((HMODULE) lib);
    lib = createFn = destroyFn = nullptr;
    loadedPath.clear();

    std::vector<std::string> paths;
    if (! dllPath.empty())
        paths.push_back (dllPath);
    else
        paths = defaultCandidates();

    for (const auto& p : paths)
    {
        std::error_code ec;
        if (! std::filesystem::exists (p, ec))
            continue;

        auto* h = LoadLibraryA (p.c_str());
        if (h == nullptr)
            continue;

        auto* create  = GetProcAddress (h, "CreateInstance_E3");
        auto* destroy = GetProcAddress (h, "DestroyInstance_E3");
        if (create == nullptr || destroy == nullptr)
        {
            FreeLibrary (h);      // 想定した export が無い＝別物の DLL
            continue;
        }

        lib        = (void*) h;
        createFn   = (void*) create;
        destroyFn  = (void*) destroy;
        loadedPath = p;
        return true;
    }
#else
    (void) dllPath;   // Windows 専用の実験機能
#endif
    return false;
}

void ElastiqueDirect::unload()
{
    std::lock_guard<std::mutex> lk (renderLock);   // レンダ中の解放を防ぐ
#ifdef _WIN32
    if (lib != nullptr)
        FreeLibrary ((HMODULE) lib);
#endif
    lib = createFn = destroyFn = nullptr;
    loadedPath.clear();
}

//==============================================================================
// 固有遅延を実測する。無音 → バースト を流し、出力側で立ち上がりが何サンプル遅れるかを見る。
// 素材に依存しないよう、判定用の信号はここで作る（呼び出し側の素材は使わない）。
int ElastiqueDirect::latencySamples (int numChannels, double sampleRate, Mode mode) const
{
    const int numCh = std::clamp (numChannels, 1, 2);
    const int srKey = (int) std::lround (sampleRate);
    const auto key = std::make_tuple ((int) mode, srKey, numCh);

    {
        std::lock_guard<std::mutex> lk (renderLock);
        const auto it = latencyCache.find (key);
        if (it != latencyCache.end())
            return it->second;
    }

    // **閾値で立ち上がりを探してはいけない。** 出力は遅延ぶん遅れて始まるだけでなく、
    // 立ち上がり方もアルゴリズム依存なので、閾値だと材料次第で早くも遅くも反応する。
    // 実際、サイン波で測ると実際の群遅延より 1022 サンプル手前を返し、808 のような
    // 立ち上がりの速い素材で先頭に無音が残って段差になった。
    // 白色雑音を流して相互相関の山を取れば、材料や閾値に依存せず遅延そのものが出る。
    const std::int64_t n = (std::int64_t) (sampleRate * 0.4);
    SampleBuffer probe;
    probe.numChannels = numCh;
    probe.sampleRate  = sampleRate;
    probe.numSamples  = n;
    probe.data.assign ((std::size_t) numCh, std::vector<float> ((std::size_t) n, 0.0f));
    std::uint32_t seed = 12345u;   // 決定的にする（測るたびに値が変わらないように）
    for (std::int64_t i = 0; i < n; ++i)
    {
        seed = seed * 1103515245u + 12345u;
        const float v = 0.5f * ((float) ((int) ((seed >> 16) & 0x7fff) - 16384) / 16384.0f);
        for (int ch = 0; ch < numCh; ++ch) probe.data[(std::size_t) ch][(std::size_t) i] = v;
    }

    // シフト無し（1.0）で流す。遅延はシフト量に依らないので、これで代表させる。
    auto out = renderOffline (probe, 0, n, numCh, sampleRate, 1.0, 1.0, mode);

    int lat = 0;
    if (! out.empty() && ! out[0].empty())
    {
        // 入力の [w0, w0+win) と、出力を lag ずらしたものとの相関が最大になる lag を探す。
        const std::int64_t maxLag = (std::int64_t) (sampleRate * 0.3);
        const std::int64_t win    = std::min<std::int64_t> (4096, n / 4);
        const std::int64_t w0     = n / 4;
        const auto& x = probe.data[0];
        const auto& y = out[0];
        double best = -1.0e30;
        std::int64_t bestLag = 0;
        for (std::int64_t lag = 0; lag <= maxLag; ++lag)
        {
            if (w0 + lag + win > (std::int64_t) y.size()) break;
            double acc = 0.0;
            for (std::int64_t i = 0; i < win; ++i)
                acc += (double) x[(std::size_t) (w0 + i)] * y[(std::size_t) (w0 + lag + i)];
            if (acc > best) { best = acc; bestLag = lag; }
        }
        lat = (int) bestLag;
    }

    std::lock_guard<std::mutex> lk (renderLock);
    latencyCache[key] = lat;
    return lat;
}

std::vector<std::vector<float>>
ElastiqueDirect::renderOffline (const SampleBuffer& src,
                                std::int64_t base, std::int64_t n,
                                int numChannels, double sampleRate,
                                double pitch, double timeRatio,
                                Mode mode) const
{
    std::vector<std::vector<float>> out;
    if (! isAvailable() || n <= 0 || numChannels <= 0)
        return out;

    // このインターフェイス（ProcessData が入力 render サンプル→出力 render サンプルの 1:1）は
    // 構造上タイムストレッチを表現できない。Soloist の pitch シフトのみ対応する。
    // ストレッチ要求時は空を返し、呼び出し側でキャッシュ不成立＝Varispeed 再生にフォールバックさせる。
    if (std::abs (timeRatio - 1.0) > 1.0e-3)
        return out;

    // 使える範囲はモードごとに違う（実測。ヘッダの usableSemitoneRange 参照）。
    // 外側だと音程が狂ったり、Soloist ではレベルが落ちすぎて makeup gain でも救えない。
    // そのまま焼き付けると「頼んだのと違う音」がキャッシュに残るので、ここで諦めて
    // 呼び出し側の Varispeed フォールバックに任せる。
    {
        int lo = 0, hi = 0;
        usableSemitoneRange (mode, lo, hi);
        const double semi = 12.0 * std::log2 (pitch);
        if (! (semi >= (double) lo - 0.5 && semi <= (double) hi + 0.5))
            return out;
    }

    std::lock_guard<std::mutex> lk (renderLock);   // DLL のスレッド安全性が不明なので直列化

    const int numCh  = std::clamp (numChannels, 1, 2);   // élastique は 1/2ch で使う
    const int render = 1024;                             // feed チャンク長（DLL 側の上限）

    auto create  = reinterpret_cast<CreateInstanceE3>  (createFn);
    auto destroy = reinterpret_cast<DestroyInstanceE3> (destroyFn);

    void* inst = create (render, numCh, (float) sampleRate, (int) mode);
    if (inst == nullptr)
        return out;

    auto process  = vtableFn<ProcessDataFn> (inst, kProcessSlot);
    auto setQ     = vtableFn<SetStretchQFn> (inst, kSetStretchSlot);
    auto resetFn  = vtableFn<ResetFn>       (inst, kResetSlot);

    // 順序が重要: Reset はピッチ factor を既定(1.0)へ戻すので、必ず Reset の**後**に
    // SetStretchQFactor を呼ぶ。逆順にすると素通し（無シフト）の音がキャッシュに焼き付く。
    resetFn (inst);
    // stretch は 1.0 固定（上でストレッチ要求を弾いている）。
    // pfStretch はエンジンが実効値を書き戻す in/out param。
    float stretch = 1.0f;
    setQ (inst, &stretch, (float) pitch);

    const std::int64_t expectedLen = n;

    out.assign ((std::size_t) numCh, {});
    for (auto& c : out)
        c.reserve ((std::size_t) (expectedLen + 2 * (std::int64_t) sampleRate));

    // 入出力のチャンクバッファ（プレーナ）
    std::vector<std::vector<float>> inBuf ((std::size_t) numCh, std::vector<float> ((std::size_t) render, 0.0f));
    std::vector<std::vector<float>> outBuf ((std::size_t) numCh, std::vector<float> ((std::size_t) render, 0.0f));
    std::vector<const float*> inPtr ((std::size_t) numCh), outPtr ((std::size_t) numCh);
    for (int ch = 0; ch < numCh; ++ch)
    {
        inPtr[(std::size_t) ch]  = inBuf[(std::size_t) ch].data();
        outPtr[(std::size_t) ch] = outBuf[(std::size_t) ch].data();
    }

    auto feedChunk = [&] () -> bool
    {
        // rc==0 のときだけ出力が有効。rc==-1 は起動プライミング中。
        const int rc = process (inst, inPtr.data(), render, outPtr.data());
        if (rc != 0)
            return false;
        for (int ch = 0; ch < numCh; ++ch)
            out[(std::size_t) ch].insert (out[(std::size_t) ch].end(),
                                          outBuf[(std::size_t) ch].begin(),
                                          outBuf[(std::size_t) ch].end());
        return true;
    };

    // 0) **素材の前に無音を詰める。**
    //    feedChunk は起動プライミング中（rc != 0）の出力を捨てるが、そのとき入力は
    //    消費済みなので、いきなり素材を流すと**素材の先頭がそのぶん失われる**。
    //    実測: 808 のような立ち上がりの速い素材で、先頭 1024 サンプル（＝1ブロック）が
    //    無音になり、その直後に振幅 0.668 へ跳んで盛大にプチッと鳴っていた。
    //    先に無音を流しておけばプライミングが無音を食うので素材は削られない。
    //    捨てられるぶんは呼び出し側が固有遅延（latencySamples）で切り落とす。
    //    この関数は遅延の実測にも使われるので、詰め物の量は常に同じにすること。
    for (std::int64_t p2 = 0; p2 < kFeedPadding; p2 += render)
    {
        for (auto& b : inBuf) std::fill (b.begin(), b.end(), 0.0f);
        feedChunk();
    }

    // 1) 実入力を供給
    for (std::int64_t pos = 0; pos < n; pos += render)
    {
        const int c = (int) std::min<std::int64_t> (render, n - pos);
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& b = inBuf[(std::size_t) ch];
            for (int i = 0; i < c; ++i)
                b[(std::size_t) i] = src.sampleAtRaw (ch, base + pos + i);
            std::fill (b.begin() + c, b.end(), 0.0f);   // 端数は 0 埋め
        }
        feedChunk();
    }

    // 2) 内部に溜まった本体を無音で押し出す。
    //    プライミング（~5120サンプル）ぶん遅れて出てくるので、実音が揃うまで流す。
    const std::int64_t maxLead = (std::int64_t) (0.3 * sampleRate);
    const std::int64_t target  = kFeedPadding + expectedLen + maxLead + (std::int64_t) (0.25 * sampleRate);
    const std::int64_t maxSilence = kFeedPadding + std::max (expectedLen, n) + 2 * (std::int64_t) sampleRate;

    for (auto& b : inBuf) std::fill (b.begin(), b.end(), 0.0f);

    std::int64_t fed = 0;
    int emptyStreak = 0;
    while (fed < maxSilence && (std::int64_t) out[0].size() < target)
    {
        const std::size_t before = out[0].size();
        feedChunk();
        fed += render;
        if (out[0].size() == before) { if (++emptyStreak >= 16) break; }
        else                          emptyStreak = 0;
    }

    destroy (inst);
    return out;
}

} // namespace otomad
