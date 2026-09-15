#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sumi::image {

// Decoding page images to 8-bit grayscale (design doc §7.1 stages 1–2). The reader never needs
// color, so decoders produce luma directly: JPEG decodes its Y channel only, and at a reduced DCT
// scale when the page will be shown smaller anyway (the biggest single win in the pipeline).

enum class Format : uint8_t { Unknown, Jpeg, Png, Webp, Gif };

// From magic bytes only. Sources lie about extensions (WeebCentral serves JPEG as ".png").
Format sniff(const uint8_t* data, size_t size);
const char* format_name(Format f);

// 8-bit luma, row-major, no padding. 0 = black.
struct Gray {
    int32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    uint8_t at(int32_t x, int32_t y) const { return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]; }
};

struct DecodeOptions {
    // Size the page will be shown at (0 = unknown). JPEG picks the smallest DCT scale N/8 whose
    // output still covers the fit-inside size, so later resizing only ever shrinks.
    int32_t fit_w = 0, fit_h = 0;
    // Refuse images larger than this before allocating anything (decompression bombs).
    int64_t max_pixels = 60'000'000;
};

struct Info {
    Format  format = Format::Unknown;
    int32_t w = 0, h = 0;
};

// Header only: format and full-size dimensions.
bool probe(const uint8_t* data, size_t size, Info& out, std::string& err);

// Decode to gray. Transparent pixels are composited over white (the page background).
bool decode_gray(const uint8_t* data, size_t size, const DecodeOptions& opt, Gray& out, std::string& err);

// The DCT scale numerator (1..8, over 8) decode_gray uses for a w×h JPEG and these options.
int jpeg_scale_num(int32_t w, int32_t h, const DecodeOptions& opt);

} // namespace sumi::image
