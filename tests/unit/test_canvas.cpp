#include "core/canvas.h"

#include "check.h"

#include <algorithm>
#include <vector>

using sumi::Canvas;
using sumi::Rect;

namespace {

constexpr int32_t kW = 10, kH = 8, kStride = 12;   // stride > width: 2 padding bytes per row
constexpr uint8_t kPad = 0xAB;                     // padding sentinel; must never be touched

struct Fb {
    std::vector<uint8_t> buf;
    bool inverted;
    explicit Fb(bool inv, uint8_t logical_bg = 255) : buf(kStride * kH, kPad), inverted(inv)
    {
        uint8_t v = inv ? static_cast<uint8_t>(255 - logical_bg) : logical_bg;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) buf[static_cast<size_t>(y * kStride + x)] = v;
    }
    Canvas canvas() { return Canvas(buf.data(), kW, kH, kStride, inverted); }
    // Read back as logical gray.
    int at(int x, int y) const
    {
        uint8_t v = buf[static_cast<size_t>(y * kStride + x)];
        return inverted ? 255 - v : v;
    }
    bool padding_intact() const
    {
        for (int y = 0; y < kH; ++y)
            for (int x = kW; x < kStride; ++x)
                if (buf[static_cast<size_t>(y * kStride + x)] != kPad) return false;
        return true;
    }
    int count(int logical) const
    {
        int n = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) n += at(x, y) == logical;
        return n;
    }
};

void fill_basic(bool inv)
{
    Fb fb(inv);
    fb.canvas().fill_rect({2, 1, 3, 2}, 0);
    CHECK_EQ(fb.count(0), 6);
    CHECK_EQ(fb.at(2, 1), 0);
    CHECK_EQ(fb.at(4, 2), 0);
    CHECK_EQ(fb.at(5, 2), 255);
    CHECK_EQ(fb.at(2, 3), 255);
    CHECK(fb.padding_intact());
}
void test_fill_basic()    { fill_basic(false); }
void test_fill_inverted() { fill_basic(true); }

void test_fill_polarity_raw_value()
{
    Fb fb(true);
    fb.canvas().fill_rect({0, 0, 1, 1}, 0);   // logical black
    CHECK_EQ(fb.buf[0], 255);                 // inverted panel: black is 255
}

void test_fill_clips_edges()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        Canvas c = fb.canvas();
        c.fill_rect({-3, -3, 5, 5}, 0);        // top-left overflow -> 2x2
        c.fill_rect({kW - 2, kH - 1, 10, 10}, 0); // bottom-right overflow -> 2x1
        CHECK_EQ(fb.count(0), 4 + 2);
        CHECK_EQ(fb.at(1, 1), 0);
        CHECK_EQ(fb.at(kW - 1, kH - 1), 0);
        CHECK(fb.padding_intact());
    }
}

void test_fill_offscreen_and_empty()
{
    Fb fb(false);
    Canvas c = fb.canvas();
    c.fill_rect({kW, 0, 5, 5}, 0);
    c.fill_rect({0, kH, 5, 5}, 0);
    c.fill_rect({-10, -10, 5, 5}, 0);
    c.fill_rect({1, 1, 0, 5}, 0);
    c.fill_rect({1, 1, 5, -1}, 0);
    CHECK_EQ(fb.count(0), 0);
    CHECK(fb.padding_intact());
}

void test_fill_full_screen()
{
    Fb fb(true);
    fb.canvas().fill_rect({0, 0, kW, kH}, 17);
    CHECK_EQ(fb.count(17), kW * kH);
    CHECK(fb.padding_intact());
}

void test_stroke()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        fb.canvas().stroke_rect({1, 1, 6, 5}, 0, 1);
        CHECK_EQ(fb.count(0), 2 * 6 + 2 * 3);      // perimeter of 6x5
        CHECK_EQ(fb.at(1, 1), 0);
        CHECK_EQ(fb.at(6, 5), 0);
        CHECK_EQ(fb.at(3, 3), 255);                 // interior untouched
        CHECK_EQ(fb.at(0, 0), 255);                 // outside untouched
        CHECK(fb.padding_intact());
    }
}

void test_stroke_thick_fills()
{
    Fb fb(false);
    fb.canvas().stroke_rect({0, 0, 4, 4}, 0, 2);     // 2*2 >= 4 -> solid
    CHECK_EQ(fb.count(0), 16);
}

void test_stroke_clipped_and_degenerate()
{
    Fb fb(false);
    Canvas c = fb.canvas();
    c.stroke_rect({-2, -2, 6, 6}, 0, 1);   // only right/bottom edges of the box land on screen
    CHECK_EQ(fb.at(3, 0), 0);
    CHECK_EQ(fb.at(0, 3), 0);
    CHECK_EQ(fb.at(3, 3), 0);
    CHECK_EQ(fb.at(1, 1), 255);
    c.stroke_rect({5, 5, 3, 3}, 0, 0);     // zero thickness: no-op
    CHECK_EQ(fb.at(5, 5), 255);
    CHECK(fb.padding_intact());
}

void test_invert()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        Canvas c = fb.canvas();
        c.fill_rect({0, 0, 2, 1}, 40);
        c.invert_rect({0, 0, 3, 1});
        CHECK_EQ(fb.at(0, 0), 215);
        CHECK_EQ(fb.at(1, 0), 215);
        CHECK_EQ(fb.at(2, 0), 0);      // white -> black
        c.invert_rect({-5, -5, 100, 100});   // whole screen, clipped
        c.invert_rect({-5, -5, 100, 100});   // twice = identity
        CHECK_EQ(fb.at(0, 0), 215);
        CHECK_EQ(fb.at(5, 5), 255);
        CHECK(fb.padding_intact());
    }
}

void test_blit_and_clip()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        // 3x2 source with stride 4 (1 junk byte per row).
        const uint8_t src[] = {10, 20, 30, 99,
                               40, 50, 60, 99};
        Canvas c = fb.canvas();
        c.blit_gray8({1, 1, 3, 2}, src, 4);
        CHECK_EQ(fb.at(1, 1), 10);
        CHECK_EQ(fb.at(3, 1), 30);
        CHECK_EQ(fb.at(1, 2), 40);
        CHECK_EQ(fb.at(3, 2), 60);
        CHECK_EQ(fb.at(4, 1), 255);   // junk byte not copied

        // Clipped at top-left: only src(1..2, 1) lands at (0, 0)..(1, 0).
        Fb fb2(inv);
        fb2.canvas().blit_gray8({-1, -1, 3, 2}, src, 4);
        CHECK_EQ(fb2.at(0, 0), 50);
        CHECK_EQ(fb2.at(1, 0), 60);
        CHECK_EQ(fb2.at(2, 0), 255);
        CHECK_EQ(fb2.at(0, 1), 255);

        // Clipped at bottom-right.
        Fb fb3(inv);
        fb3.canvas().blit_gray8({kW - 2, kH - 1, 3, 2}, src, 4);
        CHECK_EQ(fb3.at(kW - 2, kH - 1), 10);
        CHECK_EQ(fb3.at(kW - 1, kH - 1), 20);
        CHECK(fb3.padding_intact());
    }
}

void test_quantize_bw()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        const uint8_t src[] = {0, 100, 127, 128, 200, 255};
        Canvas c = fb.canvas();
        c.blit_gray8({0, 0, 6, 1}, src, 6);
        c.quantize_rect({0, 0, 6, 1}, 2);
        CHECK_EQ(fb.at(0, 0), 0);
        CHECK_EQ(fb.at(1, 0), 0);
        CHECK_EQ(fb.at(2, 0), 0);
        CHECK_EQ(fb.at(3, 0), 255);
        CHECK_EQ(fb.at(4, 0), 255);
        CHECK_EQ(fb.at(5, 0), 255);
        CHECK(fb.padding_intact());
    }
}

void test_quantize_16_levels()
{
    for (bool inv : {false, true}) {
        Fb fb(inv);
        uint8_t src[256];
        Canvas c = fb.canvas();
        for (int v = 0; v < 256; v += kW) {
            for (int i = 0; i < kW; ++i) src[i] = static_cast<uint8_t>(std::min(255, v + i));
            c.blit_gray8({0, 0, kW, 1}, src, kW);
            c.quantize_rect({0, 0, kW, 1}, 16);
            for (int i = 0; i < kW; ++i) {
                int out = fb.at(i, 0);
                CHECK_EQ(out % 17, 0);                        // one of the 16 levels
                int in = std::min(255, v + i);
                CHECK(out - in <= 8 && in - out <= 8);         // nearest level
            }
        }
        // Exact levels survive unchanged; endpoints stay exact.
        const uint8_t exact[] = {0, 17, 136, 255};
        c.blit_gray8({0, 1, 4, 1}, exact, 4);
        c.quantize_rect({0, 1, 4, 1}, 16);
        CHECK_EQ(fb.at(0, 1), 0);
        CHECK_EQ(fb.at(1, 1), 17);
        CHECK_EQ(fb.at(2, 1), 136);
        CHECK_EQ(fb.at(3, 1), 255);
    }
}

void test_quantize_clipped_and_noop()
{
    Fb fb(false);
    Canvas c = fb.canvas();
    c.fill_rect({0, 0, kW, kH}, 100);
    c.quantize_rect({kW - 1, kH - 1, 5, 5}, 2);   // only the corner pixel
    CHECK_EQ(fb.at(kW - 1, kH - 1), 0);
    CHECK_EQ(fb.at(kW - 2, kH - 1), 100);
    c.quantize_rect({0, 0, kW, kH}, 256);          // 256 levels: no-op
    CHECK_EQ(fb.at(0, 0), 100);
    CHECK(fb.padding_intact());
}

} // namespace

int main()
{
    RUN(test_fill_basic);
    RUN(test_fill_inverted);
    RUN(test_fill_polarity_raw_value);
    RUN(test_fill_clips_edges);
    RUN(test_fill_offscreen_and_empty);
    RUN(test_fill_full_screen);
    RUN(test_stroke);
    RUN(test_stroke_thick_fills);
    RUN(test_stroke_clipped_and_degenerate);
    RUN(test_invert);
    RUN(test_blit_and_clip);
    RUN(test_quantize_bw);
    RUN(test_quantize_16_levels);
    RUN(test_quantize_clipped_and_noop);
    return check_result();
}
