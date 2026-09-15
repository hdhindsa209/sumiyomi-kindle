// Image decode (M4 S1): format sniffing, JPEG (gray output, DCT scaling), PNG (gray, RGBA over white,
// palette, 16-bit), and hostile input. Test images are encoded here with the same libraries, so no
// binary fixtures (or copyrighted pages) live in the repo.
#include "image/decode.h"

#include "check.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <jpeglib.h>
#include <png.h>

using namespace sumi::image;

namespace {

using Bytes = std::vector<uint8_t>;

// RGB (3 bytes per pixel) -> baseline JPEG at quality 95.
Bytes encode_jpeg(const Bytes& rgb, int w, int h, int components = 3)
{
    jpeg_compress_struct c{};
    jpeg_error_mgr e{};
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    unsigned char* buf = nullptr;
    unsigned long len = 0;
    jpeg_mem_dest(&c, &buf, &len);
    c.image_width = static_cast<JDIMENSION>(w);
    c.image_height = static_cast<JDIMENSION>(h);
    c.input_components = components;
    c.in_color_space = components == 3 ? JCS_RGB : JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb.data() + static_cast<size_t>(c.next_scanline) * static_cast<size_t>(w * components));
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    Bytes out(buf, buf + len);
    jpeg_destroy_compress(&c);
    std::free(buf);
    return out;
}

Bytes encode_png(const Bytes& pixels, int w, int h, png_uint_32 format, const Bytes& colormap = {}, int colormap_entries = 0)
{
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = static_cast<png_uint_32>(w);
    img.height = static_cast<png_uint_32>(h);
    img.format = format;
    img.colormap_entries = static_cast<png_uint_32>(colormap_entries);
    png_alloc_size_t size = 0;
    CHECK(png_image_write_get_memory_size(img, size, 0, pixels.data(), 0, colormap.empty() ? nullptr : colormap.data()));
    Bytes out(size);
    CHECK(png_image_write_to_memory(&img, out.data(), &size, 0, pixels.data(), 0, colormap.empty() ? nullptr : colormap.data()));
    out.resize(size);
    return out;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

void test_sniff()
{
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a'};
    const uint8_t html[] = {'<', 'h', 't', 'm', 'l', '>'};
    CHECK(sniff(jpeg, sizeof jpeg) == Format::Jpeg);
    CHECK(sniff(png, sizeof png) == Format::Png);
    CHECK(sniff(webp, sizeof webp) == Format::Webp);
    CHECK(sniff(gif, sizeof gif) == Format::Gif);
    CHECK(sniff(html, sizeof html) == Format::Unknown);   // e.g. a Cloudflare page instead of an image
    CHECK(sniff(jpeg, 2) == Format::Unknown);
    CHECK(sniff(nullptr, 0) == Format::Unknown);
}

void test_jpeg_color_to_luma()
{
    // Four solid 32x32 quadrants: black, white, red, green.
    const int w = 64, h = 64;
    Bytes rgb(static_cast<size_t>(w * h * 3));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &rgb[static_cast<size_t>((y * w + x) * 3)];
            int q = (y >= 32) * 2 + (x >= 32);
            const uint8_t colors[4][3] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}};
            std::memcpy(p, colors[q], 3);
        }
    Bytes jpg = encode_jpeg(rgb, w, h);
    Info info;
    std::string err;
    CHECK(probe(jpg.data(), jpg.size(), info, err));
    CHECK(info.format == Format::Jpeg && info.w == w && info.h == h);

    Gray g;
    CHECK(decode_gray(jpg.data(), jpg.size(), {}, g, err));
    CHECK_EQ(g.w, w);
    CHECK_EQ(g.h, h);
    CHECK(near(g.at(16, 16), 0, 3));
    CHECK(near(g.at(48, 16), 255, 3));
    CHECK(near(g.at(16, 48), 76, 6));     // JPEG luma (BT.601): 0.299 R
    CHECK(near(g.at(48, 48), 150, 6));    // 0.587 G
}

void test_jpeg_dct_scaling_covers_fit_size()
{
    // A typical source page (1600x2400) shown on the 1072x1448 panel: fit scale 0.603 -> N = 5.
    DecodeOptions opt;
    opt.fit_w = 1072;
    opt.fit_h = 1448;
    CHECK_EQ(jpeg_scale_num(1600, 2400, opt), 5);
    CHECK_EQ(jpeg_scale_num(1200, 1600, opt), 8);   // WeebCentral size: fit scale 0.893 -> full size
    CHECK_EQ(jpeg_scale_num(4000, 6000, opt), 2);
    CHECK_EQ(jpeg_scale_num(800, 1000, opt), 8);    // never upscales in the decoder
    CHECK_EQ(jpeg_scale_num(1600, 2400, {}), 8);    // no fit size: full

    const int w = 1600, h = 2400;
    Bytes gray(static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) gray[static_cast<size_t>(y * w + x)] = static_cast<uint8_t>((x / 100 + y / 100) % 2 ? 230 : 20);
    Bytes jpg = encode_jpeg(gray, w, h, 1);
    Gray g;
    std::string err;
    CHECK(decode_gray(jpg.data(), jpg.size(), opt, g, err));
    CHECK_EQ(g.w, 1000);
    CHECK_EQ(g.h, 1500);
    CHECK(g.w >= 965 && g.h >= 1448);        // still covers the fit-inside size (965x1448) for this page
    CHECK(near(g.at(30, 30), 20, 12));       // checker cell (0,0) is dark
    CHECK(near(g.at(90, 30), 230, 12));      // cell (1,0) is light
}

void test_png_variants()
{
    std::string err;
    Gray g;

    // 8-bit gray.
    Bytes gray = {0, 128, 255, 64};
    Bytes p = encode_png(gray, 2, 2, PNG_FORMAT_GRAY);
    CHECK(decode_gray(p.data(), p.size(), {}, g, err));
    CHECK(g.w == 2 && g.h == 2 && g.at(0, 0) == 0 && g.at(1, 0) == 128 && g.at(0, 1) == 255 && g.at(1, 1) == 64);

    // RGBA: fully transparent -> white page, opaque black stays black, half-transparent black -> mid gray.
    Bytes rgba = {0, 0, 0, 0,   0, 0, 0, 255,   0, 0, 0, 128,   255, 0, 0, 255};
    p = encode_png(rgba, 4, 1, PNG_FORMAT_RGBA);
    CHECK(decode_gray(p.data(), p.size(), {}, g, err));
    CHECK_EQ(g.at(0, 0), 255);
    CHECK_EQ(g.at(1, 0), 0);
    CHECK(g.at(2, 0) > 60 && g.at(2, 0) < 200);
    CHECK(g.at(3, 0) > 30 && g.at(3, 0) < 140);   // red has some luma

    // Palette (colormapped).
    Bytes cmap = {255, 255, 255,   0, 0, 0};
    Bytes idx = {0, 1, 1, 0};
    p = encode_png(idx, 2, 2, PNG_FORMAT_RGB_COLORMAP, cmap, 2);
    CHECK(decode_gray(p.data(), p.size(), {}, g, err));
    CHECK(g.at(0, 0) == 255 && g.at(1, 0) == 0 && g.at(0, 1) == 0 && g.at(1, 1) == 255);

    // 16-bit linear gray.
    std::vector<uint16_t> g16 = {0, 65535};
    Bytes raw(4);
    std::memcpy(raw.data(), g16.data(), 4);
    p = encode_png(raw, 2, 1, PNG_FORMAT_LINEAR_Y);
    CHECK(decode_gray(p.data(), p.size(), {}, g, err));
    CHECK(g.at(0, 0) == 0 && g.at(1, 0) == 255);
}

void test_hostile_input()
{
    std::string err;
    Gray g;
    Info info;

    // A Cloudflare challenge page where an image was expected.
    const std::string html = "<!DOCTYPE html><title>Attention Required! | Cloudflare</title>";
    CHECK(!decode_gray(reinterpret_cast<const uint8_t*>(html.data()), html.size(), {}, g, err));
    CHECK(err.find("unrecognized format") != std::string::npos);

    // Truncated JPEG header.
    Bytes rgb(16 * 16 * 3, 200);
    Bytes jpg = encode_jpeg(rgb, 16, 16);
    CHECK(!decode_gray(jpg.data(), 20, {}, g, err));
    CHECK(err.rfind("jpeg:", 0) == 0);
    CHECK(g.px.empty());

    // Truncated scan data: libjpeg fills the rest, we still get a full-size (partial) page rather than a crash.
    Bytes noisy(256 * 256 * 3);
    for (size_t i = 0; i < noisy.size(); ++i) noisy[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
    Bytes big_jpg = encode_jpeg(noisy, 256, 256);
    CHECK(decode_gray(big_jpg.data(), big_jpg.size() / 2, {}, g, err));
    CHECK(g.w == 256 && g.h == 256);

    // Corrupt PNG data after a valid signature.
    Bytes bad = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!decode_gray(bad.data(), bad.size(), {}, g, err));
    CHECK(err.rfind("png:", 0) == 0);

    // Decompression bomb: a tiny PNG declaring a huge canvas is refused before allocating.
    Bytes big(static_cast<size_t>(9000 * 9000), 255);
    Bytes bomb = encode_png(big, 9000, 9000, PNG_FORMAT_GRAY);
    DecodeOptions limit;
    limit.max_pixels = 50'000'000;
    CHECK(probe(bomb.data(), bomb.size(), info, err) && info.w == 9000);
    CHECK(!decode_gray(bomb.data(), bomb.size(), limit, g, err));
    CHECK(err.find("too large") != std::string::npos);

    // Formats we recognize but don't decode yet say so.
    const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    CHECK(!decode_gray(webp, sizeof webp, {}, g, err));
    CHECK(err == "WebP images are not supported yet");
}

} // namespace

int main()
{
    RUN(test_sniff);
    RUN(test_jpeg_color_to_luma);
    RUN(test_jpeg_dct_scaling_covers_fit_size);
    RUN(test_png_variants);
    RUN(test_hostile_input);
    return check_result();
}
