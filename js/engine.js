/*
 * engine.js — the stretch engine.
 *
 * A spectral (STFT) time-stretcher in the family of the phase vocoder and
 * PaulStretch. One output channel is rendered by one StretchJob, which can run
 * inside a Web Worker (see worker.js) or on the main thread in small steps.
 *
 *   STRETCH  output grains advance by `hop`; the read position advances by
 *            hop / stretch, so time is scaled without touching pitch.
 *   PITCH    each spectral peak (with the bins around it) is moved to its
 *            transposed position — independent of stretch.
 *   WEAVE    number of overlapping grains: hop = N / weave.
 *   SCATTER  0 = phase-vocoder phase (propagated + locked to the real
 *            analysis phase around every peak: coherent, attacks keep their
 *            shape), 1 = fully invented random phase (PaulStretch texture).
 *            Values in between add a random phase offset of ±scatter·π.
 *   DRIFT    the read position of every grain wanders: a slow and a medium
 *            smooth random curve plus a per-grain jitter.
 *
 * No dependencies. Exposes `Stretch` on the global object (window / worker)
 * and as module.exports under Node (used by the tests).
 */
(function (root) {
  'use strict';

  const TWO_PI = Math.PI * 2;

  // ---------------------------------------------------------------- random --

  function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
      a = (a + 0x6D2B79F5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  // Stateless 32-bit hash of two integers — lets every grain have its own
  // random stream, so channels sharing a seed stay in lock-step.
  function hash32(a, b) {
    let h = (a ^ Math.imul((b + 0x9E3779B9) | 0, 0x85EBCA6B)) >>> 0;
    h ^= h >>> 16; h = Math.imul(h, 0x7FEB352D);
    h ^= h >>> 15; h = Math.imul(h, 0x846CA68B);
    h ^= h >>> 16;
    return h >>> 0;
  }

  function princarg(x) {
    return x - TWO_PI * Math.round(x / TWO_PI);
  }

  function clamp(v, lo, hi) {
    return v < lo ? lo : v > hi ? hi : v;
  }

  // ------------------------------------------------------------------- FFT --

  /** Real FFT of size n (power of two) via a complex FFT of size n/2. */
  class RealFFT {
    constructor(n) {
      if (n < 4 || (n & (n - 1)) !== 0) throw new Error('FFT size must be a power of two');
      const m = n >> 1;
      this.n = n;
      this.m = m;
      this.re = new Float64Array(m);
      this.im = new Float64Array(m);
      this.cos = new Float64Array(m >> 1);
      this.sin = new Float64Array(m >> 1);
      for (let k = 0; k < (m >> 1); k++) {
        this.cos[k] = Math.cos(TWO_PI * k / m);
        this.sin[k] = Math.sin(TWO_PI * k / m);
      }
      this.wc = new Float64Array(m + 1);
      this.ws = new Float64Array(m + 1);
      for (let k = 0; k <= m; k++) {
        this.wc[k] = Math.cos(TWO_PI * k / n);
        this.ws[k] = Math.sin(TWO_PI * k / n);
      }
      this.rev = new Uint32Array(m);
      const bits = Math.round(Math.log2(m));
      for (let i = 0; i < m; i++) {
        let r = 0;
        for (let b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        this.rev[i] = r;
      }
    }

    _fft(re, im, inverse) {
      const m = this.m, rev = this.rev, cs = this.cos, sn = this.sin;
      for (let i = 0; i < m; i++) {
        const j = rev[i];
        if (j > i) {
          let t = re[i]; re[i] = re[j]; re[j] = t;
          t = im[i]; im[i] = im[j]; im[j] = t;
        }
      }
      for (let size = 2; size <= m; size <<= 1) {
        const half = size >> 1, step = m / size;
        for (let j = 0; j < half; j++) {
          const wr = cs[j * step];
          const wi = inverse ? sn[j * step] : -sn[j * step];
          for (let i = j; i < m; i += size) {
            const b = i + half;
            const xr = re[b] * wr - im[b] * wi;
            const xi = re[b] * wi + im[b] * wr;
            re[b] = re[i] - xr; im[b] = im[i] - xi;
            re[i] += xr; im[i] += xi;
          }
        }
      }
    }

    /** x: real, length n → outRe/outIm: bins 0..n/2 (unnormalised). */
    forward(x, outRe, outIm) {
      const m = this.m, re = this.re, im = this.im, wc = this.wc, ws = this.ws;
      for (let i = 0; i < m; i++) { re[i] = x[2 * i]; im[i] = x[2 * i + 1]; }
      this._fft(re, im, false);
      for (let k = 0; k <= m; k++) {
        const k1 = k === m ? 0 : k;
        const k2 = k === 0 ? 0 : m - k;
        const zr = re[k1], zi = im[k1];
        const cr = re[k2], ci = -im[k2];
        const er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
        const or = 0.5 * (zi - ci), oi = -0.5 * (zr - cr);
        const wr = wc[k], wi = -ws[k];
        outRe[k] = er + (or * wr - oi * wi);
        outIm[k] = ei + (or * wi + oi * wr);
      }
    }

    /** bins 0..n/2 → real x of length n (normalised, inverse of forward). */
    inverse(inRe, inIm, x) {
      const m = this.m, re = this.re, im = this.im, wc = this.wc, ws = this.ws;
      for (let k = 0; k < m; k++) {
        const ar = inRe[k], ai = inIm[k];
        const br = inRe[m - k], bi = -inIm[m - k];
        const er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
        const dr = 0.5 * (ar - br), di = 0.5 * (ai - bi);
        const wr = wc[k], wi = ws[k];
        const or = dr * wr - di * wi, oi = dr * wi + di * wr;
        re[k] = er - oi;
        im[k] = ei + or;
      }
      this._fft(re, im, true);
      const s = 1 / m;
      for (let i = 0; i < m; i++) { x[2 * i] = re[i] * s; x[2 * i + 1] = im[i] * s; }
    }
  }

  /** Grain size: the power of two closest to ~170 ms. */
  function windowSizeFor(sr) {
    const target = sr * 0.17;
    let n = 1024;
    while (n * 2 <= target * 1.42) n *= 2;
    return n;
  }

  function smoothNoise(knots, t) {
    const i = Math.floor(t);
    const f = t - i;
    const u = 0.5 - 0.5 * Math.cos(Math.PI * f);
    const a = knots[Math.min(i, knots.length - 1)];
    const b = knots[Math.min(i + 1, knots.length - 1)];
    return a + (b - a) * u;
  }

  // ------------------------------------------------------------ StretchJob --

  class StretchJob {
    /**
     * @param {Float32Array} input  one channel of the source region
     * @param {object} o  { sr, stretch, pitch (semitones), weave (1–16),
     *                      scatter (0–1), driftMs, seed, n? }
     */
    constructor(input, o) {
      this.input = input;
      this.inLen = input.length;
      const N = this.N = o.n || windowSizeFor(o.sr);
      const H = this.H = (N >> 1) + 1;
      this.stretch = Math.max(0.01, +o.stretch || 1);
      this.weave = clamp(Math.round(+o.weave || 1), 1, 16);
      this.hop = Math.max(1, Math.round(N / this.weave));
      this.ratio = Math.pow(2, (+o.pitch || 0) / 12);
      this.shift = Math.abs(this.ratio - 1) > 1e-9;
      this.scatter = clamp(+o.scatter || 0, 0, 1);
      this.pv = this.scatter < 1;
      this.seed = (o.seed >>> 0) || 1;
      this.drift = Math.max(0, (+o.driftMs || 0) * o.sr / 1000);
      this.delta = N >> 3;

      this.outLen = Math.max(1, Math.round(this.inLen * this.stretch));
      this.out = new Float32Array(this.outLen);
      this.frames = Math.ceil((this.outLen + N / 2) / this.hop) + 1;
      this.frame = 0;
      this.started = false;

      const w = this.win = new Float64Array(N);
      let sw2 = 0;
      for (let n = 0; n < N; n++) {
        w[n] = 0.5 - 0.5 * Math.cos(TWO_PI * n / N);
        sw2 += w[n] * w[n];
      }
      // Level compensation: coherent overlap-add vs. incoherent (random
      // phase) overlap-add, blended by scatter, so weave/scatter changes
      // keep roughly the same loudness.
      const gCoh = this.hop / sw2;
      const gInc = Math.sqrt(N * this.hop) / sw2;
      this.gain = Math.pow(gCoh, 1 - this.scatter) * Math.pow(gInc, this.scatter);

      this.fft = new RealFFT(N);
      this.fa = new Float64Array(N);
      this.fb = new Float64Array(N);
      this.fy = new Float64Array(N);
      this.aRe = new Float64Array(H); this.aIm = new Float64Array(H);
      this.bRe = new Float64Array(H); this.bIm = new Float64Array(H);
      this.mag = new Float64Array(H);
      this.phA = new Float64Array(H);
      this.omega = new Float64Array(H);
      this.oMag = new Float64Array(H);
      this.oPh = new Float64Array(H);
      this.prev = new Float64Array(H);
      this.sRe = new Float64Array(H); this.sIm = new Float64Array(H);
      this.peaks = new Int32Array(H);
      this.binW = new Float64Array(H);
      for (let k = 0; k < H; k++) this.binW[k] = TWO_PI * k / N;

      if (this.drift > 0) {
        const rng = mulberry32(hash32(this.seed, 0xD1F7));
        this.k1 = Math.max(1, Math.round(o.sr * 2.9));
        this.k2 = Math.max(1, Math.round(o.sr * 0.7));
        this.knots1 = new Float32Array(Math.ceil((this.outLen + N) / this.k1) + 3);
        this.knots2 = new Float32Array(Math.ceil((this.outLen + N) / this.k2) + 3);
        for (let i = 0; i < this.knots1.length; i++) this.knots1[i] = rng() * 2 - 1;
        for (let i = 0; i < this.knots2.length; i++) this.knots2[i] = rng() * 2 - 1;
      }
    }

    get done() { return this.frame >= this.frames; }
    get progress() { return Math.min(1, this.frame / this.frames); }

    /** Process up to `count` grains. Returns true when finished. */
    step(count) {
      const end = Math.min(this.frames, this.frame + (count || 1));
      for (; this.frame < end; this.frame++) this._grain(this.frame);
      return this.done;
    }

    run() {
      while (!this.done) this.step(64);
      return this.out;
    }

    _read(center, dest) {
      const N = this.N, half = N >> 1, mask = N - 1;
      const w = this.win, x = this.input, L = this.inLen;
      const start = Math.round(center) - half;
      const n0 = clamp(-start, 0, N), n1 = clamp(L - start, 0, N);
      for (let n = 0; n < n0; n++) dest[(n + half) & mask] = 0;
      for (let n = n0; n < n1; n++) dest[(n + half) & mask] = x[start + n] * w[n];
      for (let n = Math.max(n0, n1); n < N; n++) dest[(n + half) & mask] = 0;
    }

    _driftAt(c, j) {
      const s1 = smoothNoise(this.knots1, c / this.k1);
      const s2 = smoothNoise(this.knots2, c / this.k2);
      const jit = mulberry32(hash32(this.seed ^ 0x5BD1E995, j))() * 2 - 1;
      return this.drift * (0.4 * s1 + 0.2 * s2 + 0.4 * jit);
    }

    _grain(j) {
      const N = this.N, H = this.H, half = N >> 1, mask = N - 1, hop = this.hop;
      const mag = this.mag, phA = this.phA, omega = this.omega;
      const oMag = this.oMag, oPh = this.oPh, prev = this.prev, binW = this.binW;
      const pv = this.pv;

      const c = j * hop;                       // grain centre, output samples
      const base = c / this.stretch;           // read centre, input samples
      let p = base;
      if (this.drift > 0) {
        p = base + this._driftAt(c, j);
        const L = this.inLen;
        if (base <= L) {                       // reflect wandering reads back in
          if (p < 0) p = -p;
          if (p > L) p = 2 * L - p;
          if (p < 0) p = 0;
        }
      }

      // ---- analysis
      const aRe = this.aRe, aIm = this.aIm;
      this._read(p, this.fa);
      this.fft.forward(this.fa, aRe, aIm);
      let maxMag = 0, maxK = 0;
      for (let k = 0; k < H; k++) {
        const m = Math.sqrt(aRe[k] * aRe[k] + aIm[k] * aIm[k]);
        mag[k] = m;
        if (m > maxMag) { maxMag = m; maxK = k; }
      }
      if (maxMag < 1e-10) return;              // silence: nothing to add

      if (pv) {
        // True frequency of every bin from a second, slightly later frame.
        const bRe = this.bRe, bIm = this.bIm, d = this.delta;
        for (let k = 0; k < H; k++) phA[k] = Math.atan2(aIm[k], aRe[k]);
        this._read(p + d, this.fb);
        this.fft.forward(this.fb, bRe, bIm);
        for (let k = 0; k < H; k++) {
          const phB = Math.atan2(bIm[k], bRe[k]);
          omega[k] = binW[k] + princarg(phB - phA[k] - binW[k] * d) / d;
        }
      }

      // ---- build the output spectrum
      if (!this.shift && !pv) {
        oMag.set(mag);
      } else {
        oMag.fill(0);
        if (pv) {
          if (this.started) for (let k = 0; k < H; k++) oPh[k] = prev[k] + binW[k] * hop;
          else oPh.set(phA);
        }
        // peaks (local maxima over ±2 bins, above −80 dB re. frame max)
        const peaks = this.peaks, thr = maxMag * 1e-4;
        let np = 0;
        for (let k = 2; k < H - 2; k++) {
          const m = mag[k];
          if (m > thr && m > mag[k - 1] && m >= mag[k + 1] && m > mag[k - 2] && m >= mag[k + 2]) peaks[np++] = k;
        }
        if (np === 0) peaks[np++] = maxK;

        const r = this.ratio, shift = this.shift, started = this.started;
        let lo = 0;
        for (let i = 0; i < np; i++) {
          const kp = peaks[i];
          let hi = H;
          if (i < np - 1) {                    // region ends at the trough
            const kn = peaks[i + 1];
            let mi = kp + 1;
            for (let k = kp + 2; k < kn; k++) if (mag[k] < mag[mi]) mi = k;
            hi = mi;
          }
          const d = shift ? Math.round(kp * (r - 1)) : 0;
          const jp = kp + d;
          if (jp >= 0 && jp < H) {
            let phs = 0;
            if (pv) phs = started ? prev[jp] + r * omega[kp] * hop : phA[kp];
            const k0 = Math.max(lo, -d), k1 = Math.min(hi, H - d);
            for (let k = k0; k < k1; k++) {
              const jj = k + d;
              oMag[jj] += mag[k];
              if (pv) oPh[jj] = phs + phA[k] - phA[kp];
            }
          }
          lo = hi;
        }
      }

      // ---- synthesis
      const s = this.scatter, sRe = this.sRe, sIm = this.sIm;
      const rng = s > 0 ? mulberry32(hash32(this.seed, j)) : null;
      const sp = s * Math.PI;
      for (let k = 0; k < H; k++) {
        let ph;
        if (!pv) ph = TWO_PI * rng();
        else {
          ph = oPh[k];
          if (s > 0) ph += sp * (2 * rng() - 1);
        }
        const m = oMag[k];
        sRe[k] = m * Math.cos(ph);
        sIm[k] = m * Math.sin(ph);
      }
      sIm[0] = 0; sIm[H - 1] = 0;
      if (pv) {
        for (let k = 0; k < H; k++) prev[k] = princarg(oPh[k]);
        this.started = true;
      }

      const fy = this.fy, w = this.win, out = this.out, g = this.gain;
      this.fft.inverse(sRe, sIm, fy);
      const start = c - half;
      const n0 = Math.max(0, -start), n1 = Math.min(N, this.outLen - start);
      for (let n = n0; n < n1; n++) out[start + n] += fy[(n + half) & mask] * w[n] * g;
    }
  }

  // ---------------------------------------------------------------- reverb --

  /**
   * Stereo impulse response: decaying noise that darkens over time.
   * `size` is the RT60 in seconds. Each channel has unit energy so the wet
   * signal sits at about the same loudness as the dry one.
   */
  function makeIR(sr, size, seed) {
    size = Math.max(0.05, size);
    const pre = Math.round(0.012 * sr);
    const len = pre + Math.max(1, Math.round(size * sr));
    const chans = [];
    for (let c = 0; c < 2; c++) {
      const rng = mulberry32(((seed || 7) + c * 7919) >>> 0);
      const h = new Float32Array(len);
      let lp = 0, energy = 0;
      for (let i = 0; i < len - pre; i++) {
        const t = i / sr;
        const env = Math.exp(-6.9078 * t / size) * Math.min(1, t / 0.004);
        const a = 0.82 * Math.exp(-t / (size * 0.5)) + 0.1;   // damping
        lp += a * ((rng() * 2 - 1) - lp);
        const v = lp * env;
        h[pre + i] = v;
        energy += v * v;
      }
      const sc = 1 / Math.sqrt(energy || 1);
      for (let i = 0; i < len; i++) h[i] *= sc;
      chans.push(h);
    }
    return chans;
  }

  const Stretch = { StretchJob, RealFFT, windowSizeFor, makeIR, mulberry32, hash32 };
  if (typeof module !== 'undefined' && module.exports) module.exports = Stretch;
  root.Stretch = Stretch;
})(typeof self !== 'undefined' ? self : this);
