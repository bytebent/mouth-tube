/*
 * mouth-tube v0.1 · bytebent
 *
 * A vocal tract as an audio effect. It makes no sound of its own: whatever comes in
 * travels through a tube shaped like a mouth and comes out of the lips (and the nose).
 *
 *   - Kelly-Lochbaum waveguide: N_ORAL sections from glottis to lips, plus a nasal branch
 *     joined at the velum by a three-way junction. Waves are volume-velocity waves, so a
 *     closed section passes nothing.
 *   - The tube shape comes from the controls: tongue position (front/back, close/open),
 *     tongue tip, jaw, lip opening, lip rounding, velum. Nothing in here knows about
 *     vowels or consonants; sounds come from the shape alone.
 *   - Tract length is real: the tube runs at its own rate (sections × speed of sound /
 *     length) and windowed-sinc resampling moves audio in and out, so formants scale
 *     smoothly with Length.
 *   - Dual mono: one tube per channel. Spread makes the two tubes differ slightly.
 *   - Radiation at the lips (flow → pressure, +6 dB/oct), Mix, Output.
 *
 * process() never allocates. Works in an AudioWorklet and in Node (module.exports).
 */
(function (root) {
  'use strict';

  const N_ORAL = 40;                  // oral sections, glottis → lips
  const N_NOSE = 25;                  // nasal sections, velum → nostrils
  const VELUM = 16;                   // junction index where the nose joins (between sections 15 and 16)
  const SOUND = 350;                  // m/s, warm moist air
  const MIN_AREA = 1e-6;
  const GLOT_REFL = 0.85;             // flow-wave reflection at the (mostly closed) glottis end (closed = +1)
  const LIP_REFL = -0.97;             // flow-wave reflection at the open lips (open = -1) at low frequencies...
  const LIP_RAD_HZ = 4000;            // ...falling above this (scaled by the opening): wide lips radiate more highs
  const NOSE_REFL = -0.95, NOSE_RAD_HZ = 3000;
  const BLOCK = 32;                   // control block (host samples)
  const ROUND_LEN = 0.10;             // rounding lengthens the tube by up to 10 %
  const SPREAD_LEN = 0.06;            // spread: ± 6 % length per side
  const SPREAD_TONGUE = 0.08;         // spread: ± tongue front/back offset per side
  const LEN_MIN = 7, LEN_MAX = 35;    // cm
  const RAD_REF_HZ = 1000;            // radiation filter has unity gain here

  // level match vs. the dry signal, measured on a glottal-flow source (Throat at C2, modal voice) with the default mouth
  const GAIN_RAD = 7.1;
  const GAIN_FLOW = 1.58;

  /* ---------- windowed-sinc kernel (shared) ---------- */

  const HW = 8;                       // zero crossings each side
  const KRES = 512;                   // table points per zero crossing
  const KT = new Float64Array(HW * KRES + 2);
  for (let k = 0; k <= HW * KRES; k++) {
    const x = k / KRES;
    const s = x === 0 ? 1 : Math.sin(Math.PI * x) / (Math.PI * x);
    const w = 0.42 + 0.5 * Math.cos(Math.PI * x / HW) + 0.08 * Math.cos(2 * Math.PI * x / HW);
    KT[k] = s * w;
  }
  KT[HW * KRES] = 0; KT[HW * KRES + 1] = 0;
  const CUT = 0.45;                   // resampler cutoff as a fraction of the lower rate

  /* ---------- parameters ---------- */

  const PARAMS = {
    tongueX:   { def: 0.55, min: 0, max: 1 },     // 0 front … 1 back
    tongueY:   { def: 0.7,  min: 0, max: 1 },     // 0 close … 1 open
    tip:       { def: 0,    min: 0, max: 1 },
    jaw:       { def: 0.3,  min: 0, max: 1 },
    lips:      { def: 0.6,  min: 0, max: 1 },     // opening
    round:     { def: 0,    min: 0, max: 1 },
    nasal:     { def: 0,    min: 0, max: 1 },     // velum opening
    length:    { def: 17,   min: LEN_MIN, max: LEN_MAX },   // cm
    damping:   { def: 0.5,  min: 0, max: 1 },
    spread:    { def: 0,    min: 0, max: 1 },
    radiation: { def: 1,    min: 0, max: 1 },     // switch
    mix:       { def: 1,    min: 0, max: 1 },
    output:    { def: 0,    min: -24, max: 12 },  // dB
  };
  const SMOOTHED = ['tongueX', 'tongueY', 'tip', 'jaw', 'lips', 'round', 'nasal', 'length', 'damping', 'spread', 'radiation', 'mix', 'output'];

  /* ---------- tube shape from the controls ---------- */

  const clamp01 = v => v < 0 ? 0 : v > 1 ? 1 : v;
  const lerp = (a, b, t) => a + (b - a) * t;
  const bilin = (fc, bc, fo, bo, X, Y) => lerp(lerp(fc, bc, X), lerp(fo, bo, X), Y);
  const NOSE_D = new Float64Array(N_NOSE);
  for (let k = 0; k < N_NOSE; k++) {
    const y = (k + 0.5) / N_NOSE;
    NOSE_D[k] = y < 0.3 ? 0.6 + 1.2 * y / 0.3 : y < 0.8 ? 1.8 : 1.8 - 0.9 * (y - 0.8) / 0.2;
  }

  // Tongue shapes at the four corners of the vowel chart, as diameters at CX along the tract
  // (0 = glottis, 1 = lips; the lips themselves are separate). The middle of the chart is a
  // uniform tube; everything in between is a blend. Tuned in tests/tune.js.
  const CX = [0.08, 0.16, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85, 0.92];
  const CORNERS = {
    fc: [1.20, 1.74, 2.23, 2.77, 2.44, 1.67, 1.04, 0.74, 0.87, 1.34],    // front close
    bc: [1.20, 1.52, 1.78, 2.50, 1.48, 0.74, 1.16, 1.84, 2.19, 1.34],    // back close
    fo: [1.20, 1.23, 1.60, 1.88, 2.09, 2.28, 2.32, 2.12, 2.37, 3.53],    // front open
    bo: [1.20, 0.63, 0.68, 0.76, 1.01, 1.55, 2.36, 2.82, 2.81, 2.49],    // back open
  };
  const REST = 2.0;
  const NC = CX.length;
  const cornerLog = { fc: new Float64Array(NC), bc: new Float64Array(NC), fo: new Float64Array(NC), bo: new Float64Array(NC) };
  function prepCorners() { for (const k in CORNERS) for (let i = 0; i < NC; i++) cornerLog[k][i] = Math.log(CORNERS[k][i]); }
  prepCorners();
  const ctl = new Float64Array(NC);

  // Fills d (N_ORAL diameters, arbitrary units, area = d²) and returns the nasal coupling area.
  // side: -1 / +1 for the two spread tubes, 0 for the centre shape.
  function shape(s, d, side) {
    const X = clamp01(s.tongueX + side * s.spread * SPREAD_TONGUE), Y = clamp01(s.tongueY);
    // how far the tongue is from rest: 0 in the middle of the chart, 1 in the corners
    const m = Math.min(1, Math.sqrt((2 * X - 1) ** 2 + (2 * Y - 1) ** 2) / Math.SQRT2);
    const lr = Math.log(REST), C = cornerLog;
    for (let i = 0; i < NC; i++) ctl[i] = Math.exp(lr + m * (bilin(C.fc[i], C.bc[i], C.fo[i], C.bo[i], X, Y) - lr));
    ctl[0] = CORNERS.fc[0];
    for (let i = 0; i < N_ORAL; i++) {
      const x = (i + 0.5) / N_ORAL;
      let v;
      if (x <= CX[0]) v = ctl[0];
      else if (x >= CX[NC - 1]) v = ctl[NC - 1];
      else {
        let k = 1; while (CX[k] < x) k++;
        const t = (x - CX[k - 1]) / (CX[k] - CX[k - 1]);
        v = lerp(ctl[k - 1], ctl[k], 0.5 - 0.5 * Math.cos(Math.PI * t));
      }
      if (x > 0.55) v *= 1 + 0.8 * (s.jaw - 0.3) * clamp01((x - 0.55) / 0.2);   // jaw: opens (or closes) the mouth cavity
      const dt = x - 0.86;                                   // tongue tip at the ridge behind the teeth
      if (dt > -0.045 && dt < 0.045) v *= Math.max(0, 1 - s.tip * 1.05 * 0.5 * (1 + Math.cos(Math.PI * dt / 0.045)));
      if (x > 0.89 && x < 0.95) v *= 1 - 0.25 * s.round;
      d[i] = v > 0 ? v : 0;
    }
    // lips: the last two sections
    const lip = 2.6 * Math.pow(s.lips, 1.2) * (1 - 0.7 * s.round);
    d[N_ORAL - 1] = lip;
    d[N_ORAL - 2] = lerp(d[N_ORAL - 2], lip, 0.6);
    // velum: lowering it opens the nose and narrows the oral tube there a little
    d[VELUM - 1] *= 1 - 0.2 * s.nasal;
    d[VELUM] *= 1 - 0.2 * s.nasal;
    const vn = 1.0 * s.nasal;
    return vn * vn;
  }

  /* ---------- one tube (waveguide) ---------- */

  class Tube {
    constructor() {
      this.R = new Float64Array(N_ORAL); this.L = new Float64Array(N_ORAL);
      this.jR = new Float64Array(N_ORAL); this.jL = new Float64Array(N_ORAL);
      this.NR = new Float64Array(N_NOSE); this.NL = new Float64Array(N_NOSE);
      this.nR = new Float64Array(N_NOSE); this.nL = new Float64Array(N_NOSE);
      this.c = new Float64Array(N_ORAL);         // junction weight 2·A[j]/(A[j-1]+A[j])
      this.nc = new Float64Array(N_NOSE);
      this.q0 = 0; this.q1 = 0; this.q2 = 0;     // velum three-way weights
      this.g = 0.999; this.gn = 0.998;
      this.lp = 0.5; this.np = 0.5;              // end-reflection low-pass coefficients
      this.lz = 0; this.nz = 0;
      this.d = new Float64Array(N_ORAL);
      this.A = new Float64Array(N_ORAL);
      this.nA = new Float64Array(N_NOSE);
    }
    reset() { this.R.fill(0); this.L.fill(0); this.NR.fill(0); this.NL.fill(0); this.lz = 0; this.nz = 0; }
    copyFrom(t) {
      this.R.set(t.R); this.L.set(t.L); this.NR.set(t.NR); this.NL.set(t.NL); this.lz = t.lz; this.nz = t.nz;
    }
    setShape(nasalArea) {
      const A = this.A, d = this.d, c = this.c;
      for (let i = 0; i < N_ORAL; i++) { const a = d[i] * d[i]; A[i] = a > MIN_AREA ? a : MIN_AREA; }
      for (let j = 1; j < N_ORAL; j++) c[j] = 2 * A[j] / (A[j - 1] + A[j]);
      const nA = this.nA;
      nA[0] = nasalArea > MIN_AREA ? nasalArea : MIN_AREA;
      for (let k = 1; k < N_NOSE; k++) nA[k] = NOSE_D[k] * NOSE_D[k];
      for (let k = 1; k < N_NOSE; k++) this.nc[k] = 2 * nA[k] / (nA[k - 1] + nA[k]);
      const sum = A[VELUM - 1] + A[VELUM] + nA[0];
      this.q0 = 2 * A[VELUM - 1] / sum; this.q1 = 2 * A[VELUM] / sum; this.q2 = 2 * nA[0] / sum;
    }
    step(x) {
      const R = this.R, L = this.L, jR = this.jR, jL = this.jL, c = this.c;
      const NR = this.NR, NL = this.NL, nR = this.nR, nL = this.nL, nc = this.nc;
      jR[0] = GLOT_REFL * L[0] + x;
      this.lz += (R[N_ORAL - 1] - this.lz) * this.lp;
      jL[N_ORAL - 1] = LIP_REFL * this.lz;
      for (let j = 1; j < VELUM; j++) {
        const s = R[j - 1] + L[j], cj = c[j];
        jR[j] = cj * s - L[j];
        jL[j - 1] = (2 - cj) * s - R[j - 1];
      }
      {
        const s = R[VELUM - 1] + L[VELUM] + NL[0];
        jR[VELUM] = this.q1 * s - L[VELUM];
        jL[VELUM - 1] = this.q0 * s - R[VELUM - 1];
        nR[0] = this.q2 * s - NL[0];
      }
      for (let j = VELUM + 1; j < N_ORAL; j++) {
        const s = R[j - 1] + L[j], cj = c[j];
        jR[j] = cj * s - L[j];
        jL[j - 1] = (2 - cj) * s - R[j - 1];
      }
      this.nz += (NR[N_NOSE - 1] - this.nz) * this.np;
      nL[N_NOSE - 1] = NOSE_REFL * this.nz;
      for (let k = 1; k < N_NOSE; k++) {
        const s = NR[k - 1] + NL[k], ck = nc[k];
        nR[k] = ck * s - NL[k];
        nL[k - 1] = (2 - ck) * s - NR[k - 1];
      }
      const g = this.g, gn = this.gn;
      for (let i = 0; i < N_ORAL; i++) { R[i] = jR[i] * g; L[i] = jL[i] * g; }
      for (let k = 0; k < N_NOSE; k++) { NR[k] = nR[k] * gn; NL[k] = nL[k] * gn; }
      return R[N_ORAL - 1] + NR[N_NOSE - 1];
    }
  }

  /* ---------- one channel: resample in → tube → resample out ---------- */

  const HM = 511, TM = 1023;

  class Channel {
    constructor(eng) {
      this.e = eng;
      this.tube = new Tube();
      this.hist = new Float64Array(HM + 1);
      this.ty = new Float64Array(TM + 1); this.tt = new Float64Array(TM + 1); this.tw = new Float64Array(TM + 1);
      this.tc = 0;
      this.T = 0;               // tube clock, in host samples
      this.dT = 1;              // host samples per tube step
      this.nuIn = CUT; this.nuOut = CUT;
    }
    reset(n) {
      this.tube.reset(); this.hist.fill(0); this.ty.fill(0); this.tt.fill(-1e9); this.tw.fill(0);
      this.tc = 0; this.T = n;
    }
    copyFrom(o) {
      this.tube.copyFrom(o.tube);
      this.hist.set(o.hist); this.ty.set(o.ty); this.tt.set(o.tt); this.tw.set(o.tw);
      this.tc = o.tc; this.T = o.T;
    }
    setRate(rate, sr) {          // rate = tube steps per second
      this.dT = sr / rate;
      const lo = Math.min(1, rate / sr);
      this.nuIn = CUT * lo;      // host → tube: band-limit to the lower of the two rates
      this.nuOut = CUT * lo;     // tube → host: same
    }
    sample(x, n) {
      const hist = this.hist, e = this.e, tube = this.tube;
      hist[n & HM] = x;
      const lim = n - e.dIn;
      if (this.T <= lim) {
        const nu = this.nuIn, f2 = 2 * nu, span = HW / f2, kr = f2 * KRES;
        do {
          const T = this.T;
          const m0 = Math.ceil(T - span), m1 = Math.floor(T + span);
          let acc = 0;
          for (let m = m0; m <= m1; m++) {
            let k = (m - T) * kr; if (k < 0) k = -k;
            const i = k | 0, fr = k - i;
            acc += hist[m & HM] * (KT[i] + (KT[i + 1] - KT[i]) * fr);
          }
          const y = tube.step(acc * f2);
          const tc = this.tc & TM;
          this.ty[tc] = y; this.tt[tc] = T; this.tw[tc] = this.dT;
          this.tc++;
          this.T = T + this.dT;
        } while (this.T <= lim);
      }
      const to = n - e.dIn - e.dOut;
      const nu = this.nuOut, f2 = 2 * nu, span = HW / f2, kr = f2 * KRES;
      let acc = 0;
      const stop = this.tc - TM;
      for (let k = this.tc - 1; k >= 0 && k > stop; k--) {
        const j = k & TM, dt = to - this.tt[j];
        if (dt < -span) continue;
        if (dt > span) break;
        let q = dt * kr; if (q < 0) q = -q;
        const i = q | 0, fr = q - i;
        acc += this.ty[j] * this.tw[j] * (KT[i] + (KT[i + 1] - KT[i]) * fr);
      }
      return acc * f2;
    }
  }

  /* ---------- engine ---------- */

  class MouthTube {
    constructor(sampleRate) {
      this.sr = sampleRate;
      this.p = {}; this.s = {};
      for (const k in PARAMS) { this.p[k] = PARAMS[k].def; this.s[k] = PARAMS[k].def; }
      this.smooth = 1 - Math.exp(-BLOCK / (0.025 * sampleRate));
      // fixed latency: enough look-ahead for the lowest tube rate (longest, rounded, spread tube)
      const maxLen = LEN_MAX / 100 * (1 + ROUND_LEN) * (1 + SPREAD_LEN);
      const minRate = SOUND * N_ORAL / maxLen;
      const nuMin = CUT * Math.min(1, minRate / sampleRate);
      this.dIn = Math.ceil(HW / (2 * nuMin)) + 1;
      this.dOut = Math.ceil(HW / (2 * nuMin)) + 1;
      this.latency = this.dIn + this.dOut;
      this.ch = [new Channel(this), new Channel(this)];
      this.n = 0;
      this.blk = 0;
      this.stereo = false;      // second tube running
      this.xf = 0;              // right channel: 0 = copy of left tube, 1 = own tube
      this.dry = [new Float32Array(256), new Float32Array(256)];
      this.radK = 1 / (2 * Math.sin(Math.PI * RAD_REF_HZ / sampleRate));
      this.prev = [0, 0];
      this.gOut = 1; this.wet = 1; this.rad = 1;
      this.vizD = new Float32Array(N_ORAL); this.vizNasal = 0; this.vizLen = 0.17;
      this.tmpD = new Float64Array(N_ORAL);
      this.updateBlock(false);
      for (const c of this.ch) c.reset(0);
    }

    setParam(id, v) {
      const d = PARAMS[id];
      if (!d) return;
      v = +v;
      if (!(v === v)) return;
      if (v < d.min) v = d.min; else if (v > d.max) v = d.max;
      if (id === 'radiation') v = v >= 0.5 ? 1 : 0;
      this.p[id] = v;
    }

    // jump straight to the current settings (no smoothing), e.g. before playback starts
    snap() { this.updateBlock(false); }

    reset() {
      for (const c of this.ch) c.reset(this.n);
      this.prev[0] = this.prev[1] = 0;
      this.dry[0].fill(0); this.dry[1].fill(0);
    }

    updateBlock(smooth) {
      const P = this.p, S = this.s, c = smooth ? this.smooth : 1;
      for (const k of SMOOTHED) {
        S[k] += (P[k] - S[k]) * c;
        if (Math.abs(P[k] - S[k]) < 1e-6) S[k] = P[k];
      }
      const B = 12 * Math.pow(25, S.damping);       // formant bandwidth from wall loss, Hz
      for (let side = 0; side < 2; side++) {
        const ch = this.ch[side], sg = side ? 1 : -1;
        const len = S.length / 100 * (1 + ROUND_LEN * S.round) * (1 + sg * SPREAD_LEN * S.spread);
        const rate = SOUND * N_ORAL / len;
        ch.setRate(rate, this.sr);
        const t = ch.tube;
        const na = shape(S, t.d, sg);
        t.setShape(na);
        t.g = Math.exp(-Math.PI * B / rate);
        t.gn = Math.exp(-Math.PI * (2 * B + 350) / rate);   // nasal cavities are very lossy
        // a bigger mouth (longer tract, wider lips) radiates lower frequencies: scale with size
        const size = 0.17 / len;
        const lipHz = Math.min(15000, Math.max(800, LIP_RAD_HZ * size * 2 / Math.max(0.3, t.d[N_ORAL - 1])));
        t.lp = 1 - Math.exp(-2 * Math.PI * lipHz / rate);
        t.np = 1 - Math.exp(-2 * Math.PI * NOSE_RAD_HZ * size / rate);
      }
      // centre shape for the live view
      this.vizNasal = shape(S, this.tmpD, 0);
      for (let i = 0; i < N_ORAL; i++) this.vizD[i] = this.tmpD[i];
      this.vizLen = S.length / 100 * (1 + ROUND_LEN * S.round);
      this.gOut = Math.pow(10, S.output / 20);
      this.wet = S.mix;
      this.rad = S.radiation;
    }

    // inL, inR: input (inR null = mono). outL, outR: output. Any n.
    process(inL, inR, outL, outR, n) {
      const ch0 = this.ch[0], ch1 = this.ch[1], dryL = this.dry[0], dryR = this.dry[1];
      const lat = this.latency;
      for (let j = 0; j < n; j++) {
        if (this.blk === 0) {
          this.updateBlock(true);
          const want = inR !== null || this.s.spread > 1e-4;
          if (want && !this.stereo) { ch1.copyFrom(ch0); this.stereo = true; }
          this.wantStereo = want;
        }
        if (++this.blk === BLOCK) this.blk = 0;
        const n0 = this.n++;
        const xl = inL[j], xr = inR !== null ? inR[j] : xl;
        const yl = ch0.sample(xl, n0);
        let yr = yl;
        if (this.stereo) {
          const y1 = ch1.sample(xr, n0);
          this.xf += ((this.wantStereo ? 1 : 0) - this.xf) * 0.004;
          if (!this.wantStereo && this.xf < 1e-3) { this.xf = 0; this.stereo = false; }
          yr = yl + (y1 - yl) * this.xf;
        }
        // radiation (flow → pressure) with crossfade between on and off
        const rad = this.rad, gr = (yl - this.prev[0]) * this.radK, gr2 = (yr - this.prev[1]) * this.radK;
        this.prev[0] = yl; this.prev[1] = yr;
        let wl = (gr * GAIN_RAD - yl * GAIN_FLOW) * rad + yl * GAIN_FLOW;
        let wr = (gr2 * GAIN_RAD - yr * GAIN_FLOW) * rad + yr * GAIN_FLOW;
        // dry, delayed by the latency so Mix doesn't comb-filter
        const di = n0 & 255, dd = (n0 - lat) & 255;
        dryL[di] = xl; dryR[di] = xr;
        const w = this.wet, g = this.gOut;
        let ol = (dryL[dd] + (wl - dryL[dd]) * w) * g;
        let or = (dryR[dd] + (wr - dryR[dd]) * w) * g;
        if (!(ol === ol) || !(or === or) || Math.abs(ol) > 1e6) { this.reset(); ol = 0; or = 0; }
        outL[j] = ol > 1 ? 1 : ol < -1 ? -1 : ol;
        outR[j] = or > 1 ? 1 : or < -1 ? -1 : or;
      }
    }
  }

  MouthTube.PARAMS = PARAMS;
  MouthTube.N_ORAL = N_ORAL;
  MouthTube.VELUM = VELUM;
  MouthTube.SOUND = SOUND;
  MouthTube.shape = shape;
  MouthTube.N_NOSE = N_NOSE;
  MouthTube.NOSE_D = NOSE_D;
  MouthTube.CORNERS = CORNERS;
  MouthTube.prepCorners = prepCorners;

  if (typeof module !== 'undefined' && module.exports) module.exports = { MouthTube, PARAMS, shape, CORNERS, prepCorners };
  else root.MouthTube = MouthTube;
})(typeof globalThis !== 'undefined' ? globalThis : this);
