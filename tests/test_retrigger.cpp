#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/SampleBuffer.h"
#include "core/VoiceManager.h"

using namespace otomad;

namespace
{
constexpr double SR    = 48000.0;
constexpr int    BLOCK = 128;

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
}

// ============================================================================
// 同じ音程の高速連打（音MADの「刻み」）でプチッと鳴らないこと。
//
// Poly では `v.isActive() && v.getNote() != note` を条件に requestSteal を
// 使い分けていたため、**同じ音程の連打だけが素通り**して noteOn が直接呼ばれ、
// 鳴っている波形の途中から新しい音の先頭へ無フェードで飛んでいた。
// 1/64 刻みはまさに同音連打なので、ほぼ 1 音に 1 回プチッと入る。
//
// Mono は元から無条件に requestSteal していたので無傷だった。つまり
// 「Mono では出ないが Poly では出る」という形でしか現れず、
// 単発ノートのテストでは原理的に捕まらない。
// ============================================================================
TEST_CASE ("fast same-note retrigger does not click", "[voice][retrigger]")
{
    // 140BPM の 1/64 ≒ 26.8ms
    const double step  = 60.0 / 140.0 / 16.0;
    const int    stepN = (int) (step * SR);

    // 低音寄りの素材。先頭も終端も 0 なので、段差が出たら切り替えのせい。
    const int n = (int) (SR * 1.0);
    SampleBuffer src;
    src.numChannels = 1;
    src.sampleRate  = SR;
    src.numSamples  = n;
    std::vector<float> v ((std::size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / SR;
        double a = 0.0;
        for (int h = 1; h <= 6; ++h) a += (0.6 / h) * std::sin (2.0 * 3.14159265358979 * 110.0 * h * t);
        v[(std::size_t) i] = (float) (0.5 * a);
    }
    src.data.assign (1, v);

    // 原音自身の最大隣接差分を基準にする（素材が急峻なら出力も急峻でよい）
    double srcStep = 0.0;
    for (int i = 1; i < n; ++i)
        srcStep = std::max (srcStep, std::abs ((double) v[(std::size_t) i] - v[(std::size_t) (i - 1)]));
    REQUIRE (srcStep > 1.0e-4);

    for (int algo = 0; algo < 6; ++algo)
    for (int poly : { 1, 8 })
    {
        INFO ("algorithm " << algo << " (" << algoName (algo) << ") / poly " << poly);

        VoiceManager vm;
        vm.prepare (SR, BLOCK, 1, nullptr);
        Voice::Params p;
        p.rootKey = 60;
        p.gainLin = 1.0f;
        vm.setVoiceParams (p);
        vm.setAdsr (0.004f, 0.100f, 1.0f, 0.009f);
        vm.setPoly (poly > 1, poly);
        Voice::EngineControl ec;
        ec.algorithm     = algo;
        ec.durationMode  = 2;      // Manual
        ec.stretchAmount = 1.0f;
        vm.setEngineControl (ec);

        const int notes  = 32;
        const int outLen = stepN * (notes + 8);
        std::vector<float> out ((std::size_t) outLen, 0.0f);

        int nextNote = 0, played = 0;
        for (int pos = 0; pos < outLen; pos += BLOCK)
        {
            const int nn = std::min (BLOCK, outLen - pos);
            while (played < notes && nextNote >= pos && nextNote < pos + nn)
            {
                vm.noteOn (60, 0.9f, &src, 0.0f, 1.0f, true);   // **毎回おなじ音程**
                ++played;
                nextNote += stepN;
            }
            float* ptr = out.data() + pos;
            float* ptrs[1] = { ptr };
            vm.render (ptrs, 1, nn);
        }

        double worst = 0.0;
        for (int i = 1; i < outLen; ++i)
            worst = std::max (worst, std::abs ((double) out[(std::size_t) i] - out[(std::size_t) (i - 1)]));

        double peak = 0.0;
        for (float s : out) peak = std::max (peak, (double) std::abs (s));
        REQUIRE (peak > 0.01);                // 鳴っている（規約15）
        REQUIRE (worst < srcStep * 3.0);      // 修正前は Varispeed/Poly で 17.7 倍だった
    }
}
