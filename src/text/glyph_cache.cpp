#include "text/glyph_cache.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <cmath>

namespace sumi {
namespace {

struct GammaLut {
    uint8_t v[256];
    GammaLut()
    {
        for (int i = 0; i < 256; ++i)
            v[i] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 255.0, 1.0 / static_cast<double>(kTextGamma))));
    }
};

uint64_t make_key(FontId id, int32_t px, uint32_t glyph, bool fill)
{
    return (static_cast<uint64_t>(id) << 60) | (static_cast<uint64_t>(fill) << 59)
         | (static_cast<uint64_t>(px & 0x7FF) << 48) | glyph;
}

} // namespace

uint8_t text_gamma(uint8_t coverage)
{
    static const GammaLut lut;
    return lut.v[coverage];
}

const GlyphBitmap* GlyphCache::get(Fonts& fonts, FontId id, int32_t px, uint32_t glyph, bool fill)
{
    uint64_t key = make_key(id, px, glyph, id == FontId::Icons && fill);
    if (auto it = map_.find(key); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        ++hits_;
        return &it->second->bm;
    }
    ++misses_;

    Fonts::Sized s = fonts.sized(id, px);
    if (!s.face) return nullptr;
    if (id == FontId::Icons && FT_HAS_MULTIPLE_MASTERS(s.face)) {
        FT_Fixed coords[1] = {fill ? (1 << 16) : 0};
        if (FT_Set_Var_Design_Coordinates(s.face, 1, coords) != 0) return nullptr;
    }
    if (FT_Load_Glyph(s.face, glyph, FT_LOAD_TARGET_LIGHT) != 0) return nullptr;
    if (FT_Render_Glyph(s.face->glyph, FT_RENDER_MODE_NORMAL) != 0) return nullptr;

    const FT_GlyphSlot slot = s.face->glyph;
    const FT_Bitmap& src = slot->bitmap;
    Node node;
    node.key       = key;
    node.bm.left   = slot->bitmap_left;
    node.bm.top    = slot->bitmap_top;
    node.bm.width  = static_cast<int32_t>(src.width);
    node.bm.height = static_cast<int32_t>(src.rows);
    node.bm.alpha.resize(static_cast<size_t>(src.width) * src.rows);
    unsigned pitch = static_cast<unsigned>(src.pitch < 0 ? -src.pitch : src.pitch);
    for (unsigned y = 0; y < src.rows; ++y) {
        const uint8_t* row = src.pitch < 0 ? src.buffer + (src.rows - 1 - y) * pitch : src.buffer + y * pitch;
        for (unsigned x = 0; x < src.width; ++x)
            node.bm.alpha[static_cast<size_t>(y) * src.width + x] = text_gamma(row[x]);
    }

    bytes_ += cost(node.bm);
    lru_.push_front(std::move(node));
    map_[key] = lru_.begin();

    // Evict least recently used, but never the entry we're about to return.
    while (bytes_ > cap_ && lru_.size() > 1) {
        Node& victim = lru_.back();
        bytes_ -= cost(victim.bm);
        map_.erase(victim.key);
        lru_.pop_back();
    }
    return &lru_.front().bm;
}

} // namespace sumi
