// Runs source/MouthTubeDSP.h on a test script and an input file, and writes raw float32 samples
// (stereo, interleaved) to stdout.
// Script lines: "sr <rate> block <n> ch <1|2>" first, then "p <id> <value>", "snap", "run <samples>".
// The input file holds float32 samples, interleaved when ch is 2. See tests/compare.js.
#include "../source/MouthTubeDSP.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace bytebent::mouthtube;

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: dsp_cli script.txt input.f32 > out.f32\n"; return 2; }
    std::ifstream in(argv[1]);
    std::ifstream audio(argv[2], std::ios::binary | std::ios::ate);
    const auto bytes = (size_t) audio.tellg();
    audio.seekg(0);
    std::vector<float> input(bytes / sizeof(float));
    audio.read(reinterpret_cast<char*>(input.data()), (std::streamsize) bytes);
    std::string line, w;
    double sr = 48000; int block = 128, chans = 1;
    std::getline(in, line);
    { std::istringstream h(line); while (h >> w) { if (w == "sr") h >> sr; else if (w == "block") h >> block; else if (w == "ch") h >> chans; } }
    static Engine e;
    e.prepare(sr);
    std::vector<float> l((size_t) block), r((size_t) block), ol((size_t) block), orr((size_t) block), outI((size_t) block * 2);
    size_t pos = 0;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        if (!(s >> w)) continue;
        if (w == "p") { std::string id; double v; s >> id >> v; e.setParam(paramIndex(id.c_str()), v); }
        else if (w == "snap") e.snap();
        else if (w == "run") {
            long long left; s >> left;
            while (left > 0) {
                const int m = (int) std::min<long long>(block, left);
                for (int i = 0; i < m; ++i, ++pos) {
                    l[(size_t) i] = input[pos * (size_t) chans];
                    r[(size_t) i] = chans > 1 ? input[pos * 2 + 1] : 0.f;
                }
                e.process(l.data(), chans > 1 ? r.data() : nullptr, ol.data(), orr.data(), m);
                for (int i = 0; i < m; ++i) { outI[(size_t) i * 2] = ol[(size_t) i]; outI[(size_t) i * 2 + 1] = orr[(size_t) i]; }
                std::fwrite(outI.data(), sizeof(float), (size_t) m * 2, stdout);
                left -= m;
            }
        }
    }
    return 0;
}
