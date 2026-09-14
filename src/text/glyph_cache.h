#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "text/fonts.h"

namespace sumi {

// Coverage gamma for e-ink (design doc §5.2): coverage' = 255 * (coverage/255)^(1/gamma).
// Thickens antialiased strokes so text doesn't look thin and washed out on the panel.
constexpr float kTextGamma = 1.8f;
uint8_t text_gamma(uint8_t coverage);

struct GlyphBitmap {
    int32_t left = 0, top = 0;           // bitmap origin relative to the pen / baseline
    int32_t width = 0, height = 0;
    std::vector<uint8_t> alpha;          // gamma-corrected coverage, width x height
};

// LRU cache of rendered glyphs keyed on (font, px, glyph index, fill), capped by bytes (§5.2: 4 MB).
class GlyphCache {
public:
    static constexpr size_t kDefaultCapBytes = 4 * 1024 * 1024;

    explicit GlyphCache(size_t cap_bytes = kDefaultCapBytes) : cap_(cap_bytes) {}

    // Renders on miss. `fill` selects the icon font's FILL axis (ignored for text faces).
    // Returns null if the glyph can't be rendered. The pointer is valid until the next get().
    const GlyphBitmap* get(Fonts& fonts, FontId id, int32_t px, uint32_t glyph, bool fill);

    size_t   bytes()   const { return bytes_; }
    size_t   entries() const { return map_.size(); }
    uint64_t hits()    const { return hits_; }
    uint64_t misses()  const { return misses_; }

private:
    struct Node { uint64_t key; GlyphBitmap bm; };
    static size_t cost(const GlyphBitmap& bm) { return bm.alpha.size() + sizeof(Node) + 32; }

    size_t cap_;
    size_t bytes_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    std::list<Node> lru_;   // front = most recently used
    std::unordered_map<uint64_t, std::list<Node>::iterator> map_;
};

} // namespace sumi
