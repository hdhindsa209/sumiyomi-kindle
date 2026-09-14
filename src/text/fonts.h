#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;

namespace sumi {

// The bundled faces (assets/fonts, design doc §5.2).
enum class FontId : uint8_t { InterRegular, InterMedium, InterSemiBold, Icons };
constexpr int kFontCount = 4;

// Owns FreeType + the font files (loaded into memory once) and hands out faces at a pixel
// size, creating each (font, size) combination on first use. A UI uses a handful of sizes.
class Fonts {
public:
    Fonts();
    ~Fonts();
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;

    bool open(const std::string& fonts_dir, int32_t dpi, std::string& err);

    // Scale-independent pixels -> device pixels (design doc §5.3: px = sp * dpi / 160).
    int32_t sp(float sp) const { return static_cast<int32_t>(sp * static_cast<float>(dpi_) / 160.0f + 0.5f); }
    int32_t dpi() const { return dpi_; }

    struct Sized {
        FT_FaceRec_* face = nullptr;
        hb_font_t*   hb   = nullptr;   // null for the icon font (no shaping)
    };
    // Null face if `px` is out of range or the face can't be created.
    Sized sized(FontId id, int32_t px);

private:
    struct Entry { FontId id; int32_t px; Sized s; };

    FT_LibraryRec_*      ft_ = nullptr;
    std::vector<uint8_t> data_[kFontCount];
    std::vector<Entry>   sized_;
    int32_t              dpi_ = 160;
};

} // namespace sumi
