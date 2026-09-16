#pragma once
#include <string>
#include <vector>

namespace sumi::source {

// Extension repository (M5 S6): an index.json listing Lua sources that can be installed without redeploying.
//
//   { "format": 1,
//     "sources": [ { "id": "weebcentral", "name": "WeebCentral", "lang": "en", "version": "1.0.1",
//                    "api_level": 1, "nsfw": false,
//                    "manifest": "weebcentral/manifest.json", "manifest_sha256": "<hex>",
//                    "source":   "weebcentral/source.lua",    "source_sha256":   "<hex>" } ] }
//
// File paths are relative to the index URL (absolute URLs work too). tools/ext/build_index.py writes one from a
// sources/ directory. Each file is checked against its SHA-256 before anything is installed.
struct RepoEntry {
    // Two kinds of repository are understood: Sumiyomi's own (Lua sources, checksummed) and Aidoku's
    // (index.min.json listing .aix packages, which carry no checksums).
    enum class Kind { Lua, Aidoku };
    Kind        kind = Kind::Lua;
    std::string id, name, lang, version;
    int         api_level = 0;
    bool        nsfw = false;
    std::string manifest_url, manifest_sha256, source_url, source_sha256;
    std::string package_url;      // Aidoku: the .aix
    std::string min_app_version;  // Aidoku: the SDK it was built for
};

// Reads either kind of index; `name` receives the repository's own name when it gives one.
bool parse_repo_index(const std::string& json, const std::string& index_url, std::vector<RepoEntry>& out, std::string& err,
                      std::string* name = nullptr);

// `relative` against `base` (a file URL): "a/b.lua" next to it, "/a" from the host root, or absolute as is.
std::string resolve_url(const std::string& base, const std::string& relative);

// Dotted numeric versions ("1.10.0" > "1.9"): <0, 0, >0. Missing parts count as 0.
int compare_versions(const std::string& a, const std::string& b);

// Lower-case hex SHA-256 of `data`.
std::string sha256_hex(const std::string& data);

} // namespace sumi::source
