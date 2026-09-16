#include "source/postcard.h"

namespace sumi::source {

uint8_t PostcardReader::byte()
{
    if (p_ >= end_) {
        fail();
        return 0;
    }
    return *p_++;
}

uint64_t PostcardReader::varint()
{
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t b = byte();
        if (!ok_) return 0;
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return value;
    }
    fail();
    return 0;
}

int64_t PostcardReader::zigzag()
{
    uint64_t v = varint();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

float PostcardReader::f32()
{
    if (left() < 4) {
        fail();
        return 0;
    }
    float v = 0;
    std::memcpy(&v, p_, 4);
    p_ += 4;
    return v;
}

double PostcardReader::f64()
{
    if (left() < 8) {
        fail();
        return 0;
    }
    double v = 0;
    std::memcpy(&v, p_, 8);
    p_ += 8;
    return v;
}

std::string PostcardReader::text()
{
    uint64_t n = varint();
    if (!ok_ || n > left()) {
        fail();
        return {};
    }
    std::string s(reinterpret_cast<const char*>(p_), static_cast<size_t>(n));
    p_ += n;
    return s;
}

std::vector<std::string> PostcardReader::texts()
{
    uint64_t n = varint();
    std::vector<std::string> out;
    for (uint64_t i = 0; i < n && ok_; ++i) out.push_back(text());
    return out;
}

bool PostcardReader::has_value() { return byte() != 0; }

std::optional<std::string> PostcardReader::maybe_text()
{
    if (!has_value() || !ok_) return std::nullopt;
    return text();
}

std::optional<int64_t> PostcardReader::maybe_i64()
{
    if (!has_value() || !ok_) return std::nullopt;
    return zigzag();
}

std::optional<float> PostcardReader::maybe_f32()
{
    if (!has_value() || !ok_) return std::nullopt;
    return f32();
}

std::optional<std::vector<std::string>> PostcardReader::maybe_texts()
{
    if (!has_value() || !ok_) return std::nullopt;
    return texts();
}

void PostcardWriter::varint(uint64_t v)
{
    do {
        uint8_t b = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out_ += static_cast<char>(b);
    } while (v);
}

void PostcardWriter::zigzag(int64_t v)
{
    varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
}

void PostcardWriter::f32(float v)
{
    char buf[4];
    std::memcpy(buf, &v, 4);
    out_.append(buf, 4);
}

void PostcardWriter::text(const std::string& s)
{
    varint(s.size());
    out_ += s;
}

void PostcardWriter::texts(const std::vector<std::string>& v)
{
    varint(v.size());
    for (const std::string& s : v) text(s);
}

void PostcardWriter::maybe_text(const std::string& s)
{
    if (s.empty()) {
        none();
        return;
    }
    some();
    text(s);
}

void PostcardWriter::maybe_i64(int64_t v, bool present)
{
    if (!present) {
        none();
        return;
    }
    some();
    zigzag(v);
}

void PostcardWriter::maybe_f32(float v, bool present)
{
    if (!present) {
        none();
        return;
    }
    some();
    f32(v);
}

} // namespace sumi::source
