// Checks that the C++ engine (source/MouthTubeDSP.h) sounds the same as the JS reference.
// Run: g++ -O2 -std=c++17 -ffp-contract=off -o dsp_cli tests/dsp_cli.cpp && node tests/compare.js ./dsp_cli
'use strict';
const fs = require('fs'), os = require('os'), path = require('path'), { execFileSync } = require('child_process');
const { MouthTube } = require(path.join(__dirname, '..', 'reference', 'mouth-tube.js'));
const { pulses, stereo } = require('./signals.js');
const cli = path.resolve(process.argv[2] || './dsp_cli');

function header(script) { const h = {}, a = script.split('\n')[0].trim().split(/\s+/); for (let i = 0; i < a.length; i += 2) h[a[i]] = +a[i + 1]; return h; }
function runJs(script, input) {
  const h = header(script), e = new MouthTube(h.sr), out = [];
  let pos = 0;
  for (const l of script.trim().split('\n').slice(1)) {
    const a = l.trim().split(/\s+/);
    if (a[0] === 'p') e.setParam(a[1], +a[2]);
    else if (a[0] === 'snap') e.snap();
    else if (a[0] === 'run') {
      let left = +a[1];
      while (left > 0) {
        const m = Math.min(h.block, left), L = new Float32Array(m), R = new Float32Array(m), oL = new Float32Array(m), oR = new Float32Array(m);
        for (let i = 0; i < m; i++, pos++) { L[i] = input[pos * h.ch]; if (h.ch > 1) R[i] = input[pos * 2 + 1]; }
        e.process(L, h.ch > 1 ? R : null, oL, oR, m);
        const I = new Float32Array(m * 2); for (let i = 0; i < m; i++) { I[2 * i] = oL[i]; I[2 * i + 1] = oR[i]; }
        out.push(I); left -= m;
      }
    }
  }
  const n = out.reduce((s, b) => s + b.length, 0), r = new Float32Array(n);
  let o = 0; for (const b of out) { r.set(b, o); o += b.length; }
  return r;
}
function runCpp(script, input) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mouthtube-'));
  fs.writeFileSync(path.join(dir, 's.txt'), script);
  fs.writeFileSync(path.join(dir, 'in.f32'), Buffer.from(input.buffer, input.byteOffset, input.byteLength));
  const buf = execFileSync(cli, [path.join(dir, 's.txt'), path.join(dir, 'in.f32')], { maxBuffer: 1 << 30 });
  return new Float32Array(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
}
const mono = (sr, sec, f0) => pulses(sr, sec, f0);
const inter = (sr, sec) => { const [L, R] = stereo(sr, sec), I = new Float32Array(L.length * 2); for (let i = 0; i < L.length; i++) { I[2 * i] = L[i]; I[2 * i + 1] = R[i]; } return I; };
const S = 48000, sec = s => Math.round(s * S);
const H = (sr = 48000, block = 128, ch = 1) => `sr ${sr} block ${block} ch ${ch}`;
const sc = (head, steps) => head + '\n' + steps.join('\n') + '\n';

const cases = [
  ['defaults, voice-like input', sc(H(), [`run ${sec(1)}`]), mono(S, 1)],
  ['vowel corners', sc(H(), ['p tongueX 0', 'p tongueY 0', 'snap', `run ${sec(0.3)}`, 'p tongueX 1', 'p tongueY 1', 'p jaw 0.8', `run ${sec(0.3)}`,
    'p tongueY 0', 'p round 1', 'p lips 0.4', `run ${sec(0.3)}`]), mono(S, 0.9)],
  ['closures and nose', sc(H(), ['p lips 0', 'p nasal 1', `run ${sec(0.3)}`, 'p lips 0.6', 'p tip 1', `run ${sec(0.3)}`, 'p nasal 0', `run ${sec(0.2)}`, 'p tip 0', `run ${sec(0.2)}`]), mono(S, 1)],
  ['length extremes', sc(H(), ['p length 7', 'snap', `run ${sec(0.3)}`, 'p length 35', `run ${sec(0.4)}`, 'p round 1', `run ${sec(0.2)}`]), mono(S, 0.9)],
  ['damping, radiation off, mix, output', sc(H(), ['p damping 0', `run ${sec(0.25)}`, 'p damping 1', 'p radiation 0', `run ${sec(0.25)}`,
    'p mix 0.4', 'p output -9', `run ${sec(0.25)}`, 'p mix 0', 'p output 12', `run ${sec(0.25)}`]), mono(S, 1)],
  ['spread on and off (mono in)', sc(H(), [`run ${sec(0.2)}`, 'p spread 1', `run ${sec(0.4)}`, 'p spread 0', `run ${sec(0.5)}`]), mono(S, 1.1)],
  ['stereo input with spread', sc(H(48000, 128, 2), ['p spread 0.7', 'p tongueX 0.2', `run ${sec(0.8)}`]), inter(S, 0.8)],
  ['every knob moving', sc(H(), ['p tongueX 0.1', 'p tongueY 0.2', `run ${sec(0.1)}`, 'p tip 0.6', 'p jaw 0.9', `run ${sec(0.1)}`, 'p lips 0.2', 'p round 0.7',
    `run ${sec(0.1)}`, 'p nasal 0.5', 'p length 22', `run ${sec(0.1)}`, 'p damping 0.8', 'p spread 0.4', `run ${sec(0.1)}`, 'p radiation 0', 'p mix 0.7', 'p output 3', `run ${sec(0.3)}`]), mono(S, 0.8)],
  ['odd host block size (37)', sc(H(48000, 37), ['p tongueX 0.8', 'p spread 0.5', `run ${sec(0.6)}`]), mono(S, 0.6)],
  ['44.1 kHz', sc(H(44100, 512), ['p tongueX 0.1', 'p length 14', `run 30000`]), mono(44100, 30000 / 44100)],
  ['96 kHz', sc(H(96000, 64), ['p nasal 0.6', 'p length 30', `run 60000`]), mono(96000, 60000 / 96000, 220)],
];

let fails = 0;
for (const [name, script, input] of cases) {
  const a = runJs(script, input), b = runCpp(script, input);
  let maxd = 0, num = 0, den = 0;
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) { const d = a[i] - b[i]; maxd = Math.max(maxd, Math.abs(d)); num += d * d; den += a[i] * a[i]; }
  const resid = den > 0 ? 10 * Math.log10(num / den + 1e-30) : -999;
  const audible = den / n > 1e-10;
  const ok = audible && a.length === b.length && maxd < 1e-4 && resid < -90;
  if (!ok) fails++;
  console.log((ok ? 'PASS ' : 'FAIL ') + name.padEnd(36) + ' max diff ' + maxd.toExponential(1) + '  residual ' + resid.toFixed(0) + ' dB' + (a.length !== b.length ? '  LENGTH ' + a.length + ' vs ' + b.length : ''));
}
console.log(fails ? fails + ' FAILED' : 'C++ matches the reference');
process.exit(fails ? 1 : 0);
