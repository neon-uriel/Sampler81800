#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "test_helpers.h"
#include "core/Voice.h"

using namespace otomad;

// ============================================================================
// Voice レベルの疎通テスト。
//
// 既存のエンジン単体テストは「必ず prepare 済みのエンジン」を直接叩くので、
// **Voice::prepare への登録漏れを検出できない**。実際 Granular / StretchLib を
// 追加したとき prepare を呼び忘れ、UI でそのアルゴリズムを選んだ瞬間に
// 未初期化のバッファ（空の vector / nullptr の窓）へ書き込んで落ちた。
//
// ここでは全アルゴリズムを Voice 経由で実際に鳴らし、
//   - 落ちないこと（＝prepare されていること）
//   - 音が出ること（＝無音を返していないこと。規約15）
//   - 報告レイテンシが実体と一致すること
// を確かめる。エンジンを追加したらこのテストが自動的に守る。
// ============================================================================

namespace
{
constexpr double SR = 48000.0;
constexpr int    BLOCK = 512;

// 使用しているアルゴリズム選択肢の数（Params.h の StringArray と一致させる）
constexpr int kNumAlgorithms = 6;

const char* algoName (int a)
{
    switch (a)
    {
        case 0: return "Varispeed";
        case 1: return "WSOLA";
        case 2: return "Phase Vocoder";
        case 3: return "Granular";
        case 4: return "Stretch Library";
        case 5: return "REAPER Shifter";
        default: return "?";
    }
}

// 1ボイスを algorithm で鳴らして出力のピークを返す
double renderPeak (int algorithm, EngineResources& res, const SampleBuffer& src, int outLen)
{
    Voice v;
    v.prepare (SR, BLOCK, 1, res, nullptr);   // REAPER API は無し（非REAPERホスト相当）

    Voice::Params p;
    p.rootKey = 60;
    p.gainLin = 1.0f;
    v.setParams (p);
    v.setAdsr (0.001f, 0.010f, 1.0f, 0.010f);

    Voice::EngineControl ec;
    ec.algorithm = algorithm;
    ec.durationMode = 0;      // Natural
    v.setEngineControl (ec);

    v.noteOn (&src, 67, 0.9f, 0.0f, 1.0f, false, false, 67.0f);   // +7半音

    std::vector<float> out ((std::size_t) outLen, 0.0f);
    for (int pos = 0; pos < outLen; pos += BLOCK)
    {
        const int nn = std::min (BLOCK, outLen - pos);
        float* ptr = out.data() + pos;
        float* ptrs[1] = { ptr };
        v.render (ptrs, 1, nn);
    }

    double peak = 0.0;
    for (float s : out) peak = std::max (peak, (double) std::abs (s));
    return peak;
}
}

TEST_CASE ("every algorithm renders through Voice without crashing", "[voice][engines]")
{
    EngineResources res;
    res.prepare (SR);
    auto src = test::makeSine (440.0, SR, 1.0);

    for (int algo = 0; algo < kNumAlgorithms; ++algo)
    {
        INFO ("algorithm " << algo << " (" << algoName (algo) << ")");
        // StretchLib はレイテンシが大きい（120ms 前後）ので長めに回す
        const double peak = renderPeak (algo, res, src, (int) (SR * 0.8));
        REQUIRE (peak > 0.01);   // 無音を返していない（規約15）
        REQUIRE (peak < 8.0);    // 発散していない
    }
}

TEST_CASE ("reported latency matches the engine actually used", "[voice][engines]")
{
    // pickEngine と getReportedLatency が別々に書かれていると、実際に鳴るエンジンと
    // 申告レイテンシが食い違って発音タイミングがズレる（Granular / StretchLib で実際に発生）。
    // 両者が同じ対応表を見ていることを、値の妥当性で間接的に確認する。
    EngineResources res;
    res.prepare (SR);

    Voice v;
    v.prepare (SR, BLOCK, 1, res, nullptr);

    // Varispeed は生演奏用なのでレイテンシ0、長さ保持系は0より大きい
    REQUIRE (v.getReportedLatency (0) == 0);
    for (int algo : { 1, 2 })
    {
        INFO ("algorithm " << algo << " (" << algoName (algo) << ")");
        REQUIRE (v.getReportedLatency (algo) > 0);
    }

    // Granular / StretchLib は無効化中（engineForAlgorithm が nullptr を返す）。
    // 選択肢は state 互換のため残っているので、選ばれたら PV へフォールバックする。
    // 有効化し直したらこの2行を消して上のループに戻すこと。
    REQUIRE (v.getReportedLatency (3) == v.getReportedLatency (2));
    REQUIRE (v.getReportedLatency (4) == v.getReportedLatency (2));
}

TEST_CASE ("switching algorithms mid-note does not crash", "[voice][engines]")
{
    // UI でアルゴリズムを切り替えると、鳴っている最中に setEngineControl が来る。
    EngineResources res;
    res.prepare (SR);
    auto src = test::makeSine (440.0, SR, 1.0);

    Voice v;
    v.prepare (SR, BLOCK, 1, res, nullptr);
    v.setAdsr (0.001f, 0.010f, 1.0f, 0.050f);
    v.noteOn (&src, 60, 0.9f, 0.0f, 1.0f, false, false, 60.0f);

    std::vector<float> out ((std::size_t) BLOCK, 0.0f);
    for (int algo = 0; algo < kNumAlgorithms; ++algo)
    {
        Voice::EngineControl ec;
        ec.algorithm = algo;
        v.setEngineControl (ec);

        for (int i = 0; i < 4; ++i)
        {
            float* ptrs[1] = { out.data() };
            v.render (ptrs, 1, BLOCK);
            for (float s : out) REQUIRE (std::isfinite (s));   // NaN/Inf を出していない
        }
    }
}

TEST_CASE ("cached playback uses varispeed from the very first note", "[voice][engines]")
{
    // ホストは 1ブロックの中で「updateVoiceParams → MIDIイベント処理 → レンダリング」
    // の順に進む。つまり setEngineControl は**発音より前**にしか来ない。
    //
    // キャッシュ経路（REAPER Shifter が事前レンダした素材を Varispeed で読む）では
    // noteOn の時点で初めて forcedAlgorithm が Varispeed になるが、startNote が activeEngine を
    // 選び直していないと、その音は直前の設定＝REAPER Shifter のフォールバック先である
    // Phase Vocoder のまま鳴り始める。エンベロープも PV のレイテンシぶん遅らされるので、
    // プラグインを読み込んだ直後の音だけ頭が欠けて聞こえる。
    EngineResources res;
    res.prepare (SR);
    auto src = test::makeSine (440.0, SR, 1.0);

    auto renderFirstBlock = [&res, &src] (bool refreshAfterNoteOn)
    {
        Voice v;
        v.prepare (SR, BLOCK, 1, res, nullptr);

        Voice::Params p;
        p.rootKey = 60;
        p.gainLin = 1.0f;
        v.setParams (p);
        v.setAdsr (0.001f, 0.010f, 1.0f, 0.010f);

        Voice::EngineControl ec;
        ec.algorithm    = 5;   // REAPER Shifter（非REAPERホスト → PV へフォールバック）
        ec.durationMode = 0;
        v.setEngineControl (ec);

        v.noteOn (&src, 60, 0.9f, 0.0f, 1.0f, false, false, 60.0f, Voice::NoteOptions { /*Varispeed*/ 0, 0.0f, 0 });
        if (refreshAfterNoteOn)
            v.setEngineControl (ec);   // 次のブロックで来る更新（＝2音目以降の状態）

        std::vector<float> out ((std::size_t) BLOCK, 0.0f);
        float* ptrs[1] = { out.data() };
        v.render (ptrs, 1, BLOCK);
        return out;
    };

    const auto first = renderFirstBlock (false);   // 読み込み直後の1音目
    const auto warm  = renderFirstBlock (true);    // 2音目以降と同じ状態

    double peak = 0.0;
    for (float s : first) peak = std::max (peak, (double) std::abs (s));
    REQUIRE (peak > 0.01);   // 頭が無音になっていない

    // 1音目と2音目以降で音が変わってはいけない
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        INFO ("sample " << i);
        REQUIRE (std::abs (first[i] - warm[i]) < 1.0e-6f);
    }
}

TEST_CASE ("alignLatency delays a zero-latency voice by exactly that many samples", "[voice][engines]")
{
    // キャッシュ再生（Varispeed, 遅延0）とフォールバック先（PV 等, 遅延N）を同じインスタンスで
    // 混ぜるとき、申告レイテンシは N になる。Varispeed 側は先頭に N の無音を置いて揃える。
    EngineResources res;
    res.prepare (SR);
    auto src = test::makeSine (440.0, SR, 1.0);

    auto renderNote = [&res, &src] (int align, int block)
    {
        Voice v;
        v.prepare (SR, BLOCK, 1, res, nullptr);
        Voice::Params p;
        p.rootKey = 60;
        p.gainLin = 1.0f;
        v.setParams (p);
        v.setAdsr (0.0f, 0.010f, 1.0f, 0.010f);
        v.setEngineControl ({});
        v.noteOn (&src, 60, 0.9f, 0.0f, 1.0f, false, false, 60.0f, Voice::NoteOptions { 0, 0.0f, align });

        std::vector<float> out ((std::size_t) BLOCK * 8, 0.0f);
        for (int pos = 0; pos < (int) out.size(); pos += block)
        {
            float* ptrs[1] = { out.data() + pos };
            v.render (ptrs, 1, std::min (block, (int) out.size() - pos));
        }
        return out;
    };

    const int align = 700;   // ブロック長の倍数でない値にして、ブロック内オフセットの経路を通す
    const auto plain   = renderNote (0, BLOCK);
    const auto delayed = renderNote (align, BLOCK);
    const auto delayed1 = renderNote (align, 1);   // 規約7: n == 1 でも同じ結果

    for (int i = 0; i < align; ++i)
        REQUIRE (delayed[(std::size_t) i] == 0.0f);

    for (std::size_t i = 0; i + (std::size_t) align < delayed.size(); ++i)
    {
        INFO ("sample " << i);
        REQUIRE (std::abs (delayed[i + (std::size_t) align] - plain[i]) < 1.0e-6f);
        REQUIRE (std::abs (delayed1[i + (std::size_t) align] - plain[i]) < 1.0e-6f);
    }
}
