// mouth-tube DSP core · bytebent
//
// C++ port of reference/mouth-tube.js, which defines the sound. tests/compare.js checks that
// both produce the same audio. No JUCE dependency.
//
// A vocal tract as an audio effect: a Kelly-Lochbaum waveguide (oral tube plus a nasal branch at
// the velum) shaped by the controls, running at its own rate so Length scales the formants, with
// windowed-sinc resampling in and out. Dual mono; Spread makes the two tubes differ.
//
// Real-time rules: everything lives in fixed-size members, set up in prepare(); process() never
// allocates, locks or does I/O. Latency is fixed per sample rate (latency()).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wfloat-equal"     // exact compares are intended (NaN check, 0/1 switch)
#endif

namespace bytebent { namespace mouthtube {

constexpr int N_ORAL = 40;
constexpr int N_NOSE = 25;
constexpr int VELUM = 16;
constexpr double SOUND = 350;
constexpr double MIN_AREA = 1e-6;
constexpr double GLOT_REFL = 0.85;
constexpr double LIP_REFL = -0.97;
constexpr double LIP_RAD_HZ = 4000;
constexpr double NOSE_REFL = -0.95, NOSE_RAD_HZ = 3000;
constexpr int BLOCK = 32;
constexpr double ROUND_LEN = 0.10;
constexpr double SPREAD_LEN = 0.06;
constexpr double SPREAD_TONGUE = 0.08;
constexpr double LEN_MIN = 7, LEN_MAX = 35;
constexpr double RAD_REF_HZ = 1000;
constexpr double GAIN_RAD = 7.1;
constexpr double GAIN_FLOW = 1.58;
constexpr int HW = 8;
constexpr int KRES = 512;
constexpr double CUT = 0.45;
constexpr double PI = 3.141592653589793;
constexpr double SQRT2 = 1.4142135623730951;
constexpr int HM = 511, TM = 1023;

enum Param {
    pTongueX, pTongueY, pTip, pJaw, pLips, pRound, pNasal, pLength, pDamping, pSpread, pRadiation, pMix, pOutput,
    kNumParams
};

struct ParamInfo { const char* id; const char* name; double def, min, max; };

// ids and ranges match reference/mouth-tube.js PARAMS exactly
inline const ParamInfo& paramInfo(int i) {
    static const ParamInfo t[kNumParams] = {
        { "tongueX",   "Tongue Front-Back", 0.55, 0,       1 },        // 0 front … 1 back
        { "tongueY",   "Tongue Close-Open", 0.7,  0,       1 },        // 0 close … 1 open
        { "tip",       "Tongue Tip",        0,    0,       1 },
        { "jaw",       "Jaw Open",          0.3,  0,       1 },
        { "lips",      "Lips Opening",      0.6,  0,       1 },
        { "round",     "Lips Round",        0,    0,       1 },
        { "nasal",     "Nasal",             0,    0,       1 },        // velum opening
        { "length",    "Length",            17,   LEN_MIN, LEN_MAX },  // cm
        { "damping",   "Damping",           0.5,  0,       1 },
        { "spread",    "Spread",            0,    0,       1 },
        { "radiation", "Radiation",         1,    0,       1 },        // switch
        { "mix",       "Mix",               1,    0,       1 },
        { "output",    "Output",            0,   -24,      12 },       // dB
    };
    return t[i];
}

inline int paramIndex(const char* id) {
    for (int i = 0; i < kNumParams; ++i) if (std::strcmp(paramInfo(i).id, id) == 0) return i;
    return -1;
}

/* ---------------- shared tables ---------------- */

struct Tables {
    std::array<double, HW * KRES + 2> KT {};
    std::array<double, N_NOSE> noseD {};
    static constexpr int NC = 10;
    std::array<double, NC> CX { { 0.08, 0.16, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85, 0.92 } };
    std::array<std::array<double, NC>, 4> corners { {
        { { 1.20, 1.74, 2.23, 2.77, 2.44, 1.67, 1.04, 0.74, 0.87, 1.34 } },   // front close
        { { 1.20, 1.52, 1.78, 2.50, 1.48, 0.74, 1.16, 1.84, 2.19, 1.34 } },   // back close
        { { 1.20, 1.23, 1.60, 1.88, 2.09, 2.28, 2.32, 2.12, 2.37, 3.53 } },   // front open
        { { 1.20, 0.63, 0.68, 0.76, 1.01, 1.55, 2.36, 2.82, 2.81, 2.49 } },   // back open
    } };
    std::array<std::array<double, NC>, 4> cornerLog {};

    Tables() {
        for (int k = 0; k <= HW * KRES; ++k) {
            const double x = (double) k / KRES;
            const double s = x == 0 ? 1 : std::sin(PI * x) / (PI * x);
            const double w = 0.42 + 0.5 * std::cos(PI * x / HW) + 0.08 * std::cos(2 * PI * x / HW);
            KT[(size_t) k] = s * w;
        }
        KT[HW * KRES] = 0; KT[HW * KRES + 1] = 0;
        for (int k = 0; k < N_NOSE; ++k) {
            const double y = (k + 0.5) / N_NOSE;
            noseD[(size_t) k] = y < 0.3 ? 0.6 + 1.2 * y / 0.3 : y < 0.8 ? 1.8 : 1.8 - 0.9 * (y - 0.8) / 0.2;
        }
        for (size_t c = 0; c < 4; ++c) for (size_t i = 0; i < NC; ++i) cornerLog[c][i] = std::log(corners[c][i]);
    }
    static const Tables& get() { static const Tables t; return t; }
};

inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double bilin(double fc, double bc, double fo, double bo, double X, double Y) { return lerp(lerp(fc, bc, X), lerp(fo, bo, X), Y); }

struct Shape { double tongueX, tongueY, tip, jaw, lips, round, nasal, spread; };

// Fills d (N_ORAL diameters, area = d²) and returns the nasal coupling area. side: -1 / +1 / 0.
inline double shape(const Shape& s, double* d, int side) {
    const Tables& T = Tables::get();
    const double X = clamp01(s.tongueX + side * s.spread * SPREAD_TONGUE), Y = clamp01(s.tongueY);
    const double m = std::min(1.0, std::sqrt((2 * X - 1) * (2 * X - 1) + (2 * Y - 1) * (2 * Y - 1)) / SQRT2);
    const double lr = std::log(2.0);
    double ctl[Tables::NC];
    for (size_t i = 0; i < Tables::NC; ++i)
        ctl[i] = std::exp(lr + m * (bilin(T.cornerLog[0][i], T.cornerLog[1][i], T.cornerLog[2][i], T.cornerLog[3][i], X, Y) - lr));
    ctl[0] = T.corners[0][0];
    const int NC = Tables::NC;
    for (int i = 0; i < N_ORAL; ++i) {
        const double x = (i + 0.5) / N_ORAL;
        double v;
        if (x <= T.CX[0]) v = ctl[0];
        else if (x >= T.CX[NC - 1]) v = ctl[NC - 1];
        else {
            int k = 1; while (T.CX[(size_t) k] < x) ++k;
            const double t = (x - T.CX[(size_t) k - 1]) / (T.CX[(size_t) k] - T.CX[(size_t) k - 1]);
            v = lerp(ctl[k - 1], ctl[k], 0.5 - 0.5 * std::cos(PI * t));
        }
        if (x > 0.55) v *= 1 + 0.8 * (s.jaw - 0.3) * clamp01((x - 0.55) / 0.2);
        const double dt = x - 0.86;
        if (dt > -0.045 && dt < 0.045) v *= std::max(0.0, 1 - s.tip * 1.05 * 0.5 * (1 + std::cos(PI * dt / 0.045)));
        if (x > 0.89 && x < 0.95) v *= 1 - 0.25 * s.round;
        d[i] = v > 0 ? v : 0;
    }
    const double lip = 2.6 * std::pow(s.lips, 1.2) * (1 - 0.7 * s.round);
    d[N_ORAL - 1] = lip;
    d[N_ORAL - 2] = lerp(d[N_ORAL - 2], lip, 0.6);
    d[VELUM - 1] *= 1 - 0.2 * s.nasal;
    d[VELUM] *= 1 - 0.2 * s.nasal;
    const double vn = 1.0 * s.nasal;
    return vn * vn;
}

/* ---------------- one tube (waveguide) ---------------- */

struct Tube {
    double R[N_ORAL] {}, L[N_ORAL] {}, jR[N_ORAL] {}, jL[N_ORAL] {};
    double NR[N_NOSE] {}, NL[N_NOSE] {}, nR[N_NOSE] {}, nL[N_NOSE] {};
    double c[N_ORAL] {}, nc[N_NOSE] {};
    double q0 = 0, q1 = 0, q2 = 0;
    double g = 0.999, gn = 0.998;
    double lp = 0.5, np = 0.5;
    double lz = 0, nz = 0;
    double d[N_ORAL] {}, A[N_ORAL] {}, nA[N_NOSE] {};

    void reset() {
        std::fill(R, R + N_ORAL, 0.0); std::fill(L, L + N_ORAL, 0.0);
        std::fill(NR, NR + N_NOSE, 0.0); std::fill(NL, NL + N_NOSE, 0.0);
        lz = 0; nz = 0;
    }
    void copyFrom(const Tube& t) {
        std::copy(t.R, t.R + N_ORAL, R); std::copy(t.L, t.L + N_ORAL, L);
        std::copy(t.NR, t.NR + N_NOSE, NR); std::copy(t.NL, t.NL + N_NOSE, NL);
        lz = t.lz; nz = t.nz;
    }
    void setShape(double nasalArea) {
        const Tables& T = Tables::get();
        for (int i = 0; i < N_ORAL; ++i) { const double a = d[i] * d[i]; A[i] = a > MIN_AREA ? a : MIN_AREA; }
        for (int j = 1; j < N_ORAL; ++j) c[j] = 2 * A[j] / (A[j - 1] + A[j]);
        nA[0] = nasalArea > MIN_AREA ? nasalArea : MIN_AREA;
        for (int k = 1; k < N_NOSE; ++k) nA[k] = T.noseD[(size_t) k] * T.noseD[(size_t) k];
        for (int k = 1; k < N_NOSE; ++k) nc[k] = 2 * nA[k] / (nA[k - 1] + nA[k]);
        const double sum = A[VELUM - 1] + A[VELUM] + nA[0];
        q0 = 2 * A[VELUM - 1] / sum; q1 = 2 * A[VELUM] / sum; q2 = 2 * nA[0] / sum;
    }
    double step(double x) {
        jR[0] = GLOT_REFL * L[0] + x;
        lz += (R[N_ORAL - 1] - lz) * lp;
        jL[N_ORAL - 1] = LIP_REFL * lz;
        for (int j = 1; j < VELUM; ++j) {
            const double s = R[j - 1] + L[j], cj = c[j];
            jR[j] = cj * s - L[j];
            jL[j - 1] = (2 - cj) * s - R[j - 1];
        }
        {
            const double s = R[VELUM - 1] + L[VELUM] + NL[0];
            jR[VELUM] = q1 * s - L[VELUM];
            jL[VELUM - 1] = q0 * s - R[VELUM - 1];
            nR[0] = q2 * s - NL[0];
        }
        for (int j = VELUM + 1; j < N_ORAL; ++j) {
            const double s = R[j - 1] + L[j], cj = c[j];
            jR[j] = cj * s - L[j];
            jL[j - 1] = (2 - cj) * s - R[j - 1];
        }
        nz += (NR[N_NOSE - 1] - nz) * np;
        nL[N_NOSE - 1] = NOSE_REFL * nz;
        for (int k = 1; k < N_NOSE; ++k) {
            const double s = NR[k - 1] + NL[k], ck = nc[k];
            nR[k] = ck * s - NL[k];
            nL[k - 1] = (2 - ck) * s - NR[k - 1];
        }
        for (int i = 0; i < N_ORAL; ++i) { R[i] = jR[i] * g; L[i] = jL[i] * g; }
        for (int k = 0; k < N_NOSE; ++k) { NR[k] = nR[k] * gn; NL[k] = nL[k] * gn; }
        return R[N_ORAL - 1] + NR[N_NOSE - 1];
    }
};

/* ---------------- one channel: resample in → tube → resample out ---------------- */

struct Channel {
    Tube tube;
    double hist[HM + 1] {};
    double ty[TM + 1] {}, tt[TM + 1] {}, tw[TM + 1] {};
    long long tc = 0;
    double T = 0, dT = 1, nuIn = CUT, nuOut = CUT;

    void reset(long long n) {
        tube.reset();
        std::fill(hist, hist + HM + 1, 0.0); std::fill(ty, ty + TM + 1, 0.0);
        std::fill(tt, tt + TM + 1, -1e9); std::fill(tw, tw + TM + 1, 0.0);
        tc = 0; T = (double) n;
    }
    void copyFrom(const Channel& o) {
        tube.copyFrom(o.tube);
        std::copy(o.hist, o.hist + HM + 1, hist);
        std::copy(o.ty, o.ty + TM + 1, ty); std::copy(o.tt, o.tt + TM + 1, tt); std::copy(o.tw, o.tw + TM + 1, tw);
        tc = o.tc; T = o.T;
    }
    void setRate(double rate, double sr) {
        dT = sr / rate;
        const double lo = std::min(1.0, rate / sr);
        nuIn = CUT * lo;
        nuOut = CUT * lo;
    }
    double sample(double x, long long n, int dIn, int dOut) {
        const double* KT = Tables::get().KT.data();
        hist[n & HM] = x;
        const double lim = (double) (n - dIn);
        if (T <= lim) {
            const double nu = nuIn, f2 = 2 * nu, span = HW / f2, kr = f2 * KRES;
            do {
                const double Tc = T;
                const long long m0 = (long long) std::ceil(Tc - span), m1 = (long long) std::floor(Tc + span);
                double acc = 0;
                for (long long m = m0; m <= m1; ++m) {
                    double k = ((double) m - Tc) * kr; if (k < 0) k = -k;
                    const int i = (int) k; const double fr = k - i;
                    acc += hist[m & HM] * (KT[i] + (KT[i + 1] - KT[i]) * fr);
                }
                const double y = tube.step(acc * f2);
                const long long j = tc & TM;
                ty[j] = y; tt[j] = Tc; tw[j] = dT;
                ++tc;
                T = Tc + dT;
            } while (T <= lim);
        }
        const double to = (double) (n - dIn - dOut);
        const double nu = nuOut, f2 = 2 * nu, span = HW / f2, kr = f2 * KRES;
        double acc = 0;
        const long long stop = tc - TM;
        for (long long k = tc - 1; k >= 0 && k > stop; --k) {
            const long long j = k & TM;
            const double dt = to - tt[j];
            if (dt < -span) continue;
            if (dt > span) break;
            double q = dt * kr; if (q < 0) q = -q;
            const int i = (int) q; const double fr = q - i;
            acc += ty[j] * tw[j] * (KT[i] + (KT[i + 1] - KT[i]) * fr);
        }
        return acc * f2;
    }
};

/* ---------------- engine ---------------- */

class Engine {
public:
    void prepare(double sampleRate) {
        Tables::get();
        sr = sampleRate;
        for (int i = 0; i < kNumParams; ++i) { p[i] = paramInfo(i).def; s[i] = paramInfo(i).def; }
        smooth = 1 - std::exp(-BLOCK / (0.025 * sampleRate));
        const double maxLen = LEN_MAX / 100 * (1 + ROUND_LEN) * (1 + SPREAD_LEN);
        const double minRate = SOUND * N_ORAL / maxLen;
        const double nuMin = CUT * std::min(1.0, minRate / sampleRate);
        dIn = (int) std::ceil(HW / (2 * nuMin)) + 1;
        dOut = (int) std::ceil(HW / (2 * nuMin)) + 1;
        n = 0; blk = 0; stereo = false; wantStereo = false; xf = 0;
        std::fill(dry[0], dry[0] + 256, 0.f); std::fill(dry[1], dry[1] + 256, 0.f);
        radK = 1 / (2 * std::sin(PI * RAD_REF_HZ / sampleRate));
        prev[0] = prev[1] = 0;
        gOut = 1; wet = 1; rad = 1;
        updateBlock(false);
        for (auto& c : ch) c.reset(0);
    }

    int latency() const { return dIn + dOut; }

    void setParam(int i, double v) {
        if (i < 0 || i >= kNumParams) return;
        const ParamInfo& d = paramInfo(i);
        if (!(v == v)) return;
        if (v < d.min) v = d.min; else if (v > d.max) v = d.max;
        if (i == pRadiation) v = v >= 0.5 ? 1 : 0;
        p[i] = v;
    }

    void snap() { updateBlock(false); }

    void reset() {
        for (auto& c : ch) c.reset(n);
        prev[0] = prev[1] = 0;
        std::fill(dry[0], dry[0] + 256, 0.f); std::fill(dry[1], dry[1] + 256, 0.f);
    }

    // inR == nullptr: mono input. In-place use (in == out) is fine.
    void process(const float* inL, const float* inR, float* outL, float* outR, int count) {
        const int lat = latency();
        for (int j = 0; j < count; ++j) {
            if (blk == 0) {
                updateBlock(true);
                const bool want = inR != nullptr || s[pSpread] > 1e-4;
                if (want && !stereo) { ch[1].copyFrom(ch[0]); stereo = true; }
                wantStereo = want;
            }
            if (++blk == BLOCK) blk = 0;
            const long long n0 = n++;
            const float xl = inL[j], xr = inR != nullptr ? inR[j] : xl;
            const double yl = ch[0].sample(xl, n0, dIn, dOut);
            double yr = yl;
            if (stereo) {
                const double y1 = ch[1].sample(xr, n0, dIn, dOut);
                xf += ((wantStereo ? 1 : 0) - xf) * 0.004;
                if (!wantStereo && xf < 1e-3) { xf = 0; stereo = false; }
                yr = yl + (y1 - yl) * xf;
            }
            const double gr = (yl - prev[0]) * radK, gr2 = (yr - prev[1]) * radK;
            prev[0] = yl; prev[1] = yr;
            const double wl = (gr * GAIN_RAD - yl * GAIN_FLOW) * rad + yl * GAIN_FLOW;
            const double wr = (gr2 * GAIN_RAD - yr * GAIN_FLOW) * rad + yr * GAIN_FLOW;
            const long long di = n0 & 255, dd = (n0 - lat) & 255;
            dry[0][di] = xl; dry[1][di] = xr;
            double ol = (dry[0][dd] + (wl - dry[0][dd]) * wet) * gOut;
            double orr = (dry[1][dd] + (wr - dry[1][dd]) * wet) * gOut;
            if (!(ol == ol) || !(orr == orr) || std::abs(ol) > 1e6) { reset(); ol = 0; orr = 0; }
            outL[j] = (float) (ol > 1 ? 1 : ol < -1 ? -1 : ol);
            outR[j] = (float) (orr > 1 ? 1 : orr < -1 ? -1 : orr);
        }
    }

    // live view: centre tube shape, nasal coupling area, effective length (m); updated every block
    float vizD[N_ORAL] {};
    double vizNasal = 0, vizLen = 0.17;

private:
    void updateBlock(bool smoothIt) {
        const double c = smoothIt ? smooth : 1;
        for (int k = 0; k < kNumParams; ++k) {
            s[k] += (p[k] - s[k]) * c;
            if (std::abs(p[k] - s[k]) < 1e-6) s[k] = p[k];
        }
        const double B = 12 * std::pow(25.0, s[pDamping]);
        const Shape sh { s[pTongueX], s[pTongueY], s[pTip], s[pJaw], s[pLips], s[pRound], s[pNasal], s[pSpread] };
        for (int side = 0; side < 2; ++side) {
            Channel& chn = ch[side];
            const int sg = side ? 1 : -1;
            const double len = s[pLength] / 100 * (1 + ROUND_LEN * s[pRound]) * (1 + sg * SPREAD_LEN * s[pSpread]);
            const double rate = SOUND * N_ORAL / len;
            chn.setRate(rate, sr);
            Tube& t = chn.tube;
            const double na = shape(sh, t.d, sg);
            t.setShape(na);
            t.g = std::exp(-PI * B / rate);
            t.gn = std::exp(-PI * (2 * B + 350) / rate);
            const double size = 0.17 / len;
            const double lipHz = std::min(15000.0, std::max(800.0, LIP_RAD_HZ * size * 2 / std::max(0.3, t.d[N_ORAL - 1])));
            t.lp = 1 - std::exp(-2 * PI * lipHz / rate);
            t.np = 1 - std::exp(-2 * PI * NOSE_RAD_HZ * size / rate);
        }
        vizNasal = shape(sh, tmpD, 0);
        for (int i = 0; i < N_ORAL; ++i) vizD[i] = (float) tmpD[i];
        vizLen = s[pLength] / 100 * (1 + ROUND_LEN * s[pRound]);
        gOut = std::pow(10.0, s[pOutput] / 20);
        wet = s[pMix];
        rad = s[pRadiation];
    }

    double sr = 48000, smooth = 0;
    double p[kNumParams] {}, s[kNumParams] {};
    int dIn = 0, dOut = 0;
    Channel ch[2];
    long long n = 0;
    int blk = 0;
    bool stereo = false, wantStereo = false;
    double xf = 0;
    float dry[2][256] {};
    double radK = 1, prev[2] {};
    double gOut = 1, wet = 1, rad = 1;
    double tmpD[N_ORAL] {};
};

}} // namespace bytebent::mouthtube

#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif
