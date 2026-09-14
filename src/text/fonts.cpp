#include "text/fonts.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include <cstdio>

namespace sumi {
namespace {

constexpr const char* kFiles[kFontCount] = {
    "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "MaterialSymbolsRounded.ttf",
};

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

} // namespace

Fonts::Fonts() = default;

Fonts::~Fonts()
{
    for (Entry& e : sized_) {
        if (e.s.hb) hb_font_destroy(e.s.hb);   // releases its referenced face
        else if (e.s.face) FT_Done_Face(e.s.face);
    }
    if (ft_) FT_Done_FreeType(ft_);
}

bool Fonts::open(const std::string& fonts_dir, int32_t dpi, std::string& err)
{
    dpi_ = dpi > 0 ? dpi : 160;
    if (!ft_ && FT_Init_FreeType(&ft_) != 0) {
        err = "FT_Init_FreeType failed";
        return false;
    }
    for (int i = 0; i < kFontCount; ++i) {
        std::string path = fonts_dir + "/" + kFiles[i];
        if (!read_file(path, data_[i])) {
            err = "cannot read font " + path;
            return false;
        }
    }
    return true;
}

Fonts::Sized Fonts::sized(FontId id, int32_t px)
{
    for (const Entry& e : sized_)
        if (e.id == id && e.px == px) return e.s;
    if (!ft_ || px <= 0 || px > 1024) return {};

    const std::vector<uint8_t>& bytes = data_[static_cast<size_t>(id)];
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(ft_, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face) != 0) return {};
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px)) != 0) {
        FT_Done_Face(face);
        return {};
    }
    Sized s;
    s.face = face;
    if (id != FontId::Icons) s.hb = hb_ft_font_create_referenced(face);
    if (s.hb) FT_Done_Face(face);   // hb holds its own reference now
    sized_.push_back({id, px, s});
    return s;
}

} // namespace sumi
