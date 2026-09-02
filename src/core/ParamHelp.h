#pragma once

#include <cstddef>
#include <cstring>

#include "core/Params.h"

namespace otomad::params
{

//==============================================================================
/**
    パラメータIDごとのホバーヘルプ（日本語）。

    ネイティブ版エディタ（`setTooltip`）と Web UI 版（`paramHelp` ネイティブ関数経由）の
    **両方がここを見る**。もとは PluginEditor.cpp の static 関数だったが、Web UI に
    移植した際に片方だけになって「ホバーヘルプが出ない」状態になったので共通化した。
    パラメータを足したらここにも1行足すこと。

    ソースは /utf-8 でコンパイルするので日本語リテラルを直接書いてよい。
*/
struct HelpEntry { const char* id; const char* text; };

inline const HelpEntry* helpTable (std::size_t& count) noexcept
{
    static const HelpEntry table[] =
    {
        { pitchSemi,     "ピッチ（半音単位）" },
        { pitchCents,    "ピッチの微調整（セント）" },
        { octave,        "オクターブ変更（±4）" },
        { rootKey,       "ルート鍵盤：この音で原音のピッチになる" },
        { gain,          "出力ゲイン" },
        { attack,        "アタック：発音から最大音量までの時間" },
        { decay,         "ディケイ：最大からサステイン音量へ減る時間" },
        { sustain,       "サステイン：鍵を押し続けた時の音量" },
        { release,       "リリース：離鍵後に音が消える時間" },
        { sampleStart,   "再生開始位置（トリム頭）" },
        { sampleEnd,     "再生終了位置（トリム尻）" },
        { portaTime,     "グライド（ポルタメント）時間" },
        { portaCurve,    "グライドのカーブ（Timeモード）" },
        { glideGroupMs,  "和音とみなす時間窓（ポリグライド）" },
        { maxVoices,     "最大同時発音数（Poly時）" },
        { bendRange,     "ピッチベンド幅（半音）" },
        { stretchAmount, "長さ倍率（Duration=Manual 時）" },
        { formant,       "フォルマントシフト（REAPER、正の値のみ有効）" },
        { interpQuality, "補間品質（Linear / Hermite）" },
        { portaMode,     "Off / Legato（重なり時）/ Always（常に）" },
        { portaShape,    "グライド形状：Time（曲線）/ Analog（指数）" },
        { polyMode,      "Mono（単音）/ Poly（和音）" },
        { algorithm,     "ピッチ変更アルゴリズム。Varispeed は長さも変わる。他は長さを保つ" },
        { durationMode,  "長さ制御：Natural（原音のまま）/ Sync（テンポ同期）/ Manual（倍率指定）" },
        { syncLength,    "同期長（Duration=Sync 時）" },
        { snapZeroCross, "トリム端をゼロクロスへ吸着（プチ防止）" },
        { phaseLock,     "位相ロック（Phase Vocoder）" },
        { reaperMode,    "REAPER ピッチシフタのモード（élastique / SoundTouch など）" },
        { reaperSubMode, "選択したモードのサブモード" },
        // Web UI で追加されたもの（ネイティブ版には無い）
        { cacheFallback, "Shifter の範囲外・生成待ちの音を鳴らすエンジン" },
        { elastiqueMode, "REAPER 外で elastique を直読みするときのアルゴリズム。"
                         "通常は Elastique Pro。Soloist は単声専用で、"
                         "シフトすると倍音がほとんど落ちて音が籠る" },
        { vibDepth,      "ビブラートの深さ（セント）。0 で無効" },
        { vibRate,       "ビブラートの速さ" },
        { vibDelay,      "発音してからビブラートが効き始めるまでの時間" },
        { vibFade,       "効き始めてから最大の深さに達するまでの時間" },
    };
    count = sizeof (table) / sizeof (table[0]);
    return table;
}

/** 見つからなければ空文字列を返す（null は返さない）。 */
inline const char* helpFor (const char* id) noexcept
{
    if (id == nullptr) return "";
    std::size_t n = 0;
    const auto* t = helpTable (n);
    for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp (t[i].id, id) == 0)
            return t[i].text;
    return "";
}

} // namespace otomad::params
