// Golden-image comparison for rendering tests (design doc §11.3).
// Images are 8-bit PGM in tests/golden/. Both sides are quantized to the panel's 16 levels
// before comparing, so sub-level antialiasing noise can't fail a test.
//   SUMI_UPDATE_GOLDENS=1 ctest ...   rewrite goldens from the current rendering
// On mismatch, the actual image is written next to the build as <name>.actual.pgm.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace golden {

inline uint8_t q16(uint8_t v) { return static_cast<uint8_t>(((v * 15 + 127) / 255) * 17); }

inline bool write_pgm(const std::string& path, const uint8_t* px, int w, int h)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%d %d\n255\n", w, h);
    bool ok = std::fwrite(px, 1, static_cast<size_t>(w) * static_cast<size_t>(h), f) == static_cast<size_t>(w) * static_cast<size_t>(h);
    std::fclose(f);
    return ok;
}

inline bool read_pgm(const std::string& path, std::vector<uint8_t>& px, int& w, int& h)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    int maxv = 0;
    bool ok = std::fscanf(f, "P5 %d %d %d", &w, &h, &maxv) == 3 && maxv == 255 && std::fgetc(f) != EOF;
    if (ok) {
        px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        ok = std::fread(px.data(), 1, px.size(), f) == px.size();
    }
    std::fclose(f);
    return ok;
}

// Matches if at most `max_bad_fraction` of pixels differ by more than one panel level.
inline bool match(const std::string& name, const uint8_t* px, int w, int h, std::string& why,
                  double max_bad_fraction = 0.0005)
{
    std::vector<uint8_t> q(px, px + static_cast<size_t>(w) * static_cast<size_t>(h));
    for (auto& v : q) v = q16(v);
    std::string path = std::string(SUMI_GOLDEN_DIR) + "/" + name + ".pgm";

    if (std::getenv("SUMI_UPDATE_GOLDENS")) {
        if (!write_pgm(path, q.data(), w, h)) { why = "cannot write " + path; return false; }
        std::fprintf(stderr, "golden updated: %s\n", path.c_str());
        return true;
    }
    std::vector<uint8_t> ref;
    int rw = 0, rh = 0;
    if (!read_pgm(path, ref, rw, rh)) { why = "missing golden " + path + " (run with SUMI_UPDATE_GOLDENS=1)"; return false; }
    if (rw != w || rh != h) { why = "size " + std::to_string(w) + "x" + std::to_string(h) + " vs golden " + std::to_string(rw) + "x" + std::to_string(rh); return false; }

    size_t bad = 0;
    for (size_t i = 0; i < q.size(); ++i) bad += (q[i] > ref[i] ? q[i] - ref[i] : ref[i] - q[i]) > 17;
    if (static_cast<double>(bad) <= max_bad_fraction * static_cast<double>(q.size())) return true;

    write_pgm(name + ".actual.pgm", q.data(), w, h);
    why = std::to_string(bad) + " pixels differ (wrote " + name + ".actual.pgm)";
    return false;
}

} // namespace golden
