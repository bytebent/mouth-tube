// Offline tests for the mouth-tube engine. Run: node tests/engine-test.js
'use strict';
const path = require('path');
const { MouthTube, PARAMS } = require(path.join(__dirname, '..', 'reference', 'mouth-tube.js'));
const { rmsDb, welch, peaks } = require('./lib.js');
const { rng, pulses } = require('./signals.js');
const SR = 48000;
let fails = 0;
const ok = (c, msg) => { console.log((c ? 'ok   ' : 'FAIL ') + msg); if (!c) fails++; };
// voice-like input: n samples of a glottal-flow-like pulse train
function voice(n, f0 = 130) { return pulses(SR, n / SR, f0); }
function run(m, xl, xr, chunk = 128) {
  const n = xl.length, yl = new Float32Array(n), yr = new Float32Array(n);
  for (let o = 0; o < n; o += chunk) {
    const c = Math.min(chunk, n - o);
    m.process(xl.subarray(o, o + c), xr ? xr.subarray(o, o + c) : null, yl.subarray(o, o + c), yr.subarray(o, o + c), c);
  }
  return [yl, yr];
}
const mk = (set = {}) => { const m = new MouthTube(SR); for (const k in set) m.setParam(k, set[k]); m.snap(); return m; };

// 1. latency + Mix 0 = delayed dry
{
  const r = rng(1), x = new Float32Array(4800).map(() => r() * 0.5);
  const m = mk({ mix: 0 }); const [y] = run(m, x);
  let err = 0; for (let i = m.latency; i < x.length; i++) err = Math.max(err, Math.abs(y[i] - x[i - m.latency]));
  ok(err < 1e-6, `Mix 0 passes the dry signal delayed by the latency (${m.latency} samples = ${(m.latency / SR * 1000).toFixed(2)} ms), max err ${err.toExponential(1)}`);
}
// 2. level vs dry with a voice-like input. Output gains were matched to Throat's glottal flow at C2;
//    radiation adds +6 dB/oct, so brighter inputs (like this test pulse) come out louder with it on.
{
  const x = voice(SR * 2);
  const dry = rmsDb(x, SR / 2, SR * 2);
  for (const rad of [1, 0]) {
    const [y] = run(mk({ radiation: rad }), x);
    const wet = rmsDb(y, SR / 2, SR * 2);
    console.log(`     level radiation ${rad ? 'on ' : 'off'}: dry ${dry.toFixed(1)} dB, wet ${wet.toFixed(1)} dB (diff ${(wet - dry).toFixed(1)})`);
    ok(Math.abs(wet - dry) < (rad ? 8 : 3), `radiation ${rad ? 'on' : 'off'} level within ${rad ? 8 : 3} dB of dry`);
  }
}
// 3. buffer-size independence
{
  const r = rng(2), x = new Float32Array(9600).map(() => r() * 0.3);
  const set = { tongueX: 0.2, tongueY: 0.3, spread: 0.6, length: 12 };
  const ref = run(mk(set), x, null, 9600)[0];
  let worst = 0;
  for (const c of [1, 7, 32, 128, 1000]) { const y = run(mk(set), x, null, c)[0]; for (let i = 0; i < x.length; i++) worst = Math.max(worst, Math.abs(y[i] - ref[i])); }
  ok(worst === 0, `same output for host buffer sizes 1, 7, 32, 128, 1000 (max diff ${worst})`);
}
// 4. Length scales formants
{
  const { fft } = require('./lib.js');
  const fm = len => {
    const m = mk({ length: len, radiation: 0, tongueX: 0.5, tongueY: 0.5, damping: 0.2 });
    const n = 1 << 15, x = new Float32Array(n); x[0] = 0.5;
    const [y] = run(m, x);
    const re = Float64Array.from(y), im = new Float64Array(n); fft(re, im);
    const db = []; for (let k = 0; k < n / 2; k++) db.push(10 * Math.log10(re[k] ** 2 + im[k] ** 2 + 1e-30));
    const pk = peaks(db, SR / n, 8000, 1); console.log('     ', len, 'cm formants', pk.slice(0, 4).join(' ')); return pk[0];
  };
  const a = fm(17), b = fm(8.5), c = fm(34);
  ok(Math.abs(b / a - 2) < 0.12 && Math.abs(a / c - 2) < 0.12, `F1 scales with Length: 34 cm ${c} Hz, 17 cm ${a} Hz, 8.5 cm ${b} Hz`);
}
// 5. closures
{
  const x = voice(SR);
  const lvl = set => rmsDb(run(mk(set), x)[0], SR / 2, SR);
  const open = lvl({}), lipsShut = lvl({ lips: 0 }), hum = lvl({ lips: 0, nasal: 1 }), tipShut = lvl({ tip: 1 }), tipHum = lvl({ tip: 1, nasal: 1 });
  console.log(`     open ${open.toFixed(1)}, lips shut ${lipsShut.toFixed(1)}, lips shut + nose ${hum.toFixed(1)}, tip shut ${tipShut.toFixed(1)}, tip shut + nose ${tipHum.toFixed(1)} dB`);
  ok(lipsShut < open - 40, 'closed lips with the velum up let (almost) nothing out');
  ok(tipShut < open - 40, 'tongue tip closure with the velum up lets (almost) nothing out');
  ok(hum > open - 25 && hum < open - 4, 'closed lips + open velum: sound comes out of the nose, quieter');
  ok(tipHum > open - 25, 'tip closure + open velum: nasal murmur');
}
// 6. stereo and spread
{
  const x = voice(SR);
  let [l, r] = run(mk({}), x); let d = 0; for (let i = 0; i < l.length; i++) d = Math.max(d, Math.abs(l[i] - r[i]));
  ok(d === 0, 'mono input, Spread 0: left = right');
  [l, r] = run(mk({ spread: 1 }), x); d = 0; for (let i = SR / 4; i < l.length; i++) d += (l[i] - r[i]) ** 2;
  ok(d > 1e-3, 'mono input, Spread 1: left and right differ');
  // spread up then down: no clicks on the right channel
  const m = mk({}); const out = [];
  const chunk = 4800; let jumps = 0;
  for (let k = 0; k < 10; k++) {
    m.setParam('spread', k >= 2 && k < 6 ? 1 : 0);
    const seg = x.subarray(k * chunk, (k + 1) * chunk); const [a, b] = run(m, seg); out.push(b);
  }
  const R = Float32Array.from(out.flatMap(a => Array.from(a)));
  let maxStep = 0; for (let i = 1; i < R.length; i++) maxStep = Math.max(maxStep, Math.abs(R[i] - R[i - 1]));
  const [ref] = run(mk({}), x); let refStep = 0; for (let i = 1; i < ref.length; i++) refStep = Math.max(refStep, Math.abs(ref[i] - ref[i - 1]));
  ok(maxStep < refStep * 2.5, `Spread on/off: biggest step ${maxStep.toFixed(4)} vs ${refStep.toFixed(4)} with no spread (no clicks)`);
  // true stereo input keeps channels separate
  const r2 = rng(9), xl = new Float32Array(SR / 2).map(() => r2() * 0.2), xr = new Float32Array(SR / 2);
  [l, r] = run(mk({}), xl, xr); ok(rmsDb(r, 4800, r.length) < -100, 'stereo input: silent right input stays silent (dual mono)');
}
// 7. stability: extremes, fast moves, all inputs
{
  const r = rng(5), x = new Float32Array(SR * 6).map((_, i) => r() * 0.9 * (i % 20000 < 10000 ? 1 : 0));
  x.set(voice(SR * 2, 65), SR * 2);
  const m = mk({}); const n = x.length, yl = new Float32Array(n), yr = new Float32Array(n);
  const ids = Object.keys(PARAMS); let bad = false, peak = 0;
  for (let o = 0; o < n; o += 256) {
    if ((o / 256) % 3 === 0) for (const id of ids) { const p = PARAMS[id]; const v = r() < 0.3 ? (r() < 0.5 ? p.min : p.max) : p.min + (p.max - p.min) * (r() * 0.5 + 0.5); m.setParam(id, v); }
    m.setParam('output', 0); m.setParam('mix', 1);
    m.process(x.subarray(o, o + 256), null, yl.subarray(o, o + 256), yr.subarray(o, o + 256), 256);
  }
  for (let i = 0; i < n; i++) { if (!isFinite(yl[i]) || !isFinite(yr[i])) bad = true; peak = Math.max(peak, Math.abs(yl[i]), Math.abs(yr[i])); }
  ok(!bad, `6 s of random extreme settings changing every 16 ms: always finite (peak ${peak.toFixed(2)})`);
  const lens = [7, 35]; for (const L of lens) for (const d of [0, 1]) {
    const [y] = run(mk({ length: L, damping: d, tip: 0.99, lips: 0.02, nasal: 1, spread: 1 }), x.subarray(0, SR));
    ok(y.every(isFinite), `Length ${L} cm, Damping ${d}, near-closed: finite`);
  }
}
// 8. CPU
{
  const x = voice(SR * 4);
  for (const [name, set] of [['default (one tube)', {}], ['Spread 1 (two tubes)', { spread: 1 }], ['7 cm + Spread 1 (worst case)', { length: 7, spread: 1 }], ['35 cm + Spread 1', { length: 35, spread: 1, round: 1 }]]) {
    const m = mk(set); run(m, x.subarray(0, SR));
    const t0 = process.hrtime.bigint(); run(m, x.subarray(SR)); const dt = Number(process.hrtime.bigint() - t0) / 1e9;
    console.log(`     CPU ${name}: ${(dt / 3 * 100).toFixed(1)}% of real time`);
  }
}
console.log(fails ? `${fails} FAILED` : 'all passed');
process.exit(fails ? 1 : 0);
