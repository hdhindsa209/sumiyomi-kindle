#pragma once
#include <cstdint>
#include <vector>

#include "core/canvas.h"
#include "image/decode.h"

namespace sumi::image {

// Decoded page -> panel-ready page (design doc §7.1 stages 3–6): auto-crop, spread split,
// resize, tone curve, 16-level quantization. Output pixels are always one of the panel's 16
// grays (multiples of 17), so the waveform shows exactly what was computed.

enum class Fit : uint8_t {
    Screen,   // whole page visible (default); long strips still fill the width (see kStripAspect)
    Width,    // always fill the width; anything taller than the screen is cut into slices
};

enum class Dither : uint8_t {
    Sharp,      // nearest level only: crispest line art, visible banding in soft tones
    Balanced,   // error diffusion in tonal areas, none on edges/line art (default)
    Smooth,     // full Floyd–Steinberg everywhere: best gradients, grain around strokes
};

struct ProcessOptions {
    int32_t screen_w = 1072, screen_h = 1448;
    Fit     fit = Fit::Screen;
    Dither  dither = Dither::Balanced;
    bool    crop_borders = true;
    bool    split_spreads = true;   // aspect > kSpreadAspect -> two pages
    bool    rtl = true;             // reading order for split halves (right half first)
    // Tone: input levels mapped so `black_point` -> 0 and `white_point` -> 255, then gamma
    // (>1 darkens midtones: e-ink shows scans lighter than a backlit screen).
    uint8_t black_point = 12, white_point = 243;
    float   gamma = 1.25f;
};

static constexpr float kSpreadAspect = 1.2f;
// Height/width above which a page is a long strip (webtoon): shown at full width in slices,
// because fitting it whole would make it a sliver.
static constexpr float kStripAspect = 2.2f;
static constexpr int32_t kSliceOverlap = 64;   // px repeated between slices when no clean cut is found

// True if a w×h source page is shown width-filled and sliced under `opt`.
bool is_strip(int32_t w, int32_t h, const ProcessOptions& opt);
// Decode options for a w×h source page: the size it will actually be shown at.
DecodeOptions decode_options_for(int32_t w, int32_t h, const ProcessOptions& opt);
// Screen-height slices of a width-filled page, in reading order (top to bottom). Cuts at a blank
// row near each boundary when there is one; otherwise slices overlap by kSliceOverlap.
std::vector<Gray> slice_tall(const Gray& g, int32_t screen_h);

// Rect of `g` left after trimming near-uniform white or black borders. Never trims more than
// 20% of either dimension from one side, and returns the full image when unsure.
Rect content_bounds(const Gray& g);

// Crop -> split -> resize -> tone -> quantize -> slice. One output page, two for a split spread
// (in reading order), or several slices for a long strip. Never empty for a non-empty input.
std::vector<Gray> process_page(const Gray& decoded, const ProcessOptions& opt);

// Individual stages (exposed for tests and page_bench).
Gray crop(const Gray& g, const Rect& r);
// Target size for a w×h page under `opt` (fit mode, screen size), keeping aspect.
void fit_size(int32_t w, int32_t h, const ProcessOptions& opt, int32_t& out_w, int32_t& out_h);
Gray resize(const Gray& g, int32_t w, int32_t h);       // area average down, bilinear up
void apply_tone(Gray& g, const ProcessOptions& opt);
void quantize16(Gray& g, Dither dither);

} // namespace sumi::image
