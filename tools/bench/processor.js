/* AudioWorklet wrapper: audio file → mouth-tube → output. Runs after mouth-tube.js. */
class MouthTubeProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.mouth = new MouthTube(sampleRate);
    this.bufN = 0;
    this.alloc(128);
    this.vizEvery = Math.round(sampleRate / 30);
    this.vizCount = 0;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'm') this.mouth.setParam(m.id, m.v);
      else if (m.t === 'snap') this.mouth.snap();
    };
    this.port.postMessage({ t: 'ready', sr: sampleRate, latency: this.mouth.latency, nose: Array.from(MouthTube.NOSE_D), velum: MouthTube.VELUM });
  }
  alloc(n) { this.bufN = n; this.zero = new Float32Array(n); }   // grows only if the host ever sends a bigger block
  process(inputs, outputs) {
    const out = outputs[0], oL = out[0], oR = out[1] || out[0], n = oL.length;
    if (n > this.bufN) this.alloc(n);
    const fin = inputs[0] || [];
    const inL = fin[0] || this.zero.subarray(0, n), inR = fin.length > 1 ? fin[1] : null;
    this.mouth.process(inL, inR, oL, oR, n);
    for (let c = 2; c < out.length; c++) out[c].set(oL);
    if ((this.vizCount += n) >= this.vizEvery) {
      this.vizCount = 0;
      const mo = this.mouth;
      this.port.postMessage({ t: 'viz', d: Array.from(mo.vizD), nasal: mo.vizNasal, len: mo.vizLen });
    }
    return true;
  }
}
registerProcessor('mouth-tube-processor', MouthTubeProcessor);
