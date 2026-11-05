/*
 * app.js — the instrument's UI, playback and export.
 *
 * Signal flow while listening:
 *   rendered buffer ─┬─ dry ─────────────┐
 *                    └─ convolver ─ wet ─┴─ normalize ─ gain ─ out
 * Everything after the buffer is live (no re-render needed). Reverse flips
 * the rendered buffer itself, so the reverb tail still rings after it.
 */
(function () {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const dbToLin = (db) => Math.pow(10, db / 20);

  const MAX_OUT_SAMPLES = 64 * 1024 * 1024;   // ≈ 11.6 min stereo @ 48 kHz
  const NORM_TARGET = dbToLin(-0.5);
  const IR_SEED = 90210;
  const STORE_KEY = 'slowsippin.settings.v1';

  // slider position is 0..1000; `scale` maps it to the value
  const CONTROLS = [
    { id: 'stretch', label: 'STRETCH', unit: '×',  min: 1,   max: 1000, nmax: 1e6, def: 20,  dec: 2, scale: 'log',  engine: true },
    { id: 'pitch',   label: 'PITCH',   unit: 'st', min: -48, max: 48,   nmin: -60, nmax: 60, def: 0, dec: 1, engine: true },
    { id: 'weave',   label: 'WEAVE',   unit: '',   min: 1,   max: 16,   def: 4,   dec: 0, int: true, engine: true },
    { id: 'scatter', label: 'SCATTER', unit: '%',  min: 0,   max: 100,  def: 100, dec: 0, engine: true },
    { id: 'drift',   label: 'DRIFT',   unit: 'ms', min: 0,   max: 1000, nmax: 10000, def: 0, dec: 0, scale: 'sqrt', engine: true },
    { id: 'reverb',  label: 'REVERB',  unit: '%',  min: 0,   max: 100,  def: 0,   dec: 0 },
    { id: 'size',    label: 'SIZE',    unit: 's',  min: 0.2, max: 20,   nmin: 0.1, nmax: 30, def: 2.5, dec: 1, scale: 'log' },
    { id: 'gain',    label: 'GAIN',    unit: 'dB', min: -24, max: 24,   nmin: -60, def: 0, dec: 1 },
  ];
  const CTL = {};
  CONTROLS.forEach((c) => { CTL[c.id] = c; });

  const S = {
    ctx: null,
    post: null,
    workerOK: null,
    src: null,          // { id, name, buffer }
    region: null,       // { a, b } samples, b exclusive
    out: null,          // { buffer, reversed, key, peak, sr, name, stretch, pitch }
    params: {},
    toggles: { normalize: true, monoStereo: true, reverse: false },
    render: null,
    play: null,         // { node, t0, offset }
    audition: null,     // { node, t0, a, b }
    startPoint: 0,      // seconds into the rendered buffer
    wetPeak: null,
    measureToken: 0,
    exporting: false,
    srcSeq: 0,
  };

  // ------------------------------------------------------------ helpers --

  function fmtNum(v, dec) {
    return dec === 0 ? String(Math.round(v)) : v.toFixed(dec);
  }
  function dispDec(c, v) {
    if (c.id === 'stretch') return v >= 10000 ? 0 : v >= 1000 ? 1 : 2;
    return c.dec;
  }
  function parseNum(s) {
    s = String(s).trim().replace(/\s+/g, '').replace(',', '.').replace(/[^0-9.+\-eE]/g, '');
    return s ? Number(s) : NaN;
  }
  function fmtDur(sec) {
    if (!isFinite(sec)) return '—';
    if (sec < 10) return sec.toFixed(2) + 's';
    if (sec < 60) return sec.toFixed(1) + 's';
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = Math.floor(sec % 60);
    if (h) return h + 'h ' + String(m).padStart(2, '0') + 'm';
    return m + 'm ' + String(s).padStart(2, '0') + 's';
  }
  function fmtClock(sec) {
    sec = Math.max(0, sec);
    const m = Math.floor(sec / 60), s = sec - m * 60;
    return m + ':' + (s < 10 ? '0' : '') + s.toFixed(1);
  }
  function fmtMB(bytes) {
    return (bytes / 1048576).toFixed(1) + ' MB';
  }
  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }
  function peakOf(arrays) {
    let p = 0;
    for (const d of arrays) {
      for (let i = 0; i < d.length; i++) {
        const v = d[i] < 0 ? -d[i] : d[i];
        if (v > p) p = v;
      }
    }
    return p;
  }
  function channelsOf(buf) {
    const out = [];
    for (let c = 0; c < buf.numberOfChannels; c++) out.push(buf.getChannelData(c));
    return out;
  }

  let statusTimer = 0;
  function setStatus(kind, text, ms) {
    const el = $('#status');
    el.className = 'status ' + (kind || '');
    $('#statusText').textContent = text;
    clearTimeout(statusTimer);
    if (ms) statusTimer = setTimeout(() => setStatus(S.out ? 'ready' : '', S.out ? 'ready' : (S.src ? 'loaded' : 'idle')), ms);
  }

  // ----------------------------------------------------------- settings --

  function loadSettings() {
    try {
      const raw = localStorage.getItem(STORE_KEY);
      if (!raw) return null;
      return JSON.parse(raw);
    } catch (e) { return null; }
  }
  let saveTimer = 0;
  function saveSettings() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      try {
        localStorage.setItem(STORE_KEY, JSON.stringify({
          params: S.params, toggles: S.toggles,
          fmt: $('#fmtSel').value, bits: $('#bitsSel').value,
        }));
      } catch (e) { /* storage unavailable: fine */ }
    }, 300);
  }

  // -------------------------------------------------------------- audio --

  function ensureCtx() {
    if (!S.ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      S.ctx = new AC();
      buildPost();
    }
    if (S.ctx.state === 'suspended') S.ctx.resume();
    return S.ctx;
  }

  function buildPost() {
    const ctx = S.ctx;
    const p = {
      input: ctx.createGain(),
      dry: ctx.createGain(),
      wet: ctx.createGain(),
      norm: ctx.createGain(),
      gain: ctx.createGain(),
      conv: null,
      convSize: null,
    };
    p.input.connect(p.dry);
    p.dry.connect(p.norm);
    p.wet.connect(p.norm);
    p.norm.connect(p.gain);
    p.gain.connect(ctx.destination);
    S.post = p;
    rebuildIR();
    applyPost(true);
  }

  const irCache = new Map();
  function irChans(sr, size) {
    const key = sr + ':' + size;
    let chans = irCache.get(key);
    if (!chans) {
      chans = Stretch.makeIR(sr, size, IR_SEED);
      if (irCache.size >= 2) irCache.delete(irCache.keys().next().value);
      irCache.set(key, chans);
    }
    return chans;
  }
  function irBuffer(ctx, sr, size) {
    const chans = irChans(sr, size);
    const b = ctx.createBuffer(2, chans[0].length, sr);
    b.copyToChannel(chans[0], 0);
    b.copyToChannel(chans[1], 1);
    return b;
  }

  function rebuildIR() {
    if (!S.ctx) return;
    const p = S.post, ctx = S.ctx, size = S.params.size;
    if (p.convSize === size) return;
    const conv = ctx.createConvolver();
    conv.normalize = false;
    conv.buffer = irBuffer(ctx, ctx.sampleRate, size);
    const old = p.conv;
    const now = ctx.currentTime;
    if (old) {
      // brief dip so the swap doesn't click
      p.wet.gain.cancelScheduledValues(now);
      p.wet.gain.setTargetAtTime(0, now, 0.01);
      setTimeout(() => {
        try { p.input.disconnect(old); old.disconnect(); } catch (e) { /* already gone */ }
        p.input.connect(conv);
        conv.connect(p.wet);
        applyPost();
      }, 45);
    } else {
      p.input.connect(conv);
      conv.connect(p.wet);
    }
    p.conv = conv;
    p.convSize = size;
  }

  let irTimer = 0;
  function scheduleIR() {
    clearTimeout(irTimer);
    irTimer = setTimeout(rebuildIR, 120);
  }

  function wetDry() {
    const a = S.params.reverb / 100;
    return { dry: Math.cos(a * Math.PI / 2), wet: Math.sin(a * Math.PI / 2) };
  }

  function currentPeak() {
    if (!S.out) return 1;
    if (S.params.reverb > 0 && S.wetPeak) return S.wetPeak;
    return S.out.peak;
  }

  function normGain() {
    return S.toggles.normalize ? NORM_TARGET / Math.max(1e-9, currentPeak()) : 1;
  }

  function applyPost(immediate) {
    if (!S.post) return;
    const p = S.post, t = S.ctx.currentTime, tc = immediate ? 0.001 : 0.03;
    const wd = wetDry();
    p.dry.gain.setTargetAtTime(wd.dry, t, tc);
    p.wet.gain.setTargetAtTime(wd.wet, t, tc);
    p.norm.gain.setTargetAtTime(normGain(), t, tc);
    p.gain.gain.setTargetAtTime(dbToLin(S.params.gain), t, tc);
  }

  /** Render the reverb mix offline (used for export and true-peak normalize). */
  async function renderMix() {
    const buf = S.out.buffer, sr = buf.sampleRate, size = S.params.size;
    const ch = Math.max(2, buf.numberOfChannels);
    const irLen = irChans(sr, size)[0].length;
    const oc = new OfflineAudioContext(ch, buf.length + irLen, sr);
    const ir = irBuffer(oc, sr, size);
    const src = oc.createBufferSource();
    src.buffer = buf;
    const conv = oc.createConvolver();
    conv.normalize = false;
    conv.buffer = ir;
    const dry = oc.createGain(), wet = oc.createGain();
    const wd = wetDry();
    dry.gain.value = wd.dry;
    wet.gain.value = wd.wet;
    src.connect(dry).connect(oc.destination);
    src.connect(conv).connect(wet).connect(oc.destination);
    src.start(0);
    return oc.startRendering();
  }

  let measureTimer = 0;
  function scheduleMeasure() {
    clearTimeout(measureTimer);
    const token = ++S.measureToken;
    if (!S.out || !S.toggles.normalize || S.params.reverb <= 0) { applyPost(); return; }
    measureTimer = setTimeout(async () => {
      try {
        const mix = await renderMix();
        if (token !== S.measureToken) return;
        S.wetPeak = peakOf(channelsOf(mix));
        applyPost();
      } catch (e) { /* keep the estimate */ }
    }, 450);
  }

  // ----------------------------------------------------------- playback --

  function play(from) {
    if (!S.out) return;
    const ctx = ensureCtx();
    stopAudition(true);
    stopPlay(true);
    const buf = S.out.buffer;
    const off = clamp(from != null ? from : S.startPoint, 0, Math.max(0, buf.duration - 0.005));
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.connect(S.post.input);
    node.start(0, off);
    const p = { node, t0: ctx.currentTime, offset: off };
    node.onended = () => {
      if (S.play === p) { S.play = null; uiTransport(); outView.overlay(); }
    };
    S.play = p;
    uiTransport();
    startRaf();
  }

  function stopPlay(silent) {
    if (S.play) {
      const n = S.play.node;
      S.play = null;
      n.onended = null;
      try { n.stop(); } catch (e) { /* not started */ }
      setTimeout(() => n.disconnect(), 50);
    }
    if (!silent) { uiTransport(); outView.overlay(); }
  }

  function playPos() {
    if (!S.play) return null;
    return Math.min(S.out.buffer.duration, S.play.offset + (S.ctx.currentTime - S.play.t0));
  }

  function toggleAudition() {
    if (S.audition) { stopAudition(); return; }
    if (!S.src) { openPicker(); return; }
    const ctx = ensureCtx();
    stopPlay(true);
    const buf = S.src.buffer, sr = buf.sampleRate;
    const r = regionOrFull();
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.connect(ctx.destination);
    node.start(0, r.a / sr, (r.b - r.a) / sr);
    const a = { node, t0: ctx.currentTime, a: r.a, b: r.b };
    node.onended = () => {
      if (S.audition === a) { S.audition = null; uiTransport(); srcView.overlay(); }
    };
    S.audition = a;
    uiTransport();
    startRaf();
  }

  function stopAudition(silent) {
    if (S.audition) {
      const n = S.audition.node;
      S.audition = null;
      n.onended = null;
      try { n.stop(); } catch (e) { /* not started */ }
      setTimeout(() => n.disconnect(), 50);
    }
    if (!silent) { uiTransport(); srcView.overlay(); }
  }

  function auditionPos() {
    if (!S.audition) return null;
    const sr = S.src.buffer.sampleRate;
    return Math.min(S.audition.b, S.audition.a + (S.ctx.currentTime - S.audition.t0) * sr);
  }

  function uiTransport() {
    $('#auditionBtn').classList.toggle('on', !!S.audition);
    $('#auditionBtn').textContent = S.audition ? 'STOP AUDITION' : 'AUDITION';
    $('#playBtn').classList.toggle('on', !!S.play);
    $('#playBtn').disabled = !S.out;
    $('#stopBtn').disabled = !S.out;
    $('#downloadBtn').disabled = !S.out || S.exporting;
    $('#auditionBtn').disabled = !S.src;
    updateTimeTag();
  }

  function updateTimeTag() {
    const tag = $('#timeTag');
    if (!S.out) { tag.textContent = ''; return; }
    const pos = S.play ? playPos() : S.startPoint;
    tag.textContent = fmtClock(pos) + ' / ' + fmtClock(S.out.buffer.duration);
  }

  let rafOn = false;
  function startRaf() {
    if (rafOn) return;
    rafOn = true;
    requestAnimationFrame(tick);
  }
  function tick() {
    srcView.overlay();
    outView.overlay();
    updateTimeTag();
    if (S.play || S.audition) requestAnimationFrame(tick);
    else rafOn = false;
  }

  // ----------------------------------------------------------- waveform --

  function computePeaks(buf, cols) {
    const n = buf.length, chans = channelsOf(buf);
    const mins = new Float32Array(cols), maxs = new Float32Array(cols);
    let peak = 0;
    for (let x = 0; x < cols; x++) {
      const s0 = Math.floor(x * n / cols);
      const s1 = Math.min(n, Math.max(s0 + 1, Math.floor((x + 1) * n / cols)));
      let lo = Infinity, hi = -Infinity;
      for (const d of chans) {
        for (let i = s0; i < s1; i++) {
          const v = d[i];
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
      }
      if (lo === Infinity) { lo = 0; hi = 0; }
      mins[x] = lo; maxs[x] = hi;
      peak = Math.max(peak, -lo, hi);
    }
    return { mins, maxs, cols, peak };
  }

  class WaveView {
    constructor(root, drawBase, drawOver) {
      this.root = root;
      this.base = root.querySelector('canvas.base');
      this.over = root.querySelector('canvas.over');
      this.drawBase = drawBase;
      this.drawOver = drawOver;
      this.peaks = null;
      this.peaksFor = null;
      this.dpr = 1;
      if (window.ResizeObserver) new ResizeObserver(() => this.resize()).observe(root);
      window.addEventListener('resize', () => this.resize());
      this.resize();
    }
    resize() {
      const dpr = Math.min(2, window.devicePixelRatio || 1);
      const w = Math.max(1, Math.round(this.root.clientWidth * dpr));
      const h = Math.max(1, Math.round(this.root.clientHeight * dpr));
      if (w === this.base.width && h === this.base.height && dpr === this.dpr) return;
      this.dpr = dpr;
      for (const c of [this.base, this.over]) { c.width = w; c.height = h; }
      this.peaks = null;
      this.redraw();
    }
    getPeaks(buf) {
      if (!buf) return null;
      if (!this.peaks || this.peaksFor !== buf || this.peaks.cols !== this.base.width) {
        this.peaks = computePeaks(buf, this.base.width);
        this.peaksFor = buf;
      }
      return this.peaks;
    }
    redraw() { this.drawBase(this); this.drawOver(this); }
    overlay() { this.drawOver(this); }
    xToFrac(clientX) {
      const r = this.root.getBoundingClientRect();
      return clamp((clientX - r.left - this.root.clientLeft) / this.root.clientWidth, 0, 1);
    }
  }

  let COLORS = null;
  function colors() {
    if (!COLORS) {
      COLORS = {
        red: cssVar('--red'), grey: cssVar('--grey-wave'), ink: cssVar('--ink'),
        faint: cssVar('--faint'), muted: cssVar('--muted'), region: cssVar('--region-bg'),
      };
    }
    return COLORS;
  }

  function drawTicks(g, W, H, dpr) {
    g.fillStyle = colors().faint;
    const n = 48;
    for (let i = 1; i < n; i++) {
      const x = Math.round(i * W / n);
      const len = (i % 4 === 0 ? 8 : 4) * dpr;
      g.fillRect(x, 0, Math.max(1, dpr), len);
      g.fillRect(x, H - len, Math.max(1, dpr), len);
    }
  }

  function drawColumns(g, pk, x0, x1, mid, sc, color) {
    g.fillStyle = color;
    for (let x = Math.max(0, x0); x < Math.min(pk.cols, x1); x++) {
      const y0 = mid - pk.maxs[x] * sc;
      const y1 = mid - pk.mins[x] * sc;
      g.fillRect(x, y0, 1, Math.max(1, y1 - y0));
    }
  }

  function drawSourceBase(v) {
    const g = v.base.getContext('2d'), W = v.base.width, H = v.base.height, C = colors();
    g.clearRect(0, 0, W, H);
    drawTicks(g, W, H, v.dpr);
    if (!S.src) return;
    const buf = S.src.buffer, n = buf.length;
    const pk = v.getPeaks(buf);
    const mid = H / 2, sc = (H * 0.40) / Math.max(1e-6, pk.peak);
    const r = S.region;
    if (!r) {
      drawColumns(g, pk, 0, W, mid, sc, C.grey);
      return;
    }
    const xa = r.a / n * W, xb = r.b / n * W;
    g.fillStyle = C.region;
    g.fillRect(xa, 0, xb - xa, H);
    drawColumns(g, pk, 0, W, mid, sc, C.grey);
    drawColumns(g, pk, Math.floor(xa), Math.ceil(xb), mid, sc, C.red);
    g.fillStyle = C.red;
    const lw = Math.max(1, Math.round(1.5 * v.dpr));
    g.fillRect(Math.round(xa) - (lw >> 1), 0, lw, H);
    if (r.b < n) g.fillRect(Math.round(xb) - (lw >> 1), 0, lw, H);
  }

  function drawSourceOver(v) {
    const g = v.over.getContext('2d'), W = v.over.width, H = v.over.height;
    g.clearRect(0, 0, W, H);
    if (!S.src || !S.audition) return;
    const x = auditionPos() / S.src.buffer.length * W;
    g.fillStyle = colors().ink;
    g.fillRect(Math.round(x), 0, Math.max(1, Math.round(1.5 * v.dpr)), H);
  }

  function drawOutBase(v) {
    const g = v.base.getContext('2d'), W = v.base.width, H = v.base.height, C = colors();
    g.clearRect(0, 0, W, H);
    drawTicks(g, W, H, v.dpr);
    if (!S.out) return;
    const pk = v.getPeaks(S.out.buffer);
    const mid = H / 2, sc = (H * 0.40) / Math.max(1e-6, pk.peak);
    drawColumns(g, pk, 0, W, mid, sc, C.ink);
  }

  function drawOutOver(v) {
    const g = v.over.getContext('2d'), W = v.over.width, H = v.over.height, C = colors();
    g.clearRect(0, 0, W, H);
    if (!S.out) return;
    const dur = S.out.buffer.duration;
    const lw = Math.max(1, Math.round(1.5 * v.dpr));
    const xs = Math.round(S.startPoint / dur * W);
    g.fillStyle = C.red;
    g.globalAlpha = S.play ? 0.45 : 1;
    g.fillRect(xs - (lw >> 1), 0, lw, H);
    g.globalAlpha = 1;
    if (S.play) {
      const x = Math.round(playPos() / dur * W);
      g.fillRect(x - (lw >> 1), 0, lw + 1, H);
    }
  }

  const srcView = new WaveView($('#srcWave'), drawSourceBase, drawSourceOver);
  const outView = new WaveView($('#outWave'), drawOutBase, drawOutOver);

  // ------------------------------------------------------------- region --

  function regionOrFull() {
    if (!S.src) return { a: 0, b: 0 };
    return S.region || { a: 0, b: S.src.buffer.length };
  }

  function setRegion(r) {
    if (r && S.src) {
      const n = S.src.buffer.length, sr = S.src.buffer.sampleRate;
      let a = clamp(Math.round(r.a), 0, n), b = clamp(Math.round(r.b), 0, n);
      if (b < a) { const t = a; a = b; b = t; }
      if (b - a < sr * 0.01 || (a === 0 && b === n)) r = null;
      else r = { a, b };
    } else r = null;
    S.region = r;
    srcView.redraw();
    updateRegionTag();
    updateStats();
    updateStale();
  }

  function updateRegionTag() {
    const tag = $('#regionTag');
    if (!S.src) { tag.textContent = ''; return; }
    const sr = S.src.buffer.sampleRate, n = S.src.buffer.length;
    const r = S.region;
    if (!r) tag.textContent = 'FULL ' + (n / sr).toFixed(2) + 'S';
    else if (r.b >= n) tag.textContent = 'FROM ' + (r.a / sr).toFixed(2) + 'S · ' + ((r.b - r.a) / sr).toFixed(2) + 'S';
    else tag.textContent = 'REGION ' + ((r.b - r.a) / sr).toFixed(2) + 'S';
  }

  function bindSourceWave() {
    const el = srcView.root;
    let drag = null;
    let lastTap = { t: 0, x: 0 };
    el.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      if (!S.src) { openPicker(); return; }
      el.setPointerCapture(e.pointerId);
      drag = { x0: e.clientX, moved: false };
    });
    el.addEventListener('pointermove', (e) => {
      if (!drag) return;
      if (!drag.moved && Math.abs(e.clientX - drag.x0) < 4) return;
      drag.moved = true;
      const n = S.src.buffer.length;
      const f0 = srcView.xToFrac(drag.x0), f1 = srcView.xToFrac(e.clientX);
      S.region = { a: Math.round(Math.min(f0, f1) * n), b: Math.round(Math.max(f0, f1) * n) };
      srcView.redraw();
      updateRegionTag();
      updateStats();
    });
    const end = (e) => {
      if (!drag) return;
      const d = drag;
      drag = null;
      if (d.moved) { setRegion(S.region); return; }
      if (e.type === 'pointercancel') return;
      const now = performance.now();
      if (now - lastTap.t < 340 && Math.abs(e.clientX - lastTap.x) < 12) {
        lastTap.t = 0;
        setRegion(null);                     // double click: clear
      } else {
        lastTap = { t: now, x: e.clientX };
        const n = S.src.buffer.length;
        setRegion({ a: srcView.xToFrac(e.clientX) * n, b: n });   // click: start point
      }
    };
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
  }

  function bindOutWave() {
    outView.root.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || !S.out || S.render) return;
      const t = outView.xToFrac(e.clientX) * S.out.buffer.duration;
      S.startPoint = t;
      if (S.play) play(t);
      else { outView.overlay(); updateTimeTag(); }
    });
  }

  // ----------------------------------------------------------- controls --

  function toPos(c, v) {
    const lo = c.min, hi = c.max;
    v = clamp(v, lo, hi);
    let t;
    if (c.scale === 'log') t = Math.log(v / lo) / Math.log(hi / lo);
    else if (c.scale === 'sqrt') t = Math.sqrt((v - lo) / (hi - lo));
    else t = (v - lo) / (hi - lo);
    return Math.round(t * 1000);
  }
  function fromPos(c, pos) {
    const t = pos / 1000, lo = c.min, hi = c.max;
    if (c.scale === 'log') return lo * Math.pow(hi / lo, t);
    if (c.scale === 'sqrt') return lo + (hi - lo) * t * t;
    return lo + (hi - lo) * t;
  }
  function quantize(c, v) {
    if (c.int) return Math.round(v);
    const q = Math.pow(10, c.dec);
    return Math.round(v * q) / q;
  }

  function buildControls() {
    const box = $('#controls');
    for (const c of CONTROLS) {
      const row = document.createElement('div');
      row.className = 'ctl' + (c.engine ? ' engine' : '');
      row.innerHTML =
        '<label class="ctl-label" for="r-' + c.id + '">' + c.label + '</label>' +
        '<div class="ctl-value"><span class="num"></span><span class="unit">' + c.unit + '</span></div>' +
        '<input type="range" id="r-' + c.id + '" min="0" max="1000" step="1">' +
        '<input type="text" class="ctl-box" inputmode="decimal" autocomplete="off" spellcheck="false" aria-label="' + c.label + ' value">';
      box.appendChild(row);
      const el = c.el = {
        row,
        num: row.querySelector('.num'),
        range: row.querySelector('input[type=range]'),
        box: row.querySelector('.ctl-box'),
      };
      el.range.addEventListener('input', () => setParam(c, fromPos(c, +el.range.value), 'range'));
      el.range.addEventListener('dblclick', () => setParam(c, c.def, 'reset'));
      el.box.addEventListener('change', () => setParam(c, parseNum(el.box.value), 'box'));
      el.box.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { setParam(c, parseNum(el.box.value), 'box'); el.box.select(); }
        else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
          e.preventDefault();
          const dir = e.key === 'ArrowUp' ? 1 : -1;
          const cur = S.params[c.id];
          let step = c.int ? 1 : Math.pow(10, -c.dec) * (e.shiftKey ? 10 : 1);
          if (c.scale === 'log') step = Math.max(step, cur * (e.shiftKey ? 0.1 : 0.01));
          setParam(c, cur + dir * step, 'key');
          el.box.select();
        }
      });
      el.box.addEventListener('focus', () => el.box.select());
    }
  }

  function setParam(c, v, from) {
    if (!isFinite(v)) { syncControl(c); return; }
    const lo = c.nmin != null ? c.nmin : c.min, hi = c.nmax != null ? c.nmax : c.max;
    v = quantize(c, clamp(v, lo, hi));
    const changed = S.params[c.id] !== v;
    S.params[c.id] = v;
    syncControl(c, from);
    if (changed && from !== 'init') {
      onParamChanged(c.id);
      saveSettings();
    }
  }

  function syncControl(c, from) {
    const v = S.params[c.id], el = c.el;
    const d = dispDec(c, v);
    el.num.textContent = fmtNum(v, d);
    if (from !== 'range') el.range.value = toPos(c, v);
    el.range.style.setProperty('--p', (el.range.value / 10) + '%');
    el.box.value = fmtNum(v, d);
  }

  function onParamChanged(id) {
    switch (id) {
      case 'reverb': applyPost(); scheduleMeasure(); break;
      case 'size': scheduleIR(); scheduleMeasure(); break;
      case 'gain': applyPost(); break;
      default: updateStale();
    }
    updateStats();
  }

  function bindToggles() {
    const map = { normalize: '#tNormalize', monoStereo: '#tMonoStereo', reverse: '#tReverse' };
    for (const k of Object.keys(map)) {
      const el = $(map[k]);
      el.checked = !!S.toggles[k];
      el.addEventListener('change', () => {
        S.toggles[k] = el.checked;
        saveSettings();
        if (k === 'normalize') { applyPost(); scheduleMeasure(); }
        else if (k === 'reverse') flipOutput();
        else if (k === 'monoStereo') { updateStale(); updateStats(); }
      });
    }
  }

  // -------------------------------------------------------- stale state --

  function engineKey() {
    if (!S.src) return '';
    const p = S.params, r = regionOrFull();
    const ms = S.src.buffer.numberOfChannels === 1 && S.toggles.monoStereo;
    return [S.src.id, r.a, r.b, p.stretch, p.pitch, p.weave, p.scatter, p.drift, ms].join('|');
  }

  function updateStale() {
    const stale = !!(S.out && S.out.key !== engineKey());
    $('#renderBtn').classList.toggle('stale', stale);
    const hint = $('#outHint');
    if (S.render) return;
    if (stale) {
      hint.textContent = 'settings changed — press RENDER to hear them';
      hint.classList.add('warn');
    } else {
      hint.textContent = S.out ? 'click rendered wave to set start point · space = play/stop' : 'set the controls, then RENDER';
      hint.classList.remove('warn');
    }
    if (S.out && S.out.snap) {
      for (const c of CONTROLS) {
        if (c.engine) c.el.row.classList.toggle('changed', S.out.snap[c.id] !== S.params[c.id]);
      }
    }
  }

  // ------------------------------------------------------------ stats --

  function outputEstimate() {
    if (!S.src) return null;
    const buf = S.src.buffer, r = regionOrFull();
    const len = r.b - r.a;
    const outLen = Math.max(1, Math.round(len * S.params.stretch));
    const ch = buf.numberOfChannels === 1 && S.toggles.monoStereo ? 2 : buf.numberOfChannels;
    return { len, outLen, ch, sr: buf.sampleRate, samples: outLen * ch };
  }

  function exportShape() {
    if (!S.out) return null;
    const b = S.out.buffer, sr = b.sampleRate;
    const rev = S.params.reverb > 0;
    const tail = rev ? Math.round(0.012 * sr) + Math.max(1, Math.round(Math.max(0.05, S.params.size) * sr)) : 0;
    return { frames: b.length + tail, ch: rev ? Math.max(2, b.numberOfChannels) : b.numberOfChannels, sr };
  }

  function updateStats() {
    const est = outputEstimate();
    const so = $('#statSource'), oo = $('#statOutput');
    if (!est) {
      so.textContent = '—';
      oo.textContent = '—';
      oo.classList.remove('over');
    } else {
      so.textContent = fmtDur(est.len / est.sr) + ' · ' + est.len.toLocaleString() + ' smp';
      oo.textContent = fmtDur(est.outLen / est.sr) + ' · ' + est.ch + 'ch';
      const over = est.samples > MAX_OUT_SAMPLES;
      oo.classList.toggle('over', over);
      oo.title = over ? 'Too long to render — select a shorter region or lower the stretch.' : '';
    }
    let bytes = 0;
    if (S.src) bytes += S.src.buffer.length * S.src.buffer.numberOfChannels * 4;
    if (S.out) bytes += S.out.buffer.length * S.out.buffer.numberOfChannels * 4;
    $('#statMemory').textContent = fmtMB(bytes);

    const sh = exportShape();
    if (!sh) $('#statExport').textContent = '—';
    else {
      const bits = +$('#bitsSel').value;
      const size = sh.frames * sh.ch * bits / 8 + 64;
      $('#statExport').textContent = fmtDur(sh.frames / sh.sr) + ' · ' + sh.ch + 'ch · ' + fmtMB(size);
    }
  }

  // ------------------------------------------------------------ loading --

  async function openPicker() {
    if (window.native) {                       // macOS app: native Open dialog
      const smp = await window.native.openSample();
      if (smp) loadFile(new File([smp.data], smp.name));
      return;
    }
    $('#fileInput').click();
  }

  async function loadFile(file) {
    if (!file) return;
    const ctx = ensureCtx();
    setStatus('busy', 'loading…');
    let buffer;
    try {
      const ab = await file.arrayBuffer();
      buffer = await Codec.decode(ab, ctx);
    } catch (e) {
      setStatus('error', 'could not read ' + file.name, 5000);
      return;
    }
    if (!buffer || !buffer.length) {
      setStatus('error', 'empty file: ' + file.name, 5000);
      return;
    }
    if (buffer.numberOfChannels > 2) {
      const b2 = ctx.createBuffer(2, buffer.length, buffer.sampleRate);
      b2.copyToChannel(buffer.getChannelData(0), 0);
      b2.copyToChannel(buffer.getChannelData(1), 1);
      buffer = b2;
    }
    cancelRender();
    stopPlay(true);
    stopAudition(true);
    S.src = { id: ++S.srcSeq, name: file.name, buffer };
    S.region = null;
    S.out = null;
    S.wetPeak = null;
    S.startPoint = 0;

    const ch = buffer.numberOfChannels;
    $('#fileInfo').innerHTML = '';
    const nm = document.createElement('span');
    nm.textContent = file.name;
    const meta = document.createElement('span');
    meta.className = 'meta';
    meta.textContent = ' · ' + buffer.sampleRate + ' Hz · ' + ch + 'ch · ' + buffer.duration.toFixed(2) + 's';
    $('#fileInfo').append(nm, meta);
    $('#fileInfo').title = file.name;
    $('#srcEmpty').classList.add('hidden');
    $('#tMonoStereo').disabled = ch !== 1;

    srcView.redraw();
    outView.redraw();
    updateRegionTag();
    updateStats();
    updateStale();
    uiTransport();
    setStatus('', 'loaded');
  }

  function bindLoading() {
    $('#loadBtn').addEventListener('click', openPicker);
    $('#fileInput').addEventListener('change', (e) => {
      const f = e.target.files && e.target.files[0];
      e.target.value = '';
      loadFile(f);
    });
    let depth = 0;
    const hasFiles = (e) => e.dataTransfer && Array.prototype.indexOf.call(e.dataTransfer.types || [], 'Files') >= 0;
    window.addEventListener('dragenter', (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      depth++;
      document.body.classList.add('dragging');
    });
    window.addEventListener('dragover', (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = 'copy';
    });
    window.addEventListener('dragleave', () => {
      depth = Math.max(0, depth - 1);
      if (!depth) document.body.classList.remove('dragging');
    });
    window.addEventListener('drop', (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      depth = 0;
      document.body.classList.remove('dragging');
      const f = e.dataTransfer.files && e.dataTransfer.files[0];
      loadFile(f);
    });
  }

  // ---------------------------------------------------------- rendering --

  function probeWorker() {
    return new Promise((resolve) => {
      let w;
      try { w = new Worker('js/worker.js'); } catch (e) { resolve(false); return; }
      const t = setTimeout(() => { w.terminate(); resolve(false); }, 2500);
      w.onmessage = (e) => {
        if (e.data && e.data.type === 'pong') { clearTimeout(t); w.terminate(); resolve(true); }
      };
      w.onerror = () => { clearTimeout(t); w.terminate(); resolve(false); };
      w.postMessage({ type: 'ping' });
    });
  }

  function runJob(job, onProgress) {
    let cancel;
    const promise = new Promise((resolve, reject) => {
      if (S.workerOK) {
        const w = new Worker('js/worker.js');
        cancel = () => { w.terminate(); reject(new Error('cancelled')); };
        w.onmessage = (e) => {
          const m = e.data;
          if (m.type === 'progress') onProgress(m.p);
          else if (m.type === 'done') { w.terminate(); resolve(m.out); }
          else if (m.type === 'error') { w.terminate(); reject(new Error(m.message)); }
        };
        w.onerror = (e) => { w.terminate(); reject(new Error(e.message || 'worker failed')); };
        w.postMessage({ type: 'run', input: job.input, opts: job.opts }, [job.input.buffer]);
      } else {
        // main-thread fallback (e.g. opened from file://): small slices
        let stop = false;
        cancel = () => { stop = true; reject(new Error('cancelled')); };
        let sj;
        try { sj = new Stretch.StretchJob(job.input, job.opts); } catch (e) { reject(e); return; }
        const pump = () => {
          if (stop) return;
          const t = performance.now();
          try {
            while (!sj.done && performance.now() - t < 20) sj.step(1);
          } catch (e) { reject(e); return; }
          onProgress(sj.progress);
          if (sj.done) resolve(sj.out);
          else setTimeout(pump, 0);
        };
        setTimeout(pump, 0);
      }
    });
    return { promise, cancel: () => cancel && cancel() };
  }

  async function startRender() {
    if (S.render) { cancelRender(); return; }
    if (!S.src) { openPicker(); return; }
    ensureCtx();
    const est = outputEstimate();
    if (est.samples > MAX_OUT_SAMPLES) {
      const hint = $('#outHint');
      hint.textContent = 'output too long (max ≈ ' + fmtDur(MAX_OUT_SAMPLES / est.ch / est.sr) + ' at this rate) — select a shorter region or lower the stretch';
      hint.classList.add('warn');
      setStatus('error', 'output too long', 4000);
      return;
    }
    if (S.workerOK == null) S.workerOK = await probeWorker();

    const buf = S.src.buffer, r = regionOrFull(), p = S.params;
    const srcCh = buf.numberOfChannels;
    const seed = (Math.random() * 4294967296) >>> 0;
    const snap = { stretch: p.stretch, pitch: p.pitch, weave: p.weave, scatter: p.scatter, drift: p.drift };
    const key = engineKey();
    const jobs = [];
    for (let c = 0; c < est.ch; c++) {
      const input = buf.getChannelData(srcCh === 1 ? 0 : c).slice(r.a, r.b);
      // mono → stereo: two independent phase passes; stereo: shared seed keeps the image
      const jseed = srcCh === 1 && est.ch === 2 ? (seed + c * 0x9E3779B9) >>> 0 : seed;
      jobs.push({
        input,
        opts: {
          sr: buf.sampleRate, stretch: p.stretch, pitch: p.pitch, weave: p.weave,
          scatter: p.scatter / 100, driftMs: p.drift, seed: jseed,
        },
      });
    }

    const token = { handles: [] };
    S.render = token;
    const prog = new Float64Array(jobs.length);
    const t0 = performance.now();
    renderUI(true, 0, '');
    token.handles = jobs.map((job, i) => runJob(job, (v) => {
      prog[i] = v;
      if (S.render !== token) return;
      let sum = 0;
      for (let k = 0; k < prog.length; k++) sum += prog[k];
      const f = sum / prog.length;
      const el = (performance.now() - t0) / 1000;
      const eta = f > 0.03 ? el / f - el : null;
      renderUI(true, f, eta != null ? fmtDur(eta) + ' left' : '');
    }));

    let outs;
    try {
      outs = await Promise.all(token.handles.map((h) => h.promise));
    } catch (e) {
      if (S.render === token) {
        S.render = null;
        renderUI(false);
        token.handles.forEach((h) => h.cancel());
        setStatus('error', 'render failed: ' + e.message, 6000);
      }
      return;
    }
    if (S.render !== token) return;
    S.render = null;

    const ctx = S.ctx;
    const len = outs[0].length;
    const ob = ctx.createBuffer(outs.length, len, buf.sampleRate);
    outs.forEach((d, c) => {
      if (S.toggles.reverse) d.reverse();
      ob.copyToChannel(d, c);
    });
    stopPlay(true);
    S.out = {
      buffer: ob,
      reversed: S.toggles.reverse,
      key,
      snap,
      peak: peakOf(outs),
      name: S.src.name,
    };
    outs = null;
    S.startPoint = 0;
    S.wetPeak = null;
    outView.peaks = null;
    outView.redraw();
    renderUI(false);
    applyPost();
    scheduleMeasure();
    updateStale();
    updateStats();
    uiTransport();
    const secs = ((performance.now() - t0) / 1000).toFixed(1);
    setStatus('ready', 'rendered in ' + secs + 's', 4000);
  }

  function cancelRender() {
    const t = S.render;
    if (!t) return;
    S.render = null;
    t.handles.forEach((h) => h.cancel());
    renderUI(false);
    setStatus('', 'cancelled', 2500);
  }

  function renderUI(on, frac, extra) {
    const btn = $('#renderBtn');
    btn.textContent = on ? 'CANCEL' : 'RENDER';
    $('#progress').classList.toggle('on', on);
    if (on) {
      $('#progressBar').style.width = (frac * 100).toFixed(1) + '%';
      $('#progressText').textContent = 'RENDERING ' + Math.round(frac * 100) + '%' + (extra ? ' · ' + extra : '');
      $('#outHint').textContent = 'rendering in the background — CANCEL to stop';
      $('#outHint').classList.remove('warn');
      setStatus('busy', 'rendering ' + Math.round(frac * 100) + '%');
    } else {
      updateStale();
    }
  }

  // ------------------------------------------------------------ reverse --

  function flipOutput() {
    if (!S.out || S.out.reversed === S.toggles.reverse) return;
    const old = S.out.buffer, dur = old.duration;
    const wasPlaying = S.play ? playPos() : null;
    stopPlay(true);
    const nb = S.ctx.createBuffer(old.numberOfChannels, old.length, old.sampleRate);
    for (let c = 0; c < old.numberOfChannels; c++) {
      const d = nb.getChannelData(c);
      d.set(old.getChannelData(c));
      d.reverse();
    }
    S.out.buffer = nb;
    S.out.reversed = S.toggles.reverse;
    S.startPoint = clamp(dur - S.startPoint, 0, dur);
    S.wetPeak = null;
    outView.peaks = null;
    outView.redraw();
    applyPost();
    scheduleMeasure();
    if (wasPlaying != null) play(clamp(dur - wasPlaying, 0, dur));
    else uiTransport();
  }

  // ------------------------------------------------------------- export --

  function exportName(ext) {
    const base = (S.out.name || 'sample').replace(/\.[^.]+$/, '').replace(/[^\w\-]+/g, '_');
    const s = S.out.snap;
    let n = base + '_x' + String(s.stretch).replace('.', ',');
    if (s.pitch) n += '_' + (s.pitch > 0 ? '+' : '') + s.pitch + 'st';
    if (S.toggles.reverse) n += '_rev';
    return n + '.' + ext;
  }

  async function doExport() {
    if (!S.out || S.exporting) return;
    S.exporting = true;
    const btn = $('#downloadBtn');
    btn.textContent = 'EXPORTING…';
    uiTransport();
    setStatus('busy', 'exporting…');
    await new Promise((r) => setTimeout(r, 30));
    try {
      const fmt = $('#fmtSel').value, bits = +$('#bitsSel').value;
      let buf = S.out.buffer;
      if (S.params.reverb > 0) buf = await renderMix();
      const chans = channelsOf(buf);
      const peak = peakOf(chans);
      if (S.params.reverb > 0) { S.wetPeak = peak; applyPost(); }
      const norm = S.toggles.normalize ? NORM_TARGET / Math.max(1e-9, peak) : 1;
      const g = norm * dbToLin(S.params.gain);
      const blob = fmt === 'aiff'
        ? Codec.encodeAIFF(chans, buf.sampleRate, bits, g)
        : Codec.encodeWAV(chans, buf.sampleRate, bits, g);
      const name = exportName(fmt === 'aiff' ? 'aif' : 'wav');
      let savedAs = name;
      if (window.native) {                     // macOS app: native Save dialog
        const bytes = new Uint8Array(await blob.arrayBuffer());
        const saved = await window.native.saveFile(name, bytes);
        if (!saved) { setStatus('', 'export cancelled', 2500); return; }
        savedAs = saved.split('/').pop();
      } else {
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = name;
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(url), 60000);
      }
      const outDb = 20 * Math.log10(Math.max(1e-9, peak * g));
      if (outDb > 0.01 && bits !== 32) setStatus('error', 'exported — clipped by ' + outDb.toFixed(1) + ' dB (lower GAIN)', 7000);
      else setStatus('ready', 'saved ' + savedAs + ' · ' + fmtMB(blob.size), 5000);
    } catch (e) {
      setStatus('error', 'export failed: ' + e.message, 7000);
    } finally {
      S.exporting = false;
      btn.textContent = 'DOWNLOAD';
      uiTransport();
    }
  }

  // --------------------------------------------------------------- init --

  function init() {
    buildControls();
    const saved = loadSettings();
    for (const c of CONTROLS) {
      const v = saved && saved.params && isFinite(saved.params[c.id]) ? saved.params[c.id] : c.def;
      setParam(c, v, 'init');
    }
    if (saved && saved.toggles) {
      for (const k of Object.keys(S.toggles)) {
        if (typeof saved.toggles[k] === 'boolean') S.toggles[k] = saved.toggles[k];
      }
    }
    if (saved && saved.fmt) $('#fmtSel').value = saved.fmt;
    if (saved && saved.bits) $('#bitsSel').value = saved.bits;

    bindToggles();
    bindLoading();
    bindSourceWave();
    bindOutWave();

    $('#auditionBtn').addEventListener('click', toggleAudition);
    $('#renderBtn').addEventListener('click', startRender);
    $('#playBtn').addEventListener('click', () => { if (S.play) stopPlay(); else play(); });
    $('#stopBtn').addEventListener('click', () => { stopPlay(); stopAudition(); });
    $('#downloadBtn').addEventListener('click', doExport);
    $('#fmtSel').addEventListener('change', () => { saveSettings(); updateStats(); });
    $('#bitsSel').addEventListener('change', () => { saveSettings(); updateStats(); });

    document.addEventListener('keydown', (e) => {
      if (e.code !== 'Space' || e.repeat) return;
      const t = e.target;
      if (t && (t.tagName === 'INPUT' && t.type === 'text' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) return;
      if (t && t.tagName === 'BUTTON') return;
      e.preventDefault();
      if (S.out) { if (S.play) stopPlay(); else play(); }
      else if (S.src) toggleAudition();
    });

    $('#tMonoStereo').disabled = true;
    updateRegionTag();
    updateStats();
    updateStale();
    uiTransport();
    setStatus('', 'idle');
    probeWorker().then((ok) => { if (S.workerOK == null) S.workerOK = ok; });

    if (window.native) {
      document.documentElement.classList.add('native');
      window.native.onOpenSample((smp) => loadFile(new File([smp.data], smp.name)));
      window.native.onMenu((cmd) => {
        if (cmd === 'render') startRender();
        else if (cmd === 'play') { if (S.play) stopPlay(); else play(); }
        else if (cmd === 'audition') toggleAudition();
        else if (cmd === 'export') doExport();
        else if (cmd === 'clear-region') setRegion(null);
      });
    }

    // test/automation hook
    window.SlowSippin = { state: S, loadFile, startRender, setParam: (id, v) => setParam(CTL[id], v, 'api'), setRegion };
  }

  init();
})();
