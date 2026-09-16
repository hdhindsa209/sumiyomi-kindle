#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sumi::source {

// Postcard (https://postcard.jamesmunns.com), the format Aidoku sources use to hand values across the
// WebAssembly boundary. Only what the app needs is here:
//   integers  LEB128 varints; signed ones zigzagged
//   f32/f64   fixed 4/8 bytes, little endian
//   bool      one byte
//   String    varint length, then UTF-8
//   Vec<T>    varint length, then items
//   Option<T> 0 = none, 1 = some, then the value
//   enum      varint discriminant, then the variant's payload
//   struct    its fields in order, nothing else
class PostcardReader {
public:
    PostcardReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    explicit PostcardReader(const std::string& s)
        : p_(reinterpret_cast<const uint8_t*>(s.data())), end_(p_ + s.size()) {}

    bool ok() const { return ok_; }
    size_t left() const { return static_cast<size_t>(end_ - p_); }

    uint64_t varint();
    int64_t  zigzag();
    bool     boolean() { return byte() != 0; }
    float    f32();
    double   f64();
    std::string text();
    std::vector<std::string> texts();
    // Option<String>: an empty result means none.
    std::optional<std::string> maybe_text();
    std::optional<int64_t> maybe_i64();
    std::optional<float> maybe_f32();
    std::optional<std::vector<std::string>> maybe_texts();
    bool has_value();   // the 0/1 byte of an Option
    uint8_t byte();

private:
    void fail() { ok_ = false; }
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

// The same format, for the values sources are given (a manga, a chapter, a search query).
class PostcardWriter {
public:
    void varint(uint64_t v);
    void zigzag(int64_t v);
    void boolean(bool v) { out_ += static_cast<char>(v ? 1 : 0); }
    void f32(float v);
    void text(const std::string& s);
    void texts(const std::vector<std::string>& v);
    void none() { out_ += '\0'; }
    void some() { out_ += '\1'; }
    void maybe_text(const std::string& s);        // empty writes none
    void maybe_i64(int64_t v, bool present);
    void maybe_f32(float v, bool present);
    void byte(uint8_t b) { out_ += static_cast<char>(b); }
    const std::string& bytes() const { return out_; }

private:
    std::string out_;
};

} // namespace sumi::source
