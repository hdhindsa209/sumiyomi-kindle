#include "source/util.h"

#include <cctype>
#include <vector>

namespace sumi::source {
namespace {

struct Uri {
    std::string_view scheme, authority, path, query, fragment;
    bool has_scheme = false, has_authority = false, has_query = false, has_fragment = false;
};

// RFC 3986 Appendix B split.
Uri split(std::string_view s)
{
    Uri u;
    size_t i = 0;
    size_t colon = s.find(':');
    size_t first_delim = s.find_first_of("/?#");
    if (colon != std::string_view::npos && colon > 0 && (first_delim == std::string_view::npos || colon < first_delim)) {
        bool valid = std::isalpha(static_cast<unsigned char>(s[0]));
        for (size_t k = 1; k < colon && valid; ++k) {
            unsigned char c = static_cast<unsigned char>(s[k]);
            valid = std::isalnum(c) || c == '+' || c == '-' || c == '.';
        }
        if (valid) {
            u.scheme = s.substr(0, colon);
            u.has_scheme = true;
            i = colon + 1;
        }
    }
    if (s.substr(i, 2) == "//") {
        size_t end = s.find_first_of("/?#", i + 2);
        if (end == std::string_view::npos) end = s.size();
        u.authority = s.substr(i + 2, end - i - 2);
        u.has_authority = true;
        i = end;
    }
    size_t end = s.find_first_of("?#", i);
    if (end == std::string_view::npos) end = s.size();
    u.path = s.substr(i, end - i);
    i = end;
    if (i < s.size() && s[i] == '?') {
        end = s.find('#', i);
        if (end == std::string_view::npos) end = s.size();
        u.query = s.substr(i + 1, end - i - 1);
        u.has_query = true;
        i = end;
    }
    if (i < s.size() && s[i] == '#') {
        u.fragment = s.substr(i + 1);
        u.has_fragment = true;
    }
    return u;
}

// RFC 3986 §5.2.4.
std::string remove_dot_segments(std::string_view in)
{
    std::string input(in), out;
    while (!input.empty()) {
        if (input.rfind("../", 0) == 0) input.erase(0, 3);
        else if (input.rfind("./", 0) == 0) input.erase(0, 2);
        else if (input.rfind("/./", 0) == 0) input.replace(0, 3, "/");
        else if (input == "/.") input = "/";
        else if (input.rfind("/../", 0) == 0 || input == "/..") {
            input = input.size() == 3 ? "/" : input.substr(3);
            size_t slash = out.rfind('/');
            out.erase(slash == std::string::npos ? 0 : slash);
        } else if (input == "." || input == "..") input.clear();
        else {
            size_t start = input[0] == '/' ? 1 : 0;
            size_t next = input.find('/', start);
            if (next == std::string::npos) next = input.size();
            out.append(input, 0, next);
            input.erase(0, next);
        }
    }
    return out;
}

std::string merge(const Uri& base, std::string_view ref_path)
{
    if (base.has_authority && base.path.empty()) return "/" + std::string(ref_path);
    size_t slash = base.path.rfind('/');
    return slash == std::string_view::npos ? std::string(ref_path)
                                           : std::string(base.path.substr(0, slash + 1)) + std::string(ref_path);
}

std::string compose(std::string_view scheme, bool has_auth, std::string_view auth, const std::string& path, bool has_q,
                    std::string_view q, bool has_f, std::string_view f)
{
    std::string out;
    if (!scheme.empty()) (out += scheme) += ':';
    if (has_auth) (out += "//") += auth;
    out += path;
    if (has_q) (out += '?') += q;
    if (has_f) (out += '#') += f;
    return out;
}

} // namespace

std::string url_resolve(std::string_view base_s, std::string_view ref_s)
{
    Uri b = split(base_s), r = split(ref_s);
    if (r.has_scheme)
        return compose(r.scheme, r.has_authority, r.authority, remove_dot_segments(r.path), r.has_query, r.query,
                       r.has_fragment, r.fragment);
    if (r.has_authority)
        return compose(b.scheme, true, r.authority, remove_dot_segments(r.path), r.has_query, r.query, r.has_fragment,
                       r.fragment);
    if (r.path.empty())
        return compose(b.scheme, b.has_authority, b.authority, std::string(b.path), r.has_query || b.has_query,
                       r.has_query ? r.query : b.query, r.has_fragment, r.fragment);
    std::string path = r.path[0] == '/' ? remove_dot_segments(r.path) : remove_dot_segments(merge(b, r.path));
    return compose(b.scheme, b.has_authority, b.authority, path, r.has_query, r.query, r.has_fragment, r.fragment);
}

std::string url_encode(std::string_view s)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string trim(std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9;   // March-based month
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::optional<int64_t> parse_time(std::string_view fmt, std::string_view s)
{
    int64_t year = 1970;
    int month = 1, day = 1, hour = 0, minute = 0, second = 0, millis = 0, offset_min = 0;
    size_t i = 0;

    auto number = [&](int max_digits, int& out) -> bool {
        int n = 0, v = 0;
        while (n < max_digits && i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            v = v * 10 + (s[i] - '0');
            ++i;
            ++n;
        }
        out = v;
        return n > 0;
    };

    for (size_t f = 0; f < fmt.size(); ++f) {
        if (fmt[f] != '%') {
            if (i >= s.size() || s[i] != fmt[f]) return std::nullopt;
            ++i;
            continue;
        }
        if (++f >= fmt.size()) return std::nullopt;
        int v = 0;
        switch (fmt[f]) {
        case 'Y': if (!number(4, v)) return std::nullopt; year = v; break;
        case 'm': if (!number(2, v) || v < 1 || v > 12) return std::nullopt; month = v; break;
        case 'd': if (!number(2, v) || v < 1 || v > 31) return std::nullopt; day = v; break;
        case 'H': if (!number(2, v) || v > 23) return std::nullopt; hour = v; break;
        case 'M': if (!number(2, v) || v > 59) return std::nullopt; minute = v; break;
        case 'S': if (!number(2, v) || v > 60) return std::nullopt; second = v; break;
        case 'f': {
            int digits = 0, frac = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                if (digits < 3) frac = frac * 10 + (s[i] - '0');
                ++digits;
                ++i;
            }
            if (digits == 0) return std::nullopt;
            for (int k = digits; k < 3; ++k) frac *= 10;
            millis = frac;
            break;
        }
        case 'b': {
            static constexpr const char* kMonths[] = {"january", "february", "march", "april", "may", "june", "july",
                                                      "august", "september", "october", "november", "december"};
            size_t start = i;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            std::string word(s.substr(start, i - start));
            for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (word.size() < 3) return std::nullopt;
            int found = 0;
            for (int k = 0; k < 12 && !found; ++k)
                if (std::string_view(kMonths[k]).substr(0, word.size()) == word) found = k + 1;
            if (!found) return std::nullopt;
            month = found;
            break;
        }
        case 'z': {
            if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) { ++i; offset_min = 0; break; }
            if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return std::nullopt;
            int sign = s[i] == '-' ? -1 : 1;
            ++i;
            int hh = 0, mm = 0;
            if (!number(2, hh)) return std::nullopt;
            if (i < s.size() && s[i] == ':') ++i;
            number(2, mm);
            offset_min = sign * (hh * 60 + mm);
            break;
        }
        case '%': if (i >= s.size() || s[i] != '%') return std::nullopt; ++i; break;
        default: return std::nullopt;
        }
    }
    if (i != s.size()) return std::nullopt;

    int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second - static_cast<int64_t>(offset_min) * 60;
    return secs * 1000 + millis;
}

} // namespace sumi::source
