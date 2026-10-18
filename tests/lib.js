// Small helpers for the tests: FFT, averaged spectra, peak picking, levels.
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) { let bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
  for (let len = 2; len <= n; len <<= 1) { const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang), h = len >> 1;
    for (let i = 0; i < n; i += len) { let cr = 1, ci = 0;
      for (let j = 0; j < h; j++) { const a = i + j, b = a + h; const vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - vr; im[b] = im[a] - vi; re[a] += vr; im[a] += vi; const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t; } } }
}
// averaged power spectrum (dB) of x, Welch with Hann, size N
function welch(x, N) {
  const P = new Float64Array(N / 2); let cnt = 0;
  for (let o = 0; o + N <= x.length; o += N / 2) {
    const re = new Float64Array(N), im = new Float64Array(N);
    for (let i = 0; i < N; i++) re[i] = x[o + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / N));
    fft(re, im); for (let k = 0; k < N / 2; k++) P[k] += re[k] * re[k] + im[k] * im[k]; cnt++;
  }
  return Array.from(P, v => 10 * Math.log10(v / cnt + 1e-30));
}
function peaks(db, binHz, fmax, minProm) {
  // smooth a little then local maxima with prominence
  const s = db.map((v, i) => { let a = 0, c = 0; for (let k = -3; k <= 3; k++) if (db[i + k] !== undefined) { a += db[i + k]; c++; } return a / c; });
  const out = [];
  for (let i = 2; i < s.length - 2 && i * binHz < fmax; i++) {
    if (s[i] > s[i - 1] && s[i] >= s[i + 1]) {
      let l = s[i], r = s[i];
      for (let k = i; k > 0 && k > i - 60; k--) l = Math.min(l, s[k]);
      for (let k = i; k < s.length && k < i + 60; k++) r = Math.min(r, s[k]);
      if (s[i] - Math.max(l, r) > (minProm || 3)) out.push(Math.round(i * binHz));
    }
  }
  return out;
}
function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296 * 2 - 1; }; }
function rmsDb(x, a, b) { let s = 0; for (let i = a; i < b; i++) s += x[i] * x[i]; return 10 * Math.log10(s / (b - a) + 1e-30); }
module.exports = { fft, welch, peaks, rng, rmsDb };
