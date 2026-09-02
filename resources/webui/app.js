// OtoMadSampler Web UI
// JUCE の WebSliderRelay / WebComboBoxRelay / WebToggleButtonRelay でパラメータを双方向バインドし、
// 波形・D&D・鍵盤などは getNativeFunction / backend イベントで橋渡しする。

const statusEl = document.getElementById ("status");
const errEl    = document.getElementById ("err");

// status は毎tick backend に上書きされるので、診断は専用要素に出す
function note (msg) { if (errEl) errEl.textContent = msg; }
function showError (msg) { note ("JS error: " + msg); }
window.addEventListener ("error", (e) => showError (e.message));
window.addEventListener ("unhandledrejection", (e) => showError (String (e.reason)));

// ---- アクセント色（設定で変えられるので変数で持つ） ----
let ACCENT = "#4bb4f5", ACCENT_HI = "#7fd0ff", ACCENT_LO = "#1d7fbe";

const hexToRgb = (h) => {
  const m = /^#?([0-9a-f]{6})$/i.exec (h.trim());
  const v = m ? parseInt (m[1], 16) : 0x4bb4f5;
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
};
const rgbToHex = (r, g, b) =>
  "#" + [r, g, b].map (x => Math.round (Math.min (255, Math.max (0, x))).toString (16).padStart (2, "0")).join ("");
const shade = (hex, t) => {   // t>0 で白へ / t<0 で黒へ
  const [r, g, b] = hexToRgb (hex);
  const to = t > 0 ? 255 : 0, a = Math.abs (t);
  return rgbToHex (r + (to - r) * a, g + (to - g) * a, b + (to - b) * a);
};
const rgba = (hex, a) => { const [r, g, b] = hexToRgb (hex); return "rgba(" + r + "," + g + "," + b + "," + a + ")"; };

function applyAccent (hex) {
  ACCENT = hex; ACCENT_HI = shade (hex, .35); ACCENT_LO = shade (hex, -.35);
  const root = document.documentElement.style;
  root.setProperty ("--accent", ACCENT);
  root.setProperty ("--accent-lo", ACCENT_LO);
  if (window.drawWave) window.drawWave();
  if (window.drawAdsr) window.drawAdsr();
  if (window.drawVib)  window.drawVib();
}

// ---- 表示フォーマット ----
const NOTE_NAMES = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
const noteName = (n) => NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor (n / 12) - 1);
const fmtMs = (v) => v >= 1000 ? (v / 1000).toFixed (2) + " s" : v.toFixed (0) + " ms";

const FMT = {
  pitchSemi:  v => v.toFixed (0),
  pitchCents: v => v.toFixed (1),
  octave:     v => (v > 0 ? "+" : "") + v.toFixed (0),
  rootKey:    v => noteName (Math.round (v)),
  gain:       v => v.toFixed (1) + " dB",
  attack: fmtMs, decay: fmtMs, release: fmtMs, portaTime: fmtMs,
  sustain:       v => v.toFixed (3),
  sampleStart:   v => v.toFixed (3),
  sampleEnd:     v => v.toFixed (3),
  portaCurve:    v => v.toFixed (2),
  maxVoices:     v => v.toFixed (0),
  glideGroupMs:  fmtMs,
  bendRange:     v => v.toFixed (0),
  stretchAmount: v => v.toFixed (2) + "x",
  formant:       v => v.toFixed (1),
  vibDepth:      v => v.toFixed (1) + " ct",
  vibRate:       v => v.toFixed (2) + " Hz",
  vibDelay: fmtMs, vibFade: fmtMs,
};

// 各リレーの state をここに集約（他のUI部品からも参照する）
const S = {};

import ("./juce-index.js").then ((juce) => {
  const { getSliderState, getToggleState, getComboBoxState, getNativeFunction } = juce;

  // DOM 参照は最初にまとめて取る。
  // （ノブの初期 render() が drawAdsr()/drawWave() を呼ぶため、後で const 宣言すると
  //   temporal dead zone で例外になり、以降の初期化が全部止まってしまう）
  const waveCanvas = document.getElementById ("wave");
  const waveWrap   = document.getElementById ("wave-wrap");
  const dropHint   = document.getElementById ("drop-hint");
  const nameEl     = document.getElementById ("sample-name");
  const adsrCanvas = document.getElementById ("adsr");
  const keysEl     = document.getElementById ("keys");
  const fileInput  = document.getElementById ("file-input");
  const selRMode   = document.getElementById ("sel-rmode");
  const selRSub    = document.getElementById ("sel-rsub");

  // 描画関数から参照される状態も、同じ理由（TDZ）でここで初期化しておく
  const vibCanvas = document.getElementById ("vib");
  const C = {};                              // コンボの state（グレーアウト判定で参照）
  let reaperAvailable = false;
  let elastiqueLoaded = false;   // status イベントで更新（非REAPERでのシフター有効判定）
  const keyEls = new Map();                  // MIDIノート → 鍵盤要素
  let peaks = null, normGain = 1;            // 波形ピーク
  let view = { start: 0, end: 1 };           // 波形の表示窓（全体に対する割合）
  let contour = null;                        // ピッチ曲線（PITCH ボタンで取得）
  let pendingLoadName = null, pendingLoadTimer = 0;   // 非同期デコードの完了待ち
  let sampleSeconds = 0;                              // 読み込んだ素材の長さ（秒）

  // パラメータ state の取得（無ければ生成）。
  // ノブが画面に無いパラメータ（sampleEnd など）もコードから使うので、ここで必ず作る。
  const st = (id) => S[id] || (S[id] = getSliderState (id));
  for (const id of ["sampleStart", "sampleEnd", "rootKey", "attack", "decay", "sustain", "release"])
    st (id);

  // ネイティブ関数も先に取得しておく（後段の初期化から参照されるため）
  const nfLoadSample    = getNativeFunction ("loadSample");
  const nfGetWaveform   = getNativeFunction ("getWaveform");
  const nfNormalize     = getNativeFunction ("normalize");
  const nfDetectRoot    = getNativeFunction ("detectRoot");
  const nfNote          = getNativeFunction ("note");
  const nfReaperModes   = getNativeFunction ("reaperModes");
  const nfSetReaperMode = getNativeFunction ("setReaperMode");
  const nfOpenReleases  = getNativeFunction ("openReleases");
  const nfResetParam    = getNativeFunction ("resetParam");
  const nfGetAppearance = getNativeFunction ("getAppearance");
  const nfSetAppearance = getNativeFunction ("setAppearance");
  const nfSetBgImage    = getNativeFunction ("setBgImage");
  const nfSaveAppearanceDefault = getNativeFunction ("saveAppearanceDefault");
  const nfGetSamples    = getNativeFunction ("getSamples");
  const nfSelectSample  = getNativeFunction ("selectSample");
  const nfRemoveSample  = getNativeFunction ("removeSample");
  const nfGetElastique    = getNativeFunction ("getElastique");
  const nfSetElastique    = getNativeFunction ("setElastique");
  const nfBrowseElastique = getNativeFunction ("browseElastique");
  const nfSpaceToHost     = getNativeFunction ("spaceToHost");
  const nfGetFfmpeg       = getNativeFunction ("getFfmpeg");
  const nfSetFfmpeg       = getNativeFunction ("setFfmpeg");
  const nfBrowseFfmpeg    = getNativeFunction ("browseFfmpeg");
  const nfFlatten         = getNativeFunction ("flatten");
  const nfRevertFlatten   = getNativeFunction ("revertFlatten");
  const nfPitchContour    = getNativeFunction ("pitchContour");
  const nfFlattenState    = getNativeFunction ("flattenState");
  const nfParamHelp       = getNativeFunction ("paramHelp");
  const nfReady           = getNativeFunction ("ready");

  //================================================================ knobs
  // SVG で描く（conic-gradient より線端・アンチエイリアスが綺麗）。
  // 円弧: 半径 R の円周を dasharray で切り、開始を左下 135° に回転させて 270° ぶん使う。
  const R = 21, CIRC = 2 * Math.PI * R, SWEEP = CIRC * 0.75;   // 270°
  const SVG_NS = "http://www.w3.org/2000/svg";

  function buildKnobSvg (knob) {
    const svg = document.createElementNS (SVG_NS, "svg");
    svg.setAttribute ("viewBox", "0 0 52 52");
    svg.innerHTML =
      '<defs><radialGradient id="knobFace" cx="50%" cy="32%" r="70%">' +
        '<stop offset="0%" stop-color="#464e57"/><stop offset="62%" stop-color="#1d242b"/>' +
        '<stop offset="100%" stop-color="#0e1216"/></radialGradient></defs>' +
      '<circle class="track" cx="26" cy="26" r="' + R + '" fill="none" stroke-width="4"' +
        ' stroke-linecap="round" stroke-dasharray="' + SWEEP + ' ' + CIRC + '"' +
        ' transform="rotate(135 26 26)"/>' +
      '<circle class="fill" cx="26" cy="26" r="' + R + '" fill="none" stroke-width="4"' +
        ' stroke-linecap="round" stroke-dasharray="0 ' + CIRC + '"' +
        ' transform="rotate(135 26 26)"/>' +
      '<circle class="cap" cx="26" cy="26" r="15.5"/>' +
      '<line class="ptr" x1="26" y1="15" x2="26" y2="20.5" transform="rotate(-135 26 26)"/>';
    knob.appendChild (svg);
    return { fill: svg.querySelector (".fill"), ptr: svg.querySelector (".ptr") };
  }

  const knobs = Array.from (document.querySelectorAll (".knob[data-relay]"));
  for (const knob of knobs) {
    const relay = knob.dataset.relay;
    const out   = knob.parentElement.querySelector (".knob-value");
    const state = st (relay);
    const fmt   = FMT[relay] || (v => String (v));
    const parts = buildKnobSvg (knob);

    const render = () => {
      const p = Math.min (1, Math.max (0, state.getNormalisedValue()));
      parts.fill.setAttribute ("stroke-dasharray", (SWEEP * p) + " " + CIRC);
      parts.ptr.setAttribute ("transform", "rotate(" + (-135 + 270 * p) + " 26 26)");
      if (out) out.textContent = fmt (state.getScaledValue());
      if (relay === "attack" || relay === "decay" || relay === "sustain" || relay === "release") drawAdsr();
      if (relay === "sampleStart" || relay === "sampleEnd") { drawWave(); updateStrip(); }
      if (relay === "rootKey") paintKeys();
      if (relay === "vibDepth") updateEnablement();
      if (relay.startsWith ("vib")) drawVib();
    };
    state.valueChangedEvent.addListener (render);
    state.propertiesChangedEvent.addListener (render);
    render();

    // ドラッグ中は正規化値をローカルに積算する。
    // state から読み直すと、整数系パラメータでは値がスナップされて微小移動が失われ、
    // 「動かない→急に飛ぶ」という不均一な動き（加速したような感触）になるため。
    let dragging = false, lastY = 0, dragNorm = 0;

    knob.addEventListener ("pointerdown", (e) => {
      dragging = true; lastY = e.clientY;
      dragNorm = state.getNormalisedValue();
      knob.classList.add ("dragging");
      knob.setPointerCapture (e.pointerId);
      state.sliderDragStarted();
    });
    knob.addEventListener ("pointermove", (e) => {
      if (! dragging) return;
      const dy = lastY - e.clientY; lastY = e.clientY;
      const speed = e.shiftKey ? 0.0006 : 0.005;   // 1px あたりの正規化変化量
      dragNorm = Math.min (1, Math.max (0, dragNorm + dy * speed));
      state.setNormalisedValue (dragNorm);
      render();
    });
    const end = () => { if (dragging) { dragging = false; knob.classList.remove ("dragging"); state.sliderDragEnded(); } };
    knob.addEventListener ("pointerup", end);
    knob.addEventListener ("pointercancel", end);

    // ダブルクリックで既定値へ（既定値は C++ 側のパラメータが持っている）
    knob.addEventListener ("dblclick", async () => {
      await nfResetParam (relay);
      render();
    });
  }

  //================================================================ combo / toggle
  // UI から隠す選択肢（値は C++ 側にそのまま残っている）。
  // Granular / Stretch Library はプラグイン上だと無音になる問題があるため外している。
  // 直ったらこの配列から抜くだけでよい。エンジン本体とテストは残してある。
  const HIDDEN_CHOICES = { algorithm: [3, 4] };

  for (const id of ["algorithm", "durationMode", "syncLength", "portaMode",
                    "polyMode", "portaShape", "interpQuality", "elastiqueMode", "cacheFallback"]) {
    const sel = document.getElementById ("sel-" + id);
    if (! sel) continue;
    const state = C[id] = getComboBoxState (id);

    const fill = () => {
      const items = state.properties.choices || [];
      const cur   = state.getChoiceIndex();
      const hide  = HIDDEN_CHOICES[id] || [];
      sel.innerHTML = "";
      items.forEach ((label, i) => {
        // 選択肢は C++ 側では消せない（正規化値で保存されるので、抜くと index がずれて
        // 既存プロジェクトの選択が別物に化ける。規約12）。UI からだけ隠す。
        // ただし今まさに選ばれている値は、隠す対象でも出す（空欄になって何が選ばれて
        // いるのか分からなくなるのを防ぐ）。
        if (hide.includes (i) && i !== cur) return;
        const o = document.createElement ("option");
        o.value = i; o.textContent = label;
        sel.appendChild (o);
      });
      sel.value = String (cur);
      updateEnablement();
    };
    state.propertiesChangedEvent.addListener (fill);
    state.valueChangedEvent.addListener (() => {
      const cur = state.getChoiceIndex();
      // 隠している選択肢に値が変わった場合（プリセット読み込み・オートメーション）、
      // 対応する option が無いので sel.value が空になり、コンボが空欄になる。
      // その値を含めて作り直す。
      if (! sel.querySelector ('option[value="' + cur + '"]')) { fill(); return; }
      sel.value = String (cur);
      updateEnablement();
    });
    fill();
    sel.addEventListener ("change", () => {
      state.setChoiceIndex (parseInt (sel.value, 10));
      updateEnablement();
    });
  }

  for (const id of ["phaseLock", "snapZeroCross"]) {
    const chk = document.getElementById ("chk-" + id);
    if (! chk) continue;
    const state = getToggleState (id);
    const render = () => { chk.checked = state.getValue(); };
    state.valueChangedEvent.addListener (render);
    state.propertiesChangedEvent.addListener (render);
    render();
    chk.addEventListener ("change", () => state.setValue (chk.checked));
  }

  //================================================================ グレーアウト
  // 規約#12: パラメータは常に存在させ、使えない場面では UI で無効化するだけにする。
  function dimKnob (relay, on) {
    const k = document.querySelector ('.knob[data-relay="' + relay + '"]');
    if (k) k.closest (".knob-cell").classList.toggle ("dim", ! on);
  }
  function dimEl (el, on) {
    if (! el) return;
    el.disabled = ! on;
    const wrap = el.closest (".field") || el.closest (".check") || el;
    wrap.classList.toggle ("dim", ! on);
  }

  function updateEnablement () {
    const algo = C.algorithm    ? C.algorithm.getChoiceIndex()    : 0;
    const dur  = C.durationMode ? C.durationMode.getChoiceIndex() : 0;
    const por  = C.portaMode    ? C.portaMode.getChoiceIndex()    : 0;

    const isReaper = (algo === 5);
    const isPV     = (algo === 2);

    // FORMANT は UI から外した（index.html 参照）。ここで dim を指定する対象も無い。
    dimKnob ("stretchAmount", dur === 2);      // Stretch は Manual のみ
    dimKnob ("portaTime",  por !== 0);         // Glide / Curve / Group は Porta が Off でないとき
    dimKnob ("portaCurve", por !== 0);
    dimKnob ("glideGroupMs", por !== 0);
    dimEl (document.getElementById ("sel-portaShape"), por !== 0);

    // ビブラートは Depth>0 のときだけ Rate/Delay/Fade が意味を持つ
    const vibOn = st ("vibDepth").getScaledValue() > 0.01;
    for (const id of ["vibRate", "vibDelay", "vibFade"]) dimKnob (id, vibOn);

    dimEl (document.getElementById ("sel-syncLength"), dur === 1);   // Sync 長は Duration=Sync のみ
    dimEl (document.getElementById ("chk-phaseLock"),  isPV);        // Phase Lock は Phase Vocoder のみ

    const rc = isReaper && reaperAvailable;
    dimEl (selRMode, rc); dimEl (selRSub, rc);
    // elastique 直読みのモードは「REAPER Shifter 選択中 かつ REAPER 外 かつ DLL 済み」でだけ意味を持つ
    dimEl (document.getElementById ("sel-elastiqueMode"),
           isReaper && ! reaperAvailable && elastiqueLoaded);
    // フォールバック先はキャッシュ経路（REAPER Shifter かつ Natural / Manual）でだけ意味を持つ
    dimEl (document.getElementById ("sel-cacheFallback"), isReaper && dur !== 1);

    // REAPER Shifter は Sync 非対応 → Duration の "Sync" を選べなくする。選択中なら Natural へ。
    const selDur = document.getElementById ("sel-durationMode");
    if (selDur && selDur.options.length > 1) selDur.options[1].disabled = isReaper;
    if (isReaper && dur === 1 && C.durationMode) C.durationMode.setChoiceIndex (0);
  }

  //---------------------------------------------------------------- waveform
  window.drawWave = drawWave;

  // トリムは波形上で操作する（sampleEnd にはノブが無い）ので、ここで再描画を購読する
  for (const id of ["sampleStart", "sampleEnd"]) {
    st (id).valueChangedEvent.addListener (() => { drawWave(); updateStrip(); });
    st (id).propertiesChangedEvent.addListener (() => { drawWave(); updateStrip(); });
  }

  function updateStrip () {
    const s = st ("sampleStart").getScaledValue(), e = st ("sampleEnd").getScaledValue();
    document.getElementById ("lbl-start").textContent = s.toFixed (3);
    document.getElementById ("lbl-end").textContent   = e.toFixed (3);
    const len = Math.max (0, e - s) * sampleSeconds;
    document.getElementById ("lbl-len").textContent = sampleSeconds > 0 ? len.toFixed (2) + " s" : "—";
  }
  function drawWave () {
    if (! waveCanvas) return;
    const c = waveCanvas, ctx = c.getContext ("2d");
    const w = c.width, h = c.height;
    const mid = h / 2, half = h * .44;
    ctx.clearRect (0, 0, w, h);

    // グリッド + センターライン（サンプルが無くても描いて“計器”らしくする）
    ctx.strokeStyle = "rgba(255,255,255,.045)"; ctx.lineWidth = 1;
    for (let i = 1; i < 8; ++i) {
      const x = Math.round (w * i / 8) + .5;
      ctx.beginPath(); ctx.moveTo (x, 0); ctx.lineTo (x, h); ctx.stroke();
    }
    for (const fy of [.25, .75]) {
      const y = Math.round (h * fy) + .5;
      ctx.beginPath(); ctx.moveTo (0, y); ctx.lineTo (w, y); ctx.stroke();
    }
    ctx.strokeStyle = "rgba(255,255,255,.10)";
    ctx.beginPath(); ctx.moveTo (0, Math.round (mid) + .5); ctx.lineTo (w, Math.round (mid) + .5); ctx.stroke();

    if (! peaks || ! peaks.length) return;

    const s01 = st ("sampleStart").getScaledValue();
    const e01 = st ("sampleEnd").getScaledValue();
    const toX = (p) => (p - view.start) / (view.end - view.start) * w;
    const sx = toX (s01), ex = toX (e01);

    // トリム外を暗く落とす（範囲が一目で分かる）
    ctx.fillStyle = "rgba(0,0,0,.55)";
    if (sx > 0) ctx.fillRect (0, 0, Math.min (sx, w), h);
    if (ex < w) ctx.fillRect (Math.max (0, ex), 0, w - Math.max (0, ex), h);

    // 波形（内側を明るく、外周をやや暗く）
    const n = peaks.length / 2;
    const grad = ctx.createLinearGradient (0, 0, 0, h);
    grad.addColorStop (0,  ACCENT_LO);
    grad.addColorStop (.5, ACCENT_HI);
    grad.addColorStop (1,  ACCENT_LO);
    ctx.strokeStyle = grad; ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = 0; x < w; ++x) {
      const pos = view.start + (x / w) * (view.end - view.start);
      const i = Math.min (n - 1, Math.max (0, Math.floor (pos * n)));
      const lo = Math.max (-1, Math.min (1, peaks[i * 2]     * normGain));
      const hi = Math.max (-1, Math.min (1, peaks[i * 2 + 1] * normGain));
      ctx.moveTo (x + .5, mid - hi * half);
      ctx.lineTo (x + .5, mid - lo * half + .5);
    }
    ctx.stroke();

    // ピッチ曲線（PITCH で取得したときだけ）。
    // 縦軸は「検出された音程の範囲」に自動スケールする。絶対音高より
    // 「どれだけ揺れているか」が見たいので、レンジは最低でも 4半音は確保する。
    if (contour && contour.ok && contour.midi && contour.midi.length && sampleSeconds > 0) {
      const vals = contour.midi.filter ((m) => m > 0);
      if (vals.length > 1) {
        let lo = Math.min.apply (null, vals), hi = Math.max.apply (null, vals);
        const mid2 = (lo + hi) / 2, span = Math.max (4, hi - lo + 1);
        lo = mid2 - span / 2; hi = mid2 + span / 2;

        const toY = (m) => h - (m - lo) / (hi - lo) * h;
        // 目標（スナップ先）の水平線
        if (contour.targetNote >= lo && contour.targetNote <= hi) {
          const y = Math.round (toY (contour.targetNote)) + .5;
          ctx.strokeStyle = "rgba(255,255,255,.35)"; ctx.lineWidth = 1;
          ctx.setLineDash ([4, 4]);
          ctx.beginPath(); ctx.moveTo (0, y); ctx.lineTo (w, y); ctx.stroke();
          ctx.setLineDash ([]);
        }
        // 曲線本体。無声(0)のフレームで線を切る
        ctx.strokeStyle = "#7cf5c8"; ctx.lineWidth = 1.5;
        ctx.beginPath();
        let pen = false;
        for (let i = 0; i < contour.midi.length; ++i) {
          const m = contour.midi[i];
          if (m <= 0) { pen = false; continue; }
          const tSec = contour.startSeconds + i * contour.hopSeconds;
          const x = toX (tSec / sampleSeconds);
          if (x < -4 || x > w + 4) { pen = false; continue; }
          const y = toY (m);
          if (pen) ctx.lineTo (x, y); else ctx.moveTo (x, y);
          pen = true;
        }
        ctx.stroke();
      }
    }

    // トリムのハンドル（縦線＋上下のつまみ）
    ctx.strokeStyle = "#ff9f4a"; ctx.fillStyle = "#ff9f4a"; ctx.lineWidth = 1;
    for (const p of [s01, e01]) {
      const x = toX (p);
      if (x < -6 || x > w + 6) continue;
      const xr = Math.round (x) + .5;
      ctx.beginPath(); ctx.moveTo (xr, 0); ctx.lineTo (xr, h); ctx.stroke();
      ctx.fillRect (xr - 3, 0, 6, 6);
      ctx.fillRect (xr - 3, h - 6, 6, 6);
    }
  }

  // 読み込み済みサンプルの一覧（切り替えて聴き比べる）
  const selSample = document.getElementById ("sel-sample");
  async function refreshSampleList () {
    const r = await nfGetSamples();
    const names = (r && r.names) || [];
    selSample.innerHTML = "";
    names.forEach ((nm, i) => {
      const o = document.createElement ("option");
      o.value = i; o.textContent = (i + 1) + ". " + (nm || "sample");
      selSample.appendChild (o);
    });
    selSample.value = String (r ? r.active : -1);
    // 1つ以下なら切り替える意味がないので隠す
    selSample.hidden = names.length < 2;
    document.getElementById ("btn-remove").hidden = names.length < 1;
  }
  selSample.addEventListener ("change", async () => {
    await nfSelectSample (parseInt (selSample.value, 10));
    // パラメータが丸ごと切り替わるので、グラフ類も描き直す（ノブ値はリレー経由で自動更新）
    refreshWave(); refreshSampleList(); refreshReaper();
    drawAdsr(); drawVib(); updateStrip(); paintKeys(); updateEnablement();
  });
  document.getElementById ("btn-remove").addEventListener ("click", async () => {
    await nfRemoveSample (parseInt (selSample.value, 10));
    refreshWave(); refreshSampleList();
  });

  let lastWaveKey = null;
  async function refreshWave () {
    const r = await nfGetWaveform();
    peaks    = r && r.peaks ? r.peaks : null;
    normGain = r && r.normGain ? r.normGain : 1;
    sampleSeconds = r && r.seconds ? r.seconds : 0;

    // 素材が変わったときだけズームを全体に戻す。
    // NORMALIZE でも version は進むので、毎回リセットすると表示位置を失う。
    const key = (r && r.name ? r.name : "") + "|" + sampleSeconds.toFixed (6);
    if (key !== lastWaveKey) { view = { start: 0, end: 1 }; lastWaveKey = key; }

    if (nameEl) nameEl.textContent = (r && r.name) ? r.name : "— no sample —";
    if (dropHint) dropHint.hidden = !! (peaks && peaks.length);
    drawWave(); updateStrip();
  }

  // 波形の操作: ホイールでズーム / 右ドラッグでスクロール / 左クリックで Start・End
  waveCanvas.addEventListener ("wheel", (e) => {
    e.preventDefault();
    if (! peaks) return;
    const fx = e.offsetX / waveCanvas.clientWidth;
    const anchor = view.start + fx * (view.end - view.start);
    const factor = e.deltaY < 0 ? 1 / 1.2 : 1.2;
    const span = Math.min (1, Math.max (0.0005, (view.end - view.start) * factor));
    // 変数名を st にしない: 外側の st(id) ヘルパを隠してしまい、
    // このブロックで st(...) を呼んだ瞬間に TDZ 例外になる（過去に何度も踏んだ罠）
    const viewStart = Math.min (Math.max (anchor - fx * span, 0), 1 - span);
    view = { start: viewStart, end: viewStart + span };
    drawWave();
  }, { passive: false });

  waveCanvas.addEventListener ("contextmenu", (e) => e.preventDefault());

  let panning = false, panX = 0, trimming = null;   // trimming は "sampleStart" | "sampleEnd"

  // トリム値を設定。Start と End が交差しないよう相手側でクランプする。
  function setTrim (which, pos) {
    const s = st ("sampleStart").getScaledValue();
    const e = st ("sampleEnd").getScaledValue();
    let v = Math.min (1, Math.max (0, pos));
    if (which === "sampleStart") v = Math.min (v, e - 0.001);
    else                         v = Math.max (v, s + 0.001);
    st (which).setNormalisedValue (Math.min (1, Math.max (0, v)));   // レンジ 0..1 なので値=正規化値
    drawWave();
  }
  waveCanvas.addEventListener ("pointerdown", (e) => {
    if (! peaks) { fileInput.click(); return; }   // 未読み込みならファイル選択を開く
    waveCanvas.setPointerCapture (e.pointerId);
    if (e.button !== 0) { panning = true; panX = e.clientX; return; }

    const fx = e.offsetX / waveCanvas.clientWidth;
    const pos = view.start + fx * (view.end - view.start);
    // 近い方のハンドルを掴む
    trimming = Math.abs (pos - st ("sampleStart").getScaledValue())
             <= Math.abs (pos - st ("sampleEnd").getScaledValue()) ? "sampleStart" : "sampleEnd";
    st (trimming).sliderDragStarted();
    setTrim (trimming, pos);
  });
  waveCanvas.addEventListener ("pointermove", (e) => {
    if (panning) {
      const dx = (e.clientX - panX) / waveCanvas.clientWidth; panX = e.clientX;
      const span = view.end - view.start;
      const viewStart = Math.min (Math.max (view.start - dx * span, 0), 1 - span);   // st は外側ヘルパ名
      view = { start: viewStart, end: viewStart + span };
      drawWave();
    } else if (trimming) {
      const fx = e.offsetX / waveCanvas.clientWidth;
      setTrim (trimming, view.start + fx * (view.end - view.start));
    }
  });
  const waveUp = () => {
    panning = false;
    if (trimming) { st (trimming).sliderDragEnded(); trimming = null; }
  };
  waveCanvas.addEventListener ("pointerup", waveUp);
  waveCanvas.addEventListener ("pointercancel", waveUp);

  //---------------------------------------------------------------- D&D / file
  // WebView2 では dataTransfer から実パスが取れないため、バイト列を読んで backend へ渡す。
  // base64 で渡す都合上、転送中はファイルサイズの 5〜6 倍のメモリを使い、
  // エンコードもメッセージスレッドを塞ぐ。大きすぎるファイルは明示的に断る。
  const MAX_TRANSFER_MB = 64;

  async function sendFile (file) {
    try {
      if (! file) { note ("drop: file オブジェクトが取得できません"); return; }
      if (file.size > MAX_TRANSFER_MB * 1024 * 1024) {
        note ("ファイルが大きすぎます（上限 " + MAX_TRANSFER_MB + " MB）: "
              + file.name + " / " + (file.size / 1048576).toFixed (1) + " MB");
        return;
      }
      note ("読み込み中: " + file.name + " (" + Math.round (file.size / 1024) + " KB)");

      const buf = new Uint8Array (await file.arrayBuffer());
      let bin = "";
      const CH = 0x8000;
      for (let i = 0; i < buf.length; i += CH)
        bin += String.fromCharCode.apply (null, buf.subarray (i, i + CH));

      const ok = await nfLoadSample (btoa (bin), file.name);
      if (! ok) { note ("転送失敗: " + file.name); return; }

      // デコードは背景スレッドで進む。失敗は loadError イベントで飛んでくるが、
      // 何も返ってこないまま黙って終わるケースの保険としてタイマも残す。
      // 内蔵デコーダで読めない形式は ffmpeg のプロセス起動が挟まるぶん時間がかかる。
      pendingLoadName = file.name;
      clearTimeout (pendingLoadTimer);
      const nativeExt = /\.(wav|aiff?|flac|mp3|ogg)$/i.test (file.name);
      pendingLoadTimer = setTimeout (() => {
        if (pendingLoadName) note ("デコードできませんでした（対応形式か確認してください）: " + pendingLoadName);
      }, nativeExt ? 5000 : 60000);
    }
    catch (e) { note ("読み込みエラー: " + (e && e.message ? e.message : e)); }
  }

  // 画像ファイルを背景として設定する（D&D / 設定画面の両方から使う）
  async function sendBgFile (file) {
    try {
      note ("背景を設定中: " + file.name);
      const buf = new Uint8Array (await file.arrayBuffer());
      let bin = ""; const CH = 0x8000;
      for (let i = 0; i < buf.length; i += CH) bin += String.fromCharCode.apply (null, buf.subarray (i, i + CH));
      const ok = await nfSetBgImage (btoa (bin));
      note (ok ? "" : "背景画像を読み込めませんでした: " + file.name);
      refreshAppearance();
    }
    catch (e) { note ("背景エラー: " + (e && e.message ? e.message : e)); }
  }

  // dataTransfer から File を取り出す（WebView2 では files が空で items 側にあることがある）
  function fileFromDataTransfer (dt) {
    if (! dt) return null;
    if (dt.files && dt.files.length) return dt.files[0];
    if (dt.items) {
      for (const it of dt.items)
        if (it.kind === "file") { const f = it.getAsFile(); if (f) return f; }
    }
    return null;
  }

  for (const ev of ["dragenter", "dragover"])
    waveWrap.addEventListener (ev, (e) => { e.preventDefault(); waveWrap.classList.add ("drag"); });
  waveWrap.addEventListener ("dragleave", () => waveWrap.classList.remove ("drag"));
  // ドロップは領域外に落とされることもあるので document 側でも受ける
  function handleDrop (e) {
    e.preventDefault();
    waveWrap.classList.remove ("drag");
    const dt = e.dataTransfer;
    const f = fileFromDataTransfer (dt);
    if (f) {
      // 画像は背景画像として扱う（音声として読もうとして失敗するのを防ぐ）
      if (/^image\//.test (f.type) || /\.(png|jpe?g|gif|bmp|webp)$/i.test (f.name)) sendBgFile (f);
      else                                                                          sendFile (f);
      return;
    }
    // 取れなかった場合は原因を表示（WebView2 が実ファイルを渡さないケースの切り分け用）
    const nf = dt && dt.files ? dt.files.length : -1;
    const ni = dt && dt.items ? dt.items.length : -1;
    const types = dt && dt.types ? Array.from (dt.types).join (",") : "?";
    note ("drop: ファイルを取得できません (files=" + nf + " items=" + ni + " types=" + types + ")");
  }
  // drop は document 側だけで受ける。waveWrap にも付けるとバブリングで二重に処理され、
  // 同じファイルが 2 つ読み込まれてしまう。
  document.addEventListener ("dragover", (e) => e.preventDefault());
  document.addEventListener ("drop", handleDrop);

  fileInput.addEventListener ("change", () => { if (fileInput.files[0]) sendFile (fileInput.files[0]); fileInput.value = ""; });

  document.getElementById ("btn-load").addEventListener ("click", () => fileInput.click());
  document.getElementById ("btn-normalize").addEventListener ("click", () => nfNormalize().then (refreshWave));
  document.getElementById ("btn-detect").addEventListener ("click", async () => {
    const btn = document.getElementById ("btn-detect");
    const ok = await nfDetectRoot();
    btn.textContent = ok ? "DETECT ✓" : "?";
    // 元のラベルへ戻す。"DETECT" 決め打ちだと押すたびに "DETECT ROOT" が痩せていく。
    setTimeout (() => { btn.textContent = "DETECT ROOT"; }, 900);
  });

  //---------------------------------------------------------------- ピッチ平坦化
  const flattenAmount = document.getElementById ("flatten-amount");
  const flattenLabel  = document.getElementById ("lbl-flatten");
  const flattenInfoEl = document.getElementById ("flatten-info");
  const btnFlatten    = document.getElementById ("btn-flatten");
  const btnUnflatten  = document.getElementById ("btn-unflatten");
  const btnContour    = document.getElementById ("btn-contour");

  flattenAmount.addEventListener ("input", () => {
    flattenLabel.textContent = Math.round (Number (flattenAmount.value) * 100) + "%";
  });

  // ピッチ曲線の表示トグル。解析は数十ms かかるので押したときだけ走らせる。
  btnContour.addEventListener ("click", async () => {
    if (contour) { contour = null; btnContour.classList.remove ("active"); drawWave(); return; }
    btnContour.textContent = "…";
    const r = await nfPitchContour();
    btnContour.textContent = "PITCH ▲";
    if (! r || ! r.ok) { note ("ピッチを検出できませんでした"); return; }
    contour = r;
    btnContour.classList.add ("active");
    flattenInfoEl.textContent = "検出 " + noteName (r.targetNote) + " (" + r.detected.toFixed (2) + ")";
    drawWave();
  });

  btnFlatten.addEventListener ("click", async () => {
    btnFlatten.disabled = true;
    btnFlatten.textContent = "…";
    // 実処理は背景スレッド。完了は sampleChanged で拾う。
    await nfFlatten (Number (flattenAmount.value));
  });

  btnUnflatten.addEventListener ("click", async () => {
    if (await nfRevertFlatten()) {
      contour = null; btnContour.classList.remove ("active");
      flattenInfoEl.textContent = "";
      refreshWave();
    }
  });

  // 平坦化の完了/状態を UI に反映する（sampleChanged のたびに呼ぶ）
  async function refreshFlatten () {
    btnFlatten.disabled = false;
    btnFlatten.textContent = "FLATTEN";
    const st2 = await nfFlattenState();
    if (! st2) return;
    btnUnflatten.disabled = ! st2.canRevert;
    flattenInfoEl.textContent = st2.targetNote >= 0
      ? noteName (st2.targetNote) + " に平坦化 (結果 " + st2.result.toFixed (2)
        + " / 元 " + st2.detected.toFixed (2) + " / " + st2.frames + "フレーム)"
      : "";
  }

  //---------------------------------------------------------------- ADSR
  window.drawAdsr = drawAdsr;
  function adsrGeom () {
    const w = adsrCanvas.width, h = adsrCanvas.height;
    const g = (id) => S[id] ? S[id].getScaledValue() : 0;
    const sc = (ms) => Math.sqrt (Math.max (0, ms));
    const wa = sc (g ("attack")), wd = sc (g ("decay")), wr = sc (g ("release"));
    const sum = Math.max (0.001, wa + wd + wr), sFrac = .22;
    const avail = w * (1 - sFrac);
    const xa = avail * wa / sum, xd = avail * wd / sum, xr = avail * wr / sum, xs = w * sFrac;
    const yTop = 4, yBot = h - 4;
    const sy = yBot - (yBot - yTop) * Math.min (1, Math.max (0, g ("sustain")));
    return { w, h, xa, xd, xs, xr, yTop, yBot, sy };
  }
  function drawAdsr () {
    if (! adsrCanvas || ! S.attack) return;
    const ctx = adsrCanvas.getContext ("2d");
    const G = adsrGeom();
    ctx.clearRect (0, 0, G.w, G.h);
    const ax = G.xa, dx = ax + G.xd, sx = dx + G.xs, rx = sx + G.xr;

    // 背景グリッド
    ctx.strokeStyle = "rgba(255,255,255,.045)"; ctx.lineWidth = 1;
    for (let i = 1; i < 4; ++i) {
      const y = Math.round (G.h * i / 4) + .5;
      ctx.beginPath(); ctx.moveTo (0, y); ctx.lineTo (G.w, y); ctx.stroke();
    }
    // 区間の区切り
    ctx.strokeStyle = "rgba(255,255,255,.12)"; ctx.setLineDash ([2, 3]);
    for (const x of [ax, dx, sx]) { ctx.beginPath(); ctx.moveTo (x + .5, 0); ctx.lineTo (x + .5, G.h); ctx.stroke(); }
    ctx.setLineDash ([]);

    // 塗り
    const grad = ctx.createLinearGradient (0, G.yTop, 0, G.yBot);
    grad.addColorStop (0, rgba (ACCENT, .35));
    grad.addColorStop (1, rgba (ACCENT, .04));
    ctx.beginPath();
    ctx.moveTo (0, G.yBot); ctx.lineTo (ax, G.yTop); ctx.lineTo (dx, G.sy);
    ctx.lineTo (sx, G.sy);  ctx.lineTo (rx, G.yBot); ctx.closePath();
    ctx.fillStyle = grad; ctx.fill();

    // 線
    ctx.beginPath();
    ctx.moveTo (0, G.yBot); ctx.lineTo (ax, G.yTop); ctx.lineTo (dx, G.sy);
    ctx.lineTo (sx, G.sy);  ctx.lineTo (rx, G.yBot);
    ctx.strokeStyle = ACCENT_HI; ctx.lineWidth = 2;
    ctx.lineJoin = "round"; ctx.stroke();

    // ハンドル
    for (const p of [[ax, G.yTop], [dx, G.sy], [rx, G.yBot]]) {
      ctx.beginPath(); ctx.arc (p[0], p[1], 4.5, 0, Math.PI * 2);
      ctx.fillStyle = "#0e1114"; ctx.fill();
      ctx.lineWidth = 2; ctx.strokeStyle = ACCENT_HI; ctx.stroke();
    }
  }

  let grab = -1, lastPos = null;
  adsrCanvas.addEventListener ("pointerdown", (e) => {
    const G = adsrGeom();
    const ax = G.xa, dx = ax + G.xd, rx = dx + G.xs + G.xr;
    const sx = e.offsetX * (adsrCanvas.width / adsrCanvas.clientWidth);
    const sy = e.offsetY * (adsrCanvas.height / adsrCanvas.clientHeight);
    const d = (px, py) => Math.hypot (px - sx, py - sy);
    const dA = d (ax, G.yTop), dD = d (dx, G.sy), dR = d (rx, G.yBot);
    grab = (dA <= dD && dA <= dR) ? 0 : (dD <= dR ? 1 : 2);
    lastPos = { x: sx, y: sy };
    adsrCanvas.setPointerCapture (e.pointerId);
    for (const id of ["attack","decay","sustain","release"]) S[id].sliderDragStarted();
  });
  adsrCanvas.addEventListener ("pointermove", (e) => {
    if (grab < 0) return;
    const sx = e.offsetX * (adsrCanvas.width / adsrCanvas.clientWidth);
    const sy = e.offsetY * (adsrCanvas.height / adsrCanvas.clientHeight);
    const dx = sx - lastPos.x, dy = sy - lastPos.y;
    lastPos = { x: sx, y: sy };
    // スケール値(ms) → 正規化値。JS 側に公開された変換が無いので properties から自前で計算する。
    // 引数名を st にしない（外側の st(id) ヘルパを隠さないため）
    const toNorm = (state, scaled) => {
      const p = state.properties;
      const t = (scaled - p.start) / (p.end - p.start);
      return Math.min (1, Math.max (0, Math.pow (Math.min (1, Math.max (0, t)), p.skew)));
    };
    const adjMs = (id, dpx) => {
      const state = st (id);
      const k  = Math.sqrt (Math.max (1, state.properties.end)) / Math.max (1, adsrCanvas.width);
      const sq = Math.sqrt (Math.max (0, state.getScaledValue())) + dpx * k;
      state.setNormalisedValue (toNorm (state, sq * sq));
    };
    if (grab === 0) adjMs ("attack", dx);
    else if (grab === 1) {
      adjMs ("decay", dx);
      const s = S.sustain;
      s.setNormalisedValue (Math.min (1, Math.max (0, s.getNormalisedValue() - dy / adsrCanvas.height)));
    } else adjMs ("release", dx);
    drawAdsr();
  });
  const adsrUp = () => {
    if (grab < 0) return;
    grab = -1;
    for (const id of ["attack","decay","sustain","release"]) S[id].sliderDragEnded();
  };
  adsrCanvas.addEventListener ("pointerup", adsrUp);
  adsrCanvas.addEventListener ("pointercancel", adsrUp);

  //---------------------------------------------------------------- ビブラート表示
  // 発音からの時間に対する変調量（DELAY 待機 → FADE で立ち上げ → DEPTH）を描く。
  window.drawVib = drawVib;
  function drawVib () {
    if (! vibCanvas || ! S.vibDepth) return;
    const ctx = vibCanvas.getContext ("2d");
    const w = vibCanvas.width, h = vibCanvas.height, mid = h / 2;
    ctx.clearRect (0, 0, w, h);

    const depth = st ("vibDepth").getScaledValue();
    const rate  = st ("vibRate").getScaledValue();
    const delay = st ("vibDelay").getScaledValue() / 1000;
    const fade  = st ("vibFade").getScaledValue()  / 1000;
    const span  = Math.max (1.0, delay + fade + 3 / Math.max (0.1, rate));   // 表示する秒数

    // グリッド（0.5秒ごと）＋センターライン
    ctx.strokeStyle = "rgba(255,255,255,.05)"; ctx.lineWidth = 1;
    for (let t = 0.5; t < span; t += 0.5) {
      const x = Math.round (t / span * w) + .5;
      ctx.beginPath(); ctx.moveTo (x, 0); ctx.lineTo (x, h); ctx.stroke();
    }
    ctx.strokeStyle = "rgba(255,255,255,.14)";
    ctx.beginPath(); ctx.moveTo (0, Math.round (mid) + .5); ctx.lineTo (w, Math.round (mid) + .5); ctx.stroke();

    if (depth <= 0.01) {
      ctx.fillStyle = "#6b7681"; ctx.font = "10px 'Segoe UI', sans-serif"; ctx.textAlign = "center";
      ctx.fillText ("DEPTH を上げるとビブラートが有効になります", w / 2, mid - 8);
      return;
    }

    const amp = h * .40;                       // 200ct を画面いっぱいとして正規化
    const scale = Math.min (1, depth / 200);
    const envAt = (t) => {
      const u = t - delay;
      if (u <= 0) return 0;
      return fade > 0 ? Math.min (1, u / fade) : 1;
    };

    // 包絡線（上下）
    ctx.strokeStyle = rgba (ACCENT, .45); ctx.setLineDash ([3, 3]); ctx.lineWidth = 1;
    for (const sign of [-1, 1]) {
      ctx.beginPath();
      for (let x = 0; x <= w; ++x) {
        const y = mid + sign * envAt (x / w * span) * amp * scale;
        x === 0 ? ctx.moveTo (x, y) : ctx.lineTo (x, y);
      }
      ctx.stroke();
    }
    ctx.setLineDash ([]);

    // 実際の揺れ
    ctx.strokeStyle = ACCENT_HI; ctx.lineWidth = 2; ctx.lineJoin = "round";
    ctx.beginPath();
    for (let x = 0; x <= w; ++x) {
      const t = x / w * span;
      const y = mid + envAt (t) * amp * scale * Math.sin (2 * Math.PI * rate * t);
      x === 0 ? ctx.moveTo (x, y) : ctx.lineTo (x, y);
    }
    ctx.stroke();

    // DELAY / FADE の境界
    ctx.strokeStyle = "#ff9f4a"; ctx.setLineDash ([2, 3]);
    for (const t of [delay, delay + fade]) {
      if (t <= 0 || t >= span) continue;
      const x = Math.round (t / span * w) + .5;
      ctx.beginPath(); ctx.moveTo (x, 0); ctx.lineTo (x, h); ctx.stroke();
    }
    ctx.setLineDash ([]);

    ctx.fillStyle = "#6b7681"; ctx.font = "9px 'Segoe UI', sans-serif"; ctx.textAlign = "left";
    ctx.fillText (span.toFixed (2) + " s", 4, h - 4);
  }

  //---------------------------------------------------------------- keyboard
  const LOW = 36, HIGH = 96;   // C2..C7
  const isBlack = (n) => [1,3,6,8,10].includes (((n % 12) + 12) % 12);

  function buildKeys () {
    keysEl.innerHTML = ""; keyEls.clear();
    const whites = [];
    for (let n = LOW; n <= HIGH; ++n) if (! isBlack (n)) whites.push (n);
    const wpc = 100 / whites.length;

    whites.forEach ((n, i) => {
      const d = document.createElement ("div");
      d.className = "key white"; d.style.left = (i * wpc) + "%"; d.style.width = wpc + "%";
      d.dataset.note = n; keysEl.appendChild (d); keyEls.set (n, d);
    });
    for (let n = LOW; n <= HIGH; ++n) {
      if (! isBlack (n)) continue;
      const below = whites.filter (w => w < n).length;   // 直前の白鍵数
      const d = document.createElement ("div");
      d.className = "key black";
      d.style.left = (below * wpc - wpc * .3) + "%"; d.style.width = (wpc * .6) + "%";
      d.dataset.note = n; keysEl.appendChild (d); keyEls.set (n, d);
    }
    paintKeys();
  }

  window.paintKeys = paintKeys;
  function paintKeys () {
    if (! keyEls || keyEls.size === 0) return;
    const root = S.rootKey ? Math.round (S.rootKey.getScaledValue()) : -1;
    for (const [n, el] of keyEls) el.classList.toggle ("root", n === root);
  }

  // バックエンドからの鍵盤状態（"keys" イベント、20Hz）。文字コードの意味は WebEditor.cpp 参照。
  const keyFlashTimers = new Map();   // note -> timeout id（短いノート / miss の消灯用）
  const flashClass = (n, cls, ms) => {
    const el = keyEls.get (n); if (! el) return;
    el.classList.add (cls);
    const key = n + cls;
    clearTimeout (keyFlashTimers.get (key));
    keyFlashTimers.set (key, setTimeout (() => { el.classList.remove (cls); keyFlashTimers.delete (key); }, ms));
  };
  let fbCount = 0;   // 表示範囲内でフォールバックになる鍵の数（凡例の出し分け用）
  window.__JUCE__.backend.addEventListener ("keys", (k) => {
    if (! k || typeof k.state !== "string") return;
    fbCount = 0;
    for (const [n, el] of keyEls) {
      const st = k.state.charCodeAt (n) - 48;
      el.classList.toggle ("fb",   st === 2);
      el.classList.toggle ("pend", st === 1);
      if (st === 2) ++fbCount;
      const h = k.held ? k.held.charCodeAt (n) - 48 : 0;
      if (h === 1) {   // 押されている間は点灯し続ける（進行中の消灯タイマーは捨てる）
        el.classList.add ("midi");
        clearTimeout (keyFlashTimers.get (n + "midi")); keyFlashTimers.delete (n + "midi");
      }
      else if (h === 2) flashClass (n, "midi", 120);                       // 短いノート: 1 フレームは見せる
      else if (! keyFlashTimers.has (n + "midi")) el.classList.remove ("midi");
      if (k.miss && k.miss.charCodeAt (n) === 49) flashClass (n, "miss", 700);
    }
    const legend = document.getElementById ("key-legend");
    if (legend) legend.hidden = fbCount === 0;
  });

  let heldNote = -1;
  const noteOn  = (n) => { if (heldNote === n) return; noteOff(); heldNote = n; nfNote (n, true);
                           const el = keyEls.get (n); if (el) el.classList.add ("down"); };
  const noteOff = () => { if (heldNote < 0) return; nfNote (heldNote, false);
                          const el = keyEls.get (heldNote); if (el) el.classList.remove ("down"); heldNote = -1; };

  keysEl.addEventListener ("contextmenu", (e) => e.preventDefault());
  keysEl.addEventListener ("pointerdown", (e) => {
    const t = e.target.closest (".key"); if (! t) return;
    const n = parseInt (t.dataset.note, 10);
    if (e.button !== 0) {   // 右クリックで Root 指定（音は出さない）
      S.rootKey.sliderDragStarted();
      S.rootKey.setNormalisedValue (n / 127);
      S.rootKey.sliderDragEnded();
      paintKeys();
      return;
    }
    keysEl.setPointerCapture (e.pointerId);
    noteOn (n);
  });
  keysEl.addEventListener ("pointermove", (e) => {
    if (heldNote < 0) return;
    const t = document.elementFromPoint (e.clientX, e.clientY);
    const k = t && t.closest ? t.closest (".key") : null;
    if (k) noteOn (parseInt (k.dataset.note, 10));
  });
  keysEl.addEventListener ("pointerup", noteOff);
  keysEl.addEventListener ("pointercancel", noteOff);
  buildKeys();

  //---------------------------------------------------------------- REAPER modes
  async function refreshReaper () {
    const r = await nfReaperModes();
    if (! r) return;
    const fill = (sel, arr, cur) => {
      sel.innerHTML = "";
      (arr || []).forEach ((label, i) => {
        const o = document.createElement ("option"); o.value = i; o.textContent = label || ("#" + i);
        sel.appendChild (o);
      });
      sel.value = String (cur);
      sel.disabled = ! r.available || ! (arr && arr.length);
    };
    fill (selRMode, r.modes, r.mode);
    fill (selRSub,  r.subs,  r.sub);
    reaperAvailable = !! r.available;
    updateEnablement();
  }
  selRMode.addEventListener ("change", async () => {
    await nfSetReaperMode (parseInt (selRMode.value, 10), 0);
    refreshReaper();
  });
  selRSub.addEventListener ("change", () => nfSetReaperMode (parseInt (selRMode.value, 10), parseInt (selRSub.value, 10)));
  refreshReaper();

  //---------------------------------------------------------------- backend events
  window.__JUCE__.backend.addEventListener ("sampleChanged", () => {
    pendingLoadName = null; clearTimeout (pendingLoadTimer);
    note ("");
    contour = null;                                  // 音が変わったので曲線は作り直し
    document.getElementById ("btn-contour").classList.remove ("active");
    refreshWave(); refreshReaper(); refreshSampleList(); refreshFlatten();
  });
  // デコード失敗（背景スレッドなので loadSample の戻り値では返せない）。
  // ffmpeg の stderr がそのまま入ってくるので、原因が UI で読める。
  window.__JUCE__.backend.addEventListener ("loadError", (e) => {
    pendingLoadName = null; clearTimeout (pendingLoadTimer);
    note (e && e.message ? e.message : "読み込みに失敗しました");
    refreshFlatten();   // 平坦化が失敗した場合もボタンを戻す
  });
  window.__JUCE__.backend.addEventListener ("status", (s) => {
    const bar = document.getElementById ("cache-bar");
    const fill = document.getElementById ("cache-fill");
    const text = document.getElementById ("cache-text");
    bar.hidden = ! s.cacheBusy;
    if (s.cacheBusy) {
      fill.style.width = Math.round (s.cacheProgress * 100) + "%";
      // 件数は出す。「0% で止まる」と言われたときに、生成済みが 0 なのか
      // 分母がおかしいのかを利用者の画面だけで切り分けられる。
      const done = s.cacheReady | 0, left = s.cachePending | 0;
      text.textContent = "キャッシュ生成中 " + Math.round (s.cacheProgress * 100) + "%"
                       + "  (" + done + "/" + (done + left) + ")";
      // 開発用の内部状態（世代 / 変化理由 / 稼働ジョブ数 / 失敗数）。
      // 配布ビルドには出さない。切り分けが要るときだけ次の1行を有効にする。
      // text.textContent += s.cacheDebug ? "  [" + s.cacheDebug + "]" : "";
    }
    // 空文字＝REAPER でもなく élastique も未設定。何をすれば使えるかまで出す。
    document.getElementById ("reaper-info").textContent =
      s.reaper || "REAPER 非対応ホスト — 設定（⚙）で elastique を指定すると使えます";
    if (!! s.elastique !== elastiqueLoaded) { elastiqueLoaded = !! s.elastique; updateEnablement(); }
    const up = document.getElementById ("update-btn");
    up.hidden = ! s.updateAvail;
    if (s.updateAvail) up.textContent = "⬆ v" + s.latest;
    statusEl.textContent = (s.fallback ? "⚠ 代替エンジンで再生中 · " : "") + "v" + s.version;
  });
  document.getElementById ("update-btn").addEventListener ("click", () => nfOpenReleases());

  //---------------------------------------------------------------- 外観設定
  const dlg     = document.getElementById ("settings");
  const inColour= document.getElementById ("set-colour");
  const inOpac  = document.getElementById ("set-opacity");
  const opLbl   = document.getElementById ("set-oplbl");
  const inPanel = document.getElementById ("set-panel");
  const paLbl   = document.getElementById ("set-palbl");
  const bgInput = document.getElementById ("bg-input");
  const bgLayer = document.getElementById ("bg-layer");

  // プリセット色
  const SWATCHES = ["#4bb4f5","#7ee0c0","#a678f0","#ff7b9c","#ffb04a","#8cd94a","#ff5e5e","#c8d0d8"];
  const swWrap = document.getElementById ("set-swatches");
  for (const c of SWATCHES) {
    const d = document.createElement ("div");
    d.className = "swatch"; d.style.background = c;
    d.addEventListener ("click", () => { inColour.value = c; pushAppearance(); });
    swWrap.appendChild (d);
  }

  function applyBg (dataUrl, opacity) {
    bgLayer.style.setProperty ("--bg-img", dataUrl ? "url('" + dataUrl + "')" : "none");
    bgLayer.style.setProperty ("--bg-op", dataUrl ? opacity : 0);
  }

  // パネルを半透明にして背景画像を透かす。窪み（波形など）は読みやすさのため少し濃いめに。
  function applyPanelAlpha (a) {
    const root = document.documentElement.style;
    root.setProperty ("--panel-a", a);
    root.setProperty ("--inset-a", Math.min (1, a + 0.15));
  }

  let bgDataUrl = null;
  async function refreshAppearance () {
    const a = await nfGetAppearance();
    if (! a) return;
    inColour.value = a.colour || "#4bb4f5";
    inOpac.value   = a.opacity != null ? a.opacity : .25;
    inPanel.value  = a.panel   != null ? a.panel   : 1;
    opLbl.textContent = Number (inOpac.value).toFixed (2);
    paLbl.textContent = Number (inPanel.value).toFixed (2);
    bgDataUrl = a.bg || null;
    applyAccent (inColour.value);
    applyBg (bgDataUrl, Number (inOpac.value));
    applyPanelAlpha (Number (inPanel.value));
  }

  function pushAppearance () {
    opLbl.textContent = Number (inOpac.value).toFixed (2);
    paLbl.textContent = Number (inPanel.value).toFixed (2);
    applyAccent (inColour.value);
    applyBg (bgDataUrl, Number (inOpac.value));
    applyPanelAlpha (Number (inPanel.value));
    nfSetAppearance (inColour.value, Number (inOpac.value), Number (inPanel.value));
  }
  inColour.addEventListener ("input", pushAppearance);
  inOpac.addEventListener ("input", pushAppearance);
  inPanel.addEventListener ("input", pushAppearance);

  document.getElementById ("btn-settings").addEventListener ("click", () => { dlg.hidden = false; });
  document.getElementById ("set-close").addEventListener ("click", () => { dlg.hidden = true; });
  dlg.addEventListener ("pointerdown", (e) => { if (e.target === dlg) dlg.hidden = true; });

  document.getElementById ("set-pick").addEventListener ("click", () => bgInput.click());
  bgInput.addEventListener ("change", () => {
    const f = bgInput.files[0]; bgInput.value = "";
    if (f) sendBgFile (f);
  });
  document.getElementById ("set-clear").addEventListener ("click", async () => {
    await nfSetBgImage ("");
    refreshAppearance();
  });
  document.getElementById ("set-default").addEventListener ("click", async () => {
    const b = document.getElementById ("set-default");
    await nfSaveAppearanceDefault();
    b.textContent = "保存しました ✓";
    setTimeout (() => { b.textContent = "全インスタンスの既定にする"; }, 1200);
  });

  // ---- ffmpeg（外部デコーダ）----
  const ffState = document.getElementById ("ff-state");
  const ffPath  = document.getElementById ("ff-path");
  function showFfmpeg (s) {
    if (!s) return;
    ffState.textContent  = s.available ? "有効" : "未設定";
    ffState.style.color  = s.available ? "var(--accent)" : "";
    ffPath.textContent   = s.available ? s.path
      : (s.found ? "自動検出できます: " + s.found : "ffmpeg が見つかりません（PATH を確認してください）");
  }
  document.getElementById ("ff-auto") .addEventListener ("click", async () => showFfmpeg (await nfSetFfmpeg ("")));
  document.getElementById ("ff-clear").addEventListener ("click", async () => showFfmpeg (await nfSetFfmpeg ("-")));
  document.getElementById ("ff-pick") .addEventListener ("click", async () => showFfmpeg (await nfBrowseFfmpeg()));
  nfGetFfmpeg().then (showFfmpeg);

  // ---- élastique 直叩き（実験機能）----
  const elaState = document.getElementById ("ela-state");
  const elaPath  = document.getElementById ("ela-path");
  function showElastique (s) {
    if (!s) return;
    if (s.reaperHosted) {
      elaState.textContent = "REAPER 上（不要）";
      elaPath.textContent  = "REAPER 上では本来の API を使うので、この設定は無視されます。";
      return;
    }
    elaState.textContent = s.loaded ? "有効" : "無効";
    elaState.style.color = s.loaded ? "var(--accent)" : "";
    elaPath.textContent  = s.loaded ? s.path
      : (s.found && s.found.length ? "候補: " + s.found[0] : "elastique3.dll が見つかりません");
  }
  async function refreshElastique() { showElastique (await nfGetElastique()); }
  document.getElementById ("ela-auto") .addEventListener ("click", async () => showElastique (await nfSetElastique ("")));
  document.getElementById ("ela-clear").addEventListener ("click", async () => showElastique (await nfSetElastique ("-")));
  document.getElementById ("ela-pick") .addEventListener ("click", async () => showElastique (await nfBrowseElastique()));
  refreshElastique();

  // 他インスタンスからのブロードキャストや state 復元にも追従
  window.__JUCE__.backend.addEventListener ("appearanceChanged", refreshAppearance);
  refreshAppearance();

  //---------------------------------------------------------------- ホバーヘルプ
  // ネイティブ版エディタは JUCE の TooltipWindow で出していたが、Web UI 版には
  // 移植されていなかった。文言は C++ 側（core/ParamHelp.h）から取るので二重管理しない。
  // ブラウザ既定の title ツールチップは遅延も見た目も制御できないので自前で出す。
  const tipEl = document.createElement ("div");
  tipEl.className = "hovertip";
  tipEl.hidden = true;
  document.body.appendChild (tipEl);

  let tipTimer = 0;
  function hideTip () { clearTimeout (tipTimer); tipEl.hidden = true; }
  function showTipFor (el, text) {
    clearTimeout (tipTimer);
    tipTimer = setTimeout (() => {
      tipEl.textContent = text;
      tipEl.hidden = false;
      // いったん表示してから実寸で位置を決める（画面外へはみ出さないように寄せる）
      const r = el.getBoundingClientRect();
      const t = tipEl.getBoundingClientRect();
      let x = r.left + r.width / 2 - t.width / 2;
      let y = r.bottom + 8;
      x = Math.max (4, Math.min (x, window.innerWidth  - t.width  - 4));
      if (y + t.height > window.innerHeight - 4) y = r.top - t.height - 8;   // 下が狭ければ上へ
      tipEl.style.left = Math.round (x) + "px";
      tipEl.style.top  = Math.round (y) + "px";
    }, 450);
  }

  function attachHelp (el, text) {
    if (! el || ! text) return;
    el.addEventListener ("mouseenter", () => showTipFor (el, text));
    el.addEventListener ("mouseleave", hideTip);
    el.addEventListener ("pointerdown", hideTip);   // 操作を始めたら邪魔なので消す
  }

  nfParamHelp().then ((help) => {
    if (! help) return;
    // ノブ: ラベルと値も含めたセル全体を当たり判定にする（ノブ本体だけだと狭い）
    for (const knob of document.querySelectorAll (".knob[data-relay]")) {
      const text = help[knob.dataset.relay];
      attachHelp (knob.closest (".knob-cell") || knob, text);
    }
    // コンボ / チェックボックス: ラベルごと当たり判定にする
    for (const id of Object.keys (help)) {
      const sel = document.getElementById ("sel-" + id);
      if (sel) attachHelp (sel.closest ("label") || sel, help[id]);
      const chk = document.getElementById ("chk-" + id);
      if (chk) attachHelp (chk.closest ("label") || chk, help[id]);
    }
    // DOM の id がパラメータ名と違うものは個別に対応付ける
    for (const [domId, paramId] of [["sel-rmode", "reaperMode"], ["sel-rsub", "reaperSubMode"]]) {
      const el = document.getElementById (domId);
      if (el) attachHelp (el.closest ("label") || el, help[paramId]);
    }
  }).catch (() => {});

  // タブを切り替えたら出しっぱなしにしない
  window.addEventListener ("blur", hideTip);

  // ---- Space = ホストの再生/停止 ----
  // WebView2 はネイティブ子ウィンドウなので、フォーカスがあると Space が DAW まで届かない。
  // さらに直前に押したボタンにフォーカスが残っていると Space でそれが再クリックされてしまう。
  // capture フェーズで握りつぶし、ネイティブ側でホスト窓へキーを投げ直す
  // （アクションを決め打ちしないので、ホスト側の Space の割り当てがそのまま効く）。
  function isTypingTarget (el) {
    if (!el) return false;
    if (el.isContentEditable) return true;
    const tag = el.tagName;
    if (tag === "TEXTAREA" || tag === "SELECT") return true;
    // range/color/file は Space を使わないのでトランスポート優先。text 系だけ除外する。
    return tag === "INPUT" && !["range", "color", "file", "checkbox", "radio"].includes (el.type);
  }
  function onSpace (e) {
    if (e.code !== "Space" && e.key !== " ") return;
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    if (isTypingTarget (e.target)) return;
    // keydown/keyup の両方を止める（button の click は keyup で合成されるため）
    e.preventDefault();
    e.stopPropagation();
    if (e.type === "keydown" && !e.repeat) nfSpaceToHost();
  }
  window.addEventListener ("keydown", onSpace, true);
  window.addEventListener ("keyup",   onSpace, true);

  // ---- タブ ----
  const tabs  = Array.from (document.querySelectorAll (".tab"));
  const pages = Array.from (document.querySelectorAll ("[data-page]"));
  function showTab (name) {
    for (const t of tabs)  t.classList.toggle ("active", t.dataset.tab === name);
    for (const p of pages) p.hidden = (p.dataset.page !== name);
    layoutCanvases();   // 非表示中は幅0なので、表示後にキャンバスを測り直す
  }
  for (const t of tabs) t.addEventListener ("click", () => showTab (t.dataset.tab));
  showTab ("main");

  refreshWave();
  refreshSampleList();
  refreshFlatten();
  layoutCanvases();
  statusEl.textContent = "ready";
  nfReady();   // イベント購読が揃ったので status / keys を送り直してもらう
}).catch (e => showError (String (e && e.message ? e.message : e)));

//================================================================ canvas sizing
function layoutCanvases () {
  for (const c of document.querySelectorAll ("canvas")) {
    const r = c.getBoundingClientRect();
    if (r.width > 0 && r.height > 0) { c.width = Math.round (r.width); c.height = Math.round (r.height); }
  }
  if (window.drawWave) window.drawWave();
  if (window.drawAdsr) window.drawAdsr();
  if (window.drawVib)  window.drawVib();
}
window.addEventListener ("resize", layoutCanvases);
layoutCanvases();
