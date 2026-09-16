#include "image/decode.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>

// libjpeg's headers need FILE and size_t declared first.
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

namespace sumi::image {
namespace {

// ---------------------------------------------------------------- JPEG

// libjpeg reports fatal errors through error_exit, which must not return. We longjmp back to the
// setjmp in the decode function; that function keeps only trivially destructible locals so the
// jump skips nothing that needs cleanup (the output vector belongs to the caller).
struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf        jump;
    char           message[JMSG_LENGTH_MAX];
};

void jpeg_fail(j_common_ptr cinfo)
{
    auto* e = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->message);
    std::longjmp(e->jump, 1);
}

void jpeg_quiet(j_common_ptr, int) {}   // corrupt-data warnings: keep decoding what we can

bool jpeg_header(jpeg_decompress_struct& cinfo, JpegError& jerr, const uint8_t* data, size_t size)
{
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_fail;
    jerr.mgr.emit_message = jpeg_quiet;
    jerr.message[0] = '\0';
    if (setjmp(jerr.jump)) return false;
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    jpeg_read_header(&cinfo, TRUE);
    return true;
}

bool decode_jpeg(const uint8_t* data, size_t size, const DecodeOptions& opt, Gray& out, std::string& err)
{
    jpeg_decompress_struct cinfo{};
    JpegError jerr{};
    if (!jpeg_header(cinfo, jerr, data, size)) {
        err = std::string("jpeg: ") + jerr.message;
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    if (static_cast<int64_t>(cinfo.image_width) * cinfo.image_height > opt.max_pixels) {
        err = "jpeg: image too large";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    if (cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK) {
        err = "jpeg: CMYK images are not supported";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    if (setjmp(jerr.jump)) {
        err = std::string("jpeg: ") + jerr.message;
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    // Y channel only: libjpeg skips chroma upsampling and color conversion entirely.
    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.scale_num = static_cast<unsigned int>(jpeg_scale_num(static_cast<int32_t>(cinfo.image_width),
                                                               static_cast<int32_t>(cinfo.image_height), opt));
    cinfo.scale_denom = 8;
    cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&cinfo);

    out.w = static_cast<int32_t>(cinfo.output_width);
    out.h = static_cast<int32_t>(cinfo.output_height);
    out.px.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h), 255);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = out.px.data() + static_cast<size_t>(cinfo.output_scanline) * static_cast<size_t>(out.w);
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

// ---------------------------------------------------------------- PNG

bool decode_png(const uint8_t* data, size_t size, const DecodeOptions& opt, Gray& out, std::string& err)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size)) {
        err = std::string("png: ") + image.message;
        png_image_free(&image);
        return false;
    }
    if (static_cast<int64_t>(image.width) * image.height > opt.max_pixels) {
        err = "png: image too large";
        png_image_free(&image);
        return false;
    }
    // Gray without alpha + a background: libpng composites transparency over it for us.
    image.format = PNG_FORMAT_GRAY;
    out.w = static_cast<int32_t>(image.width);
    out.h = static_cast<int32_t>(image.height);
    out.px.assign(PNG_IMAGE_SIZE(image), 255);
    png_color background{255, 255, 255};
    if (!png_image_finish_read(&image, &background, out.px.data(), 0, nullptr)) {
        err = std::string("png: ") + image.message;
        png_image_free(&image);
        out = {};
        return false;
    }
    return true;
}

bool png_dims(const uint8_t* data, size_t size, Info& info, std::string& err)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    bool ok = png_image_begin_read_from_memory(&image, data, size) != 0;
    if (ok) {
        info.w = static_cast<int32_t>(image.width);
        info.h = static_cast<int32_t>(image.height);
    } else {
        err = std::string("png: ") + image.message;
    }
    png_image_free(&image);
    return ok;
}

// WebP has no equivalent of JPEG's DCT scaling, but its decoder can rescale while it decodes, which
// is nearly as good: the RGBA buffer we allocate is the scaled size, not the full page.
void webp_fit(int32_t w, int32_t h, const DecodeOptions& opt, int& out_w, int& out_h)
{
    out_w = out_h = 0;
    if (w <= 0 || h <= 0 || (opt.fit_w <= 0 && opt.fit_h <= 0)) return;
    double s = 1.0;
    if (opt.fit_w > 0) s = static_cast<double>(opt.fit_w) / w;
    if (opt.fit_h > 0) s = std::min(opt.fit_w > 0 ? s : 1e9, static_cast<double>(opt.fit_h) / h);
    if (s >= 1.0) return;   // never scale up: process.cpp does the final fit
    out_w = std::max(1, static_cast<int>(std::lround(w * s)));
    out_h = std::max(1, static_cast<int>(std::lround(h * s)));
}

bool decode_webp(const uint8_t* data, size_t size, const DecodeOptions& opt, Gray& out, std::string& err)
{
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        err = "webp: decoder version mismatch";
        return false;
    }
    if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) {
        err = "webp: bad header";
        return false;
    }
    int32_t w = config.input.width, h = config.input.height;
    if (static_cast<int64_t>(w) * h > opt.max_pixels) {
        err = "webp: image too large";
        return false;
    }
    int sw = 0, sh = 0;
    webp_fit(w, h, opt, sw, sh);
    if (sw > 0) {
        config.options.use_scaling  = 1;
        config.options.scaled_width = sw;
        config.options.scaled_height = sh;
    }
    // RGBA even for opaque images: the decoder has no luma-only output, and alpha has to be
    // composited over white here anyway.
    config.output.colorspace = MODE_RGBA;
    if (WebPDecode(data, size, &config) != VP8_STATUS_OK) {
        err = "webp: decode failed";
        WebPFreeDecBuffer(&config.output);
        return false;
    }
    const WebPRGBABuffer& rgba = config.output.u.RGBA;
    out.w = config.output.width;
    out.h = config.output.height;
    out.px.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h));
    for (int32_t y = 0; y < out.h; ++y) {
        const uint8_t* src = rgba.rgba + static_cast<size_t>(y) * static_cast<size_t>(rgba.stride);
        uint8_t* dst = out.px.data() + static_cast<size_t>(y) * static_cast<size_t>(out.w);
        for (int32_t x = 0; x < out.w; ++x, src += 4) {
            // Rec.601 luma, then transparency composited over the white page background.
            int luma = (77 * src[0] + 150 * src[1] + 29 * src[2]) >> 8;
            int a = src[3];
            dst[x] = static_cast<uint8_t>(a == 255 ? luma : 255 + (luma - 255) * a / 255);
        }
    }
    WebPFreeDecBuffer(&config.output);
    return true;
}

bool webp_dims(const uint8_t* data, size_t size, Info& info, std::string& err)
{
    int w = 0, h = 0;
    if (!WebPGetInfo(data, size, &w, &h)) {
        err = "webp: bad header";
        return false;
    }
    info.w = static_cast<int32_t>(w);
    info.h = static_cast<int32_t>(h);
    return true;
}

} // namespace

Format sniff(const uint8_t* d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Format::Jpeg;
    if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return Format::Png;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return Format::Webp;
    if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return Format::Gif;
    return Format::Unknown;
}

const char* format_name(Format f)
{
    switch (f) {
    case Format::Jpeg: return "JPEG";
    case Format::Png:  return "PNG";
    case Format::Webp: return "WebP";
    case Format::Gif:  return "GIF";
    case Format::Unknown: break;
    }
    return "unknown";
}

int jpeg_scale_num(int32_t w, int32_t h, const DecodeOptions& opt)
{
    if (w <= 0 || h <= 0 || (opt.fit_w <= 0 && opt.fit_h <= 0)) return 8;
    double s = 1.0;
    if (opt.fit_w > 0) s = static_cast<double>(opt.fit_w) / w;
    if (opt.fit_h > 0) s = std::min(opt.fit_w > 0 ? s : 1e9, static_cast<double>(opt.fit_h) / h);
    // Smallest N with N/8 >= s: libjpeg's output (ceil(w*N/8)) then covers the fit size.
    int n = static_cast<int>(std::ceil(s * 8.0 - 1e-9));
    return std::clamp(n, 1, 8);
}

bool probe(const uint8_t* data, size_t size, Info& out, std::string& err)
{
    out = {};
    out.format = sniff(data, size);
    switch (out.format) {
    case Format::Jpeg: {
        jpeg_decompress_struct cinfo{};
        JpegError jerr{};
        bool ok = jpeg_header(cinfo, jerr, data, size);
        if (ok) {
            out.w = static_cast<int32_t>(cinfo.image_width);
            out.h = static_cast<int32_t>(cinfo.image_height);
        } else {
            err = std::string("jpeg: ") + jerr.message;
        }
        jpeg_destroy_decompress(&cinfo);
        return ok;
    }
    case Format::Png:
        return png_dims(data, size, out, err);
    case Format::Webp:
        return webp_dims(data, size, out, err);
    case Format::Gif:
        err = "GIF images are not supported yet";
        return false;
    case Format::Unknown:
        break;
    }
    err = "not an image (unrecognized format)";
    return false;
}

bool decode_gray(const uint8_t* data, size_t size, const DecodeOptions& opt, Gray& out, std::string& err)
{
    out = {};
    switch (sniff(data, size)) {
    case Format::Jpeg: return decode_jpeg(data, size, opt, out, err);
    case Format::Png:  return decode_png(data, size, opt, out, err);
    case Format::Webp: return decode_webp(data, size, opt, out, err);
    case Format::Gif:  err = "GIF images are not supported yet"; return false;
    case Format::Unknown: break;
    }
    err = "not an image (unrecognized format)";
    return false;
}

} // namespace sumi::image
