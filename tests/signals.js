// Deterministic test inputs shared by the tests: a voice-like pulse train and stereo material.
'use strict';
function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296 * 2 - 1; }; }
// Glottal-flow-like pulse train (Rosenberg pulses, DC removed) with slight vibrato and soft breath noise.
function pulses(sr, seconds, f0 = 130, breath = 0.01, seed = 7) {
  const n = Math.round(sr * seconds), out = new Float32Array(n), r = rng(seed);
  const lp = 1 - Math.exp(-2 * Math.PI * 1500 / sr);
  let ph = 0, dc = 0, b = 0;
  for (let i = 0; i < n; i++) {
    const f = f0 * (1 + 0.01 * Math.sin(2 * Math.PI * 5 * i / sr));
    ph += f / sr; if (ph >= 1) ph -= 1;
    const tp = 0.4, tn = 0.16;
    let g = 0;
    if (ph < tp) g = 0.5 * (1 - Math.cos(Math.PI * ph / tp));
    else if (ph < tp + tn) g = Math.cos(Math.PI * (ph - tp) / (2 * tn));
    b += (r() - b) * lp;
    const x = 0.12 * g + breath * b;
    dc += (x - dc) * 0.002;
    out[i] = x - dc;
  }
  return out;
}
// Stereo: left = noise bursts (drum-like), right = a sawtooth chord
function stereo(sr, seconds, seed = 9) {
  const n = Math.round(sr * seconds), L = new Float32Array(n), R = new Float32Array(n), r = rng(seed);
  for (let i = 0; i < n; i++) {
    const t = i % Math.round(sr * 0.25);
    L[i] = t < sr * 0.05 ? r() * 0.6 * Math.exp(-t / (sr * 0.012)) : 0;
    let s = 0; for (const f of [110, 165, 220]) s += ((i * f / sr) % 1) * 2 - 1;
    R[i] = 0.08 * s;
  }
  return [L, R];
}
module.exports = { rng, pulses, stereo };
