#pragma once

#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/SampleBuffer.h"

namespace otomad
{

//==============================================================================
/**
    élastique を DLL 直叩きで使うバックエンド（**実験的 / 再配布不可**）。

    REAPER 同梱の `elastique3.dll` を実行時ロードして、REAPER の外でも
    ピッチキャッシュのオフラインレンダに élastique を使えるようにする。

    公式 SDK が無いため、C export と vtable スロットを直接叩く：
      - `CreateInstance_E3(blockSize, channels, sampleRate, mode)` / `DestroyInstance_E3`
      - vtable[1] = ProcessData, vtable[5] = SetStretchQFactor, vtable[9] = Reset
      - ProcessData は起動プライミング中 rc=-1、出力が有効になると rc=0

    **重要**
    - élastique は zplane のプロプライエタリ技術で、REAPER にライセンスされたもの。
      DLL はリポジトリに含めず、ユーザーが自分の環境のパスを指定したときだけ動く。
    - オフラインレンダ専用。リアルタイム経路（Duration=Sync）では使わない
      （プライミング・FIFO・レイテンシ補償が必要になり、別物の複雑さになる）。
    - DLL が無い / ロード失敗なら `isAvailable()` は false のまま。呼び出し側は
      従来どおりフォールバックする（規約15: 無音を返さない）。
*/
class ElastiqueDirect
{
public:
    ElastiqueDirect() = default;
    ~ElastiqueDirect();

    ElastiqueDirect (const ElastiqueDirect&) = delete;
    ElastiqueDirect& operator= (const ElastiqueDirect&) = delete;

    /** DLL を読み込む。空文字なら既定の候補パスを順に試す。
        成功したら true。メッセージスレッドから呼ぶこと。 */
    bool load (const std::string& dllPath);
    void unload();

    bool isAvailable() const noexcept { return lib != nullptr; }
    const std::string& getLoadedPath() const noexcept { return loadedPath; }

    /** REAPER が入っていそうな既定の候補パス（先頭優先）。UI の初期値に使う。 */
    static std::vector<std::string> defaultCandidates();

    /** CreateInstance_E3 の第4引数。**SDK が無いので番号と実装の対応は実測で決めている。**

        実測（220Hz 倍音列・ステレオ 1.5秒、シフト無しで固有遅延、+7半音で音程と多声）:

          mode | 遅延      | 多声 | 音程 | 1半音の処理時間 | 判定
          -----+-----------+------+------+-----------------+--------------------------
             0 | 4608      | OK   | 正確 | 43 ms           | 最も重く遅延も最大 → Pro
             1 | 4096      | OK   | 正確 | 27 ms           | 中間
             2 | 4096      | OK   | 正確 | 14 ms           | 最も軽い。5,6 と数値が完全一致
             3 | 0         | 単声 | ---  | 24 ms           | SOLO
             4 | -64       | ---  | 1oct下| ---            | 壊れている。使わない
          5 以上 | 2 と同一 |      |      |                 | 範囲外→既定へ丸められる

        DLL の RTTI には CElastiqueProV3 / CElastiqueV3(Eff) / CElastiqueEffV3mobile /
        CelastiqueSOLOV3 の4系統があり、それぞれ Direct 版も持つ。うちが叩いている
        ProcessData は入出力 1:1 なので Direct 系にあたる。

        - `Pro`     (0): 多声OK。遅延 4608、処理時間は mode 2 の約3倍。音質が最も良い。
        - `Soloist` (3): 単声専用。和音だと片方の声部が消える（実測: 上声が 8% まで落ちる）。

        以前は Pro を 2 にしていたが、2 は最も軽く 5/6 と数値が一致することから
        既定へ丸められている疑いがあり、実際 0 に変えたほうが音が良いと確認できた。
        「mode 0/1 は3オクターブ下がる」という以前の記述は誤りで、どちらも正確に鳴る。
    */
    enum Mode { Pro = 0, Soloist = 3 };

    /** そのモードで音程・レベルが信用できる半音範囲（実測）。範囲外は空を返す。 */
    // 使える範囲はモードごとに違う。**変えるときは必ず実測すること。**
    //
    // 実測（220Hz の倍音入りを -48..+48 でレンダし、期待周波数の近傍で自己相関。
    //       このとき usableSemitoneRange の関門は -96..+96 に開けて DLL の素の挙動を見る）:
    //   Pro     : -39..+48 で正しい音程。-40 以下は出力は出るが音程がクランプされて
    //             +400〜500 cent ずれる（＝頼んだのと違う音になる）。
    //   Soloist : -48..+48 まで正しい（誤差 -16 cent、ピークも 0.555 で一定）。
    //             ただし下限 -17 は「レベルが落ちる」という別の理由で置いたもので、
    //             合成音でしか確かめていないため実素材で確認するまで広げない。
    //
    // 注意: この関門はヘッダに inline で書いてあるが renderOffline は .cpp にある。
    //       プローブを組むときに片方だけ古いオブジェクトを使うと、
    //       「DLL の限界」ではなく「自分の関門」を測ってしまう（実際にやらかした）。
    static void usableSemitoneRange (Mode mode, int& lo, int& hi) noexcept
    {
        if (mode == Soloist) { lo = -17; hi = 41; }   // 下限は makeup gain(最大8倍)で救える限界
        else                 { lo = -39; hi = 48; }   // Pro
    }

    /** オフラインで [base, base+n) をピッチシフトして返す。
        pitch は周波数比（2^(semi/12)）、timeRatio は「入力長/出力長」（1.0 で長さ維持）。
        失敗したら空を返す。背景スレッドから呼ぶ。 */
    std::vector<std::vector<float>> renderOffline (const SampleBuffer& src,
                                                   std::int64_t base, std::int64_t n,
                                                   int numChannels, double sampleRate,
                                                   double pitch, double timeRatio,
                                                   Mode mode) const;

    /** そのモード/SR/チャンネル数での固有遅延（サンプル）。初回に実測してキャッシュする。

        振幅の閾値で「音の頭」を探す方式は使えない。素材が最初から大きいと閾値が
        実レイテンシより手前で反応し、その差のぶん中身が前倒しになって**末尾が欠ける**
        （実測: 原音の末尾に置いた 100ms の無音がキャッシュでは 61ms しか残らず、
        39ms ぶん失われていた。そのせいで波形の途中で終わり、発音のたびにプチッと鳴る）。
        無音→バーストを流して遅延そのものを測れば、素材によらず正しく揃う。 */
    int latencySamples (int numChannels, double sampleRate, Mode mode) const;

private:
    void* lib = nullptr;          // HMODULE（windows.h をヘッダに持ち込まない）
    void* createFn  = nullptr;
    void* destroyFn = nullptr;
    std::string loadedPath;

    // キャッシュ生成は複数の背景スレッドで並列に走るが、この DLL のスレッド安全性は
    // 公式に保証されていない（SDK が無く RE で使っている）。実験機能なので安全側に倒し、
    // レンダを直列化する。並列度が落ちるだけで結果は変わらない。
    mutable std::mutex renderLock;
    // 実測した固有遅延のキャッシュ。キーは (mode, サンプルレート, チャンネル数)。
    mutable std::map<std::tuple<int, int, int>, int> latencyCache;
};

} // namespace otomad
