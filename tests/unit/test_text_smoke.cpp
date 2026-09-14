// M2 S2: FreeType + HarfBuzz link and work, the bundled fonts load, and the icon font keeps
// its FILL axis. Built for host (ctest) and device (build/kindle/bin/text_smoke).
#include "check.h"

#include "icons.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include <hb-ft.h>
#include <hb.h>

#include <cstdlib>
#include <string>

namespace {

std::string assets()
{
    const char* env = std::getenv("SUMI_ASSETS");
    return env ? env : SUMI_ASSETS_DIR;
}

FT_Library g_ft = nullptr;

void test_shape_and_rasterize_inter()
{
    FT_Face face = nullptr;
    CHECK_EQ(FT_New_Face(g_ft, (assets() + "/fonts/Inter-Regular.ttf").c_str(), 0, &face), 0);
    if (!face) return;
    CHECK_EQ(FT_Set_Pixel_Sizes(face, 0, 30), 0);   // 16 sp at 300 dpi ≈ 30 px (design doc §5.3)

    hb_font_t* font = hb_ft_font_create_referenced(face);
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, "Library", -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font, buf, nullptr, 0);

    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    CHECK_EQ(n, 7);

    long advance = 0;
    long coverage = 0;
    for (unsigned i = 0; i < n; ++i) {
        CHECK(info[i].codepoint != 0);                          // real glyph, not .notdef
        advance += pos[i].x_advance;
        CHECK_EQ(FT_Load_Glyph(face, info[i].codepoint, FT_LOAD_TARGET_LIGHT), 0);
        CHECK_EQ(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL), 0);
        const FT_Bitmap& bm = face->glyph->bitmap;
        for (unsigned y = 0; y < bm.rows; ++y)
            for (unsigned x = 0; x < bm.width; ++x) coverage += bm.buffer[y * static_cast<unsigned>(bm.pitch) + x];
    }
    long advance_px = advance / 64;
    CHECK(advance_px > 60 && advance_px < 140);                 // ~7 glyphs at 30 px
    CHECK(coverage > 0);

    hb_buffer_destroy(buf);
    hb_font_destroy(font);   // also releases the referenced face
}

void test_icon_font_and_fill_axis()
{
    FT_Face face = nullptr;
    CHECK_EQ(FT_New_Face(g_ft, (assets() + "/fonts/MaterialSymbolsRounded.ttf").c_str(), 0, &face), 0);
    if (!face) return;
    CHECK(FT_HAS_MULTIPLE_MASTERS(face));
    CHECK(FT_Get_Char_Index(face, sumi::icon::collections_bookmark) != 0);
    CHECK(FT_Get_Char_Index(face, sumi::icon::search) != 0);
    CHECK_EQ(FT_Get_Char_Index(face, 0x41), 0);                 // 'A' was subset away

    FT_Set_Pixel_Sizes(face, 0, 45);
    auto ink = [&](FT_Fixed fill) {
        FT_Fixed coords[1] = {fill};
        CHECK_EQ(FT_Set_Var_Design_Coordinates(face, 1, coords), 0);
        CHECK_EQ(FT_Load_Char(face, sumi::icon::collections_bookmark, FT_LOAD_RENDER), 0);
        long sum = 0;
        const FT_Bitmap& bm = face->glyph->bitmap;
        for (unsigned y = 0; y < bm.rows; ++y)
            for (unsigned x = 0; x < bm.width; ++x) sum += bm.buffer[y * static_cast<unsigned>(bm.pitch) + x];
        return sum;
    };
    long outlined = ink(0), filled = ink(1 << 16);
    CHECK(outlined > 0);
    CHECK(filled > outlined * 5 / 4);                           // filled icon has visibly more ink
    FT_Done_Face(face);
}

} // namespace

int main()
{
    if (FT_Init_FreeType(&g_ft) != 0) {
        std::fprintf(stderr, "FT_Init_FreeType failed\n");
        return 1;
    }
    RUN(test_shape_and_rasterize_inter);
    RUN(test_icon_font_and_fill_axis);
    FT_Done_FreeType(g_ft);
    return check_result();
}
