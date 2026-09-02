// SDK 隔離（windows.h のマクロ汚染を抑止, JUCEは含めない）
#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
#endif

#include "host/reaper_sdk/reaper_plugin.h"   // IReaperPitchShift, ReaSample(double)

#include "PitchCache.h"
#include "host/ReaperApi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace otomad
{

using ReaperGetPitchShiftAPI_t = IReaperPitchShift* (*) (int);

// REAPER のシフタに「生成できる範囲」を直接聞く。
//
// SDK には REAPER_PITCHSHIFT_EXT_GETMINMAXPRODUCTS があり、シフタが扱える
// **shift × tempo の積**の下限/上限を返す（未対応の実装は 0 を返して parm に触らない）。
// 制限が shift 単体ではなく積に乗っているのは、この手のエンジンが内部で
// 「リサンプル＋伸縮」に分解して動くため。うちのキャッシュは常に tempo=1 で回すので、
// 積の範囲がそのまま shift の範囲になる。
//
// 決め打ちしないのが肝。élastique 直読みで使用可能範囲を推測して間違えた
// （Pro -39..+48 と申告していたが実際は ±24）ので、聞けるものは聞く。
// 取れなければ REAPER 経路は全域とみなす（従来動作）。
// REAPER のピッチシフト API が使えるか（＝フォルマントや広い音程範囲が使えるか）。
// élastique 直読みにはフォルマントの入口が無いので、この判定が「効くかどうか」と一致する。
bool PitchCache::reaperPathAvailable() const
{
    if (api == nullptr)
        return false;
    return api->getFunction ("ReaperGetPitchShiftAPI") != nullptr;
}

bool PitchCache::queryReaperRange (int mode, int sub, double sampleRate, int& lo, int& hi) const
{
    if (api == nullptr)
        return false;
    auto getPS = reinterpret_cast<ReaperGetPitchShiftAPI_t> (api->getFunction ("ReaperGetPitchShiftAPI"));
    if (getPS == nullptr)
        return false;
    IReaperPitchShift* ps = getPS (REAPER_PITCHSHIFT_API_VER);
    if (ps == nullptr)
        return false;

    lo = kMin; hi = kMax;   // 問い合わせに失敗しても REAPER 経路は全域扱い（従来動作）

    ps->set_srate (sampleRate > 0.0 ? sampleRate : 48000.0);
    ps->set_nch (1);
    ps->set_tempo (1.0);
    ps->SetQualityParameter ((mode << 16) + sub);

    double minProduct = 0.0, maxProduct = 0.0;
    if (ps->Extended (REAPER_PITCHSHIFT_EXT_GETMINMAXPRODUCTS, &minProduct, &maxProduct, nullptr) != 0
        && minProduct > 0.0 && minProduct < 1.0 && maxProduct > 1.0 && maxProduct < 1024.0)
    {
        // 積の範囲 → 半音。境界を跨がないよう内側に丸める。
        lo = (int) std::ceil  (12.0 * std::log2 (minProduct) - 1.0e-6);
        hi = (int) std::floor (12.0 * std::log2 (maxProduct) + 1.0e-6);
    }

    delete ps;
    return true;
}

bool PitchCache::configure (const SampleBuffer* src, int version, int mode, int sub,
                            double sampleRate, float formant, double timeRatio,
                            float start01, float end01, int elaMode)
{
    // 微小変化での再レンダリング連発を避けるため量子化
    // フォルマントは REAPER 経路でしか焼き込めない（élastique 直読みには入口が無い）。
    // 効かないのに鍵に入れると、ノブを動かすだけでキャッシュが全部無効化され、
    // 作り直している間ずっと Varispeed 再生に落ちる＝音が悪くなったように聞こえる。
    const bool   reaperOk = reaperPathAvailable();
    const bool   elaOk    = (elastique != nullptr && elastique->isAvailable());
    const float  fUsed = reaperOk ? formant : 0.0f;
    const float  fq = std::round (fUsed * 4.0f) / 4.0f;        // 0.25半音刻み
    const double tq = std::round (timeRatio * 100.0) / 100.0;    // 0.01刻み
    const float  sq = std::round (std::clamp (start01, 0.0f, 1.0f) * 1000.0f) / 1000.0f;   // 0.1%刻み
    const float  eq = std::round (std::clamp (end01,   0.0f, 1.0f) * 1000.0f) / 1000.0f;

    // 生成できる半音範囲を更新する。
    // **バックエンドが「使えるようになったか」も条件に入れること。** モード番号だけを
    // 見ていると、起動時にどちらのバックエンドも無くて空範囲(lo=1,hi=0)が入った後、
    // 後から élastique DLL を設定しても（あるいは REAPER の attach が遅れて完了しても）
    // 二度と問い合わせ直さず、request() が全音程を弾き続けてキャッシュが永久に作られない。
    // 問い合わせは REAPER のシフタ生成を伴うので、変化したときだけ走らせる。
    if (mode != probedMode || sub != probedSub || elaMode != probedElaMode
        || reaperOk != probedReaperOk || elaOk != probedElaOk)
    {
        probedMode = mode; probedSub = sub; probedElaMode = elaMode;
        probedReaperOk = reaperOk; probedElaOk = elaOk;

        int lo = 1, hi = 0;   // 既定は空範囲＝一切要求しない
        if (! queryReaperRange (mode, sub, sampleRate, lo, hi))
        {
            // REAPER が使えない → élastique 直読みの制限に従う。どちらも無ければ空のまま。
            if (elaOk)
                ElastiqueDirect::usableSemitoneRange (elaMode == 1 ? ElastiqueDirect::Soloist
                                                                   : ElastiqueDirect::Pro, lo, hi);
        }
        reqLo.store (std::max (lo, kMin), std::memory_order_relaxed);
        reqHi.store (std::min (hi, kMax), std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock (ownerLock);

    // どのフィールドで変化と判定したかを記録する。世代が進み続けて
    // レンダリング結果が毎回捨てられる不具合を、推測せずに特定するため。
    Changed why = Changed::None;
    if      (src        != curSrc)                          why = Changed::Src;
    else if (version    != curVersion)                      why = Changed::Version;
    else if (mode       != curMode)                         why = Changed::Mode;
    else if (sub        != curSub)                          why = Changed::Sub;
    else if (std::abs (sampleRate - curSr)   >= 1.0e-6)     why = Changed::SampleRate;
    else if (std::abs (fq - curFormant)      >= 1.0e-6)     why = Changed::Formant;
    else if (std::abs (tq - curTimeRatio)    >= 1.0e-6)     why = Changed::TimeRatio;
    else if (std::abs (sq - curStart)        >= 1.0e-6)     why = Changed::Start;
    else if (std::abs (eq - curEnd)          >= 1.0e-6)     why = Changed::End;
    else if (elaMode    != curElaMode)                      why = Changed::ElaMode;

    if (why == Changed::None)
        return false;
    changeReason.store (why, std::memory_order_relaxed);

    // 無効化: 新しい ready のみクリア。古いバッファは再生中の可能性があるので解放しない（graveyard保持）。
    for (auto& p : ready) p.store (nullptr, std::memory_order_release);
    for (auto& w : req)    w.store (0);
    for (auto& w : failed) w.store (0);   // 設定が変わったら失敗記録もやり直す

    curSrc = src; curVersion = version; curMode = mode; curSub = sub; curSr = sampleRate;
    curFormant = fq; curTimeRatio = tq; curStart = sq; curEnd = eq; curElaMode = elaMode;
    ++curGen;   // 設定が変わった → 進行中のレンダリングは無効化される
    gen.store (curGen, std::memory_order_relaxed);
    return true;
}

bool PitchCache::renderPending()
{
    int semi = 0; bool found = false;
    {
        // 保留ビットを1つ「原子的に」取り出す（複数背景スレッドが同じ半音を二重処理しないよう、
        // fetch_and の戻り値で自分がクリアした場合だけ確定する）。
        for (int w = 0; w < kWords && ! found; ++w)
        {
            for (int b = 0; b < 64 && ! found; ++b)
            {
                const int idx = w * 64 + b;
                if (idx >= kN) break;
                const std::uint64_t mask = 1ull << b;
                if ((req[(std::size_t) w].load() & mask) == 0) continue;
                if (req[(std::size_t) w].fetch_and (~mask) & mask)
                { semi = kMin + idx; found = true; }
            }
        }
    }

    if (! found)
        return false;

    int usedGen = -1;
    auto buf = renderShift (semi, usedGen);
    {
        std::lock_guard<std::mutex> lock (ownerLock);
        if (usedGen == curGen)   // レンダリング中に設定(素材/モード/フォルマント/ストレッチ)が変わっていなければ公開
        {
            if (buf != nullptr)
            {
                graveyard.push_back (buf);
                ready[(std::size_t) (semi - kMin)].store (buf.get(), std::memory_order_release);
            }
            else
            {
                // この設定では作れなかった → 記録して再要求されないようにする。
                // 記録しないと「要求→空→また要求」を無限に回して進捗が動かなくなる。
                const int bit = semi - kMin;
                failed[(std::size_t) (bit >> 6)].fetch_or (1ull << (bit & 63), std::memory_order_release);
            }
        }
    }
    return true;
}

std::shared_ptr<SampleBuffer> PitchCache::renderShift (int semi, int& usedGen)
{
    // 設定のスナップショット
    const SampleBuffer* src; int mode, sub, elaMode; double sr; float formant; double timeRatio; float start, end;
    {
        std::lock_guard<std::mutex> lock (ownerLock);
        src = curSrc; mode = curMode; sub = curSub; sr = curSr;
        formant = curFormant; timeRatio = curTimeRatio;
        start = curStart; end = curEnd; elaMode = curElaMode;
        usedGen = curGen;
    }
    if (src == nullptr || src->numSamples <= 0)
        return nullptr;

    // REAPER 上なら REAPER のピッチシフト API を使う。
    // それ以外でも、élastique DLL 直叩き（実験機能）が使えるならそちらで代替する。
    IReaperPitchShift* ps = nullptr;
    if (api != nullptr)
        if (auto getPS = reinterpret_cast<ReaperGetPitchShiftAPI_t> (api->getFunction ("ReaperGetPitchShiftAPI")))
            ps = getPS (REAPER_PITCHSHIFT_API_VER);

    const bool useDirect = (ps == nullptr)
                        && (elastique != nullptr) && elastique->isAvailable();
    if (ps == nullptr && ! useDirect)
        return nullptr;

    const int    numCh = std::max (1, src->numChannels);
    const double shift = std::pow (2.0, (double) semi / 12.0);

    // トリム範囲 [base, base+n) だけをレンダする（Start/End で指定した範囲のみキャッシュ）。
    const std::int64_t total = src->numSamples;
    std::int64_t base   = (std::int64_t) std::llround ((double) start * (double) total);
    std::int64_t endIdx = (std::int64_t) std::llround ((double) end   * (double) total);
    base   = std::clamp<std::int64_t> (base,   0, total);
    endIdx = std::clamp<std::int64_t> (endIdx, 0, total);
    if (endIdx <= base)
        return nullptr;   // 空トリム

    // 入力(トリム範囲)ピーク（メイクアップゲイン算出用）
    float srcPeak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        for (std::int64_t i = base; i < endIdx; ++i)
            srcPeak = std::max (srcPeak, std::abs ((float) src->sampleAtRaw (ch, i)));

    const std::int64_t n = endIdx - base;   // トリム範囲の長さ
    // 目標出力長（Manualストレッチ等で長さが変わる）。set_tempo(timeRatio) → 出力 ≈ n / timeRatio。
    const std::int64_t expectedLen = timeRatio > 1.0e-6
        ? (std::int64_t) std::llround ((double) n / timeRatio) : n;
    const std::int64_t maxLead = (std::int64_t) (0.3 * sr);   // 先頭で除去する上限

    std::vector<std::vector<float>> out;
    auto outLen = [&]() -> std::int64_t { return out.empty() ? 0 : (std::int64_t) out[0].size(); };

    if (useDirect)
    {
        // --- élastique DLL 直叩き（REAPER 外での代替。実験的・再配布不可）---
        // REAPER のモード/サブモード番号とは体系が違うので、専用パラメータで選ぶ。
        // ストレッチ / モードごとの音程範囲外 / フォルマントは非対応。その場合は空が返るので
        // キャッシュを作らず nullptr を返す＝規約15どおり Varispeed 再生にフォールバックする。
        out = elastique->renderOffline (*src, base, n, numCh, sr, shift, timeRatio,
                                        elaMode == 1 ? ElastiqueDirect::Soloist
                                                     : ElastiqueDirect::Pro);
        if (out.empty() || out[0].empty())
            return nullptr;
    }
    else
    {
    ps->set_srate (sr);
    ps->set_nch (numCh);
    ps->set_shift (shift);
    // フォルマント: set_formant_shift(0.0) は「フォルマント保持モードで0シフト」を起動し、
    // élastique の出力をほぼ無音にする（実測: outPeak が 1/100 に低下）。API仕様では shift<0 は
    // 「保持モード時のみ適用（＝通常のピッチシフト・フォルマント非操作）」なので、シフト無し(0)や
    // 下方向は負値で渡して実質オフ＝フル出力にする。正の値のときだけ明示的にフォルマントを上げる。
    ps->set_formant_shift (formant > 0.05f ? (double) formant : -1.0);
    ps->set_tempo (timeRatio > 0.0 ? timeRatio : 1.0);   // ストレッチ(長さ)を焼き込む
    ps->SetQualityParameter ((mode << 16) + sub);
    ps->Reset();

    // --- オフライン・レンダリング（プローブ非依存の堅牢版）---
    const int chunk = 1024;
    std::vector<double> pull ((std::size_t) chunk * (std::size_t) numCh, 0.0);
    out.assign ((std::size_t) numCh, {});
    for (auto& c : out) c.reserve ((std::size_t) (expectedLen + 2 * (std::int64_t) sr));

    auto drain = [&]()
    {
        for (;;)
        {
            const int got = ps->GetSamples (chunk, pull.data());
            if (got <= 0) break;
            for (int i = 0; i < got; ++i)
                for (int ch = 0; ch < numCh; ++ch)
                    out[(std::size_t) ch].push_back ((float) pull[(std::size_t) (i * numCh + ch)]);
        }
    };
    // 1) 実入力を供給（トリム範囲のみ）
    for (std::int64_t pos = 0; pos < n; pos += chunk)
    {
        const int c = (int) std::min<std::int64_t> (chunk, n - pos);
        if (ReaSample* b = ps->GetBuffer (c))
        {
            for (int i = 0; i < c; ++i)
                for (int ch = 0; ch < numCh; ++ch)
                    b[(std::size_t) (i * numCh + ch)] = (ReaSample) src->sampleAtRaw (ch, base + pos + i);
            ps->BufferDone (c);
        }
        drain();
    }

    // 2) 内部に溜まった本体を無音で押し出す（大レイテンシモード対策）。
    // 実音が揃う分（expLen＋先頭遅延＋余白）まで出れば十分なので
    // そこで止める（高速化）。tempo=1 だと供給した無音がそのまま 1:1 で出力に混ざるため、target で
    // 打ち切らないと無駄に大量の無音を処理してしまう。target・出力停止・上限のいずれかで終了。
    const std::int64_t target     = expectedLen + maxLead + (std::int64_t) (0.25 * sr);
    const std::int64_t maxSilence = std::max (expectedLen, n) + 2 * (std::int64_t) sr;  // 上限（保険）
    std::int64_t silenceFed = 0;
    int emptyStreak = 0;
    while (silenceFed < maxSilence && outLen() < target)
    {
        if (ReaSample* b = ps->GetBuffer (chunk))
        {
            std::memset (b, 0, sizeof (ReaSample) * (std::size_t) chunk * (std::size_t) numCh);
            ps->BufferDone (chunk);
        }
        silenceFed += chunk;
        const std::int64_t before = outLen();
        drain();
        if (outLen() == before) { if (++emptyStreak >= 16) break; }   // 持続的に無出力なら完了
        else emptyStreak = 0;
    }
    ps->FlushSamples();
    drain();
    delete ps;
    }   // else（REAPER API 経路）ここまで

    // 3) 実音のオンセット（頭）を自動検出して整列（先頭の遅延/無音を除去, 上限 maxLead）
    const std::int64_t avail = outLen();
    float peak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        for (float v : out[(std::size_t) ch]) peak = std::max (peak, std::abs (v));

    std::int64_t onset = 0;
    if (useDirect)
    {
        // **élastique 直読みは実測の固有遅延で揃える。**
        // 振幅の閾値で「音の頭」を探すと、素材が最初から大きいときに実レイテンシより
        // 手前で反応し、その差のぶん中身が前倒しになって末尾が欠ける。
        // 実測: 原音の末尾に置いた 100ms の無音がキャッシュでは 61ms しか残らず、
        // 39ms ぶん失われていた。波形の途中で終わるので発音のたびにプチッと鳴る。
        onset = std::clamp<std::int64_t> (
                    elastique->latencySamples (numCh, sr,
                        elaMode == 1 ? ElastiqueDirect::Soloist : ElastiqueDirect::Pro),
                    0, std::max<std::int64_t> (0, avail - 1));
    }
    else
    {
        // REAPER 経路は遅延を問い合わせる手段が無いので従来どおり閾値で探す。
        const float onsetThr = std::max (0.0005f, peak * 0.02f);
        for (std::int64_t i = 0, lim = std::min (avail, maxLead); i < lim; ++i)
        {
            float mx = 0.0f;
            for (int ch = 0; ch < numCh; ++ch) mx = std::max (mx, std::abs (out[(std::size_t) ch][(std::size_t) i]));
            if (mx > onsetThr) { onset = i; break; }
        }
    }
    const std::int64_t len = std::max<std::int64_t> (0, std::min (expectedLen, avail - onset));

    // メイクアップゲイン: élastique 等は時間圧縮/ピッチシフト時に全体レベルを落とすことがある
    // （特にノイズ質感の素材を強く圧縮するとグレインの位相が揃わず peak が下がる）。
    // 出力ピークを入力ピークまで持ち上げてレベルを保つ。増幅のみ（減衰はしない）。
    // ほぼ無音の取りこぼしを過剰ブーストしないよう上限を設ける。
    float makeup = 1.0f;
    if (peak > 1.0e-4f && srcPeak > 1.0e-4f)
        makeup = std::clamp (srcPeak / peak, 1.0f, 8.0f);

    auto sb = std::make_shared<SampleBuffer>();
    sb->numChannels        = numCh;
    sb->sampleRate         = sr;
    sb->originalSampleRate = sr;
    sb->name               = src->name + "_shift";
    sb->data.assign ((std::size_t) numCh, std::vector<float> ((std::size_t) len, 0.0f));
    for (int ch = 0; ch < numCh; ++ch)
        for (std::int64_t i = 0; i < len; ++i)
            sb->data[(std::size_t) ch][(std::size_t) i] = makeup * out[(std::size_t) ch][(std::size_t) (onset + i)];

    // **先頭をゼロから始める。**
    // 上のオンセット検出は「ピークの2%を超えた最初のサンプル」で切るので、
    // 出来上がったバッファは構造上かならず振幅 2%(-34dB) から始まる。
    // これを無音の直後に鳴らすと段差になり、発音のたびにプチッと入る（実測: 先頭値が
    // ピーク比 2.1%）。オンセットの位置は音の頭を保つために動かしたくないので、
    // ごく短いフェードインで段差だけを消す。
    //
    // ゼロ交差へ吸着させる手もあるが、チャンネルごとに交差位置が違うので
    // ステレオだと片側に段差が残る。フェードなら全チャンネルで確実にゼロから始まる。
    // 長さは 1ms。アタックの立ち上がりに対しては十分短く、段差を消すには十分長い。
    {
        const std::int64_t fade = std::min<std::int64_t> (len, (std::int64_t) (sr * 0.001));
        for (int ch = 0; ch < numCh; ++ch)
            for (std::int64_t i = 0; i < fade; ++i)
                sb->data[(std::size_t) ch][(std::size_t) i] *= (float) i / (float) fade;

        // 終端も同じ理由でゼロに落とす。切り出し位置が波形の途中に来ると、
        // そこから無音へ落ちて発音の終わりにプチッと入る（実測: 原音が 0 で終わる素材でも
        // キャッシュはピーク比 80% で終わっていた）。
        for (int ch = 0; ch < numCh; ++ch)
            for (std::int64_t i = 0; i < fade; ++i)
                sb->data[(std::size_t) ch][(std::size_t) (len - 1 - i)] *= (float) i / (float) fade;
    }

    sb->numSamples = len;
    return sb;
}

} // namespace otomad
