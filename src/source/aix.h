#pragma once
#include <map>
#include <string>

namespace sumi::source {

// An Aidoku source package (M6): a zip holding `Payload/<id>/main.wasm`, `source.json` and icons.
// Only what the app needs is unpacked: the module, its manifest, and (optionally) the icon.
struct AixPackage {
    std::string id;            // from source.json ("en.asurascans")
    std::string name, version_text;
    int         version = 0;
    std::string language;      // first of the languages listed
    std::string base_url;
    int         content_rating = 0;
    std::string min_app_version;
    std::string wasm;          // main.wasm bytes
    std::string icon;          // icon bytes, if the package has one
};

// Read an .aix file. `err` explains any failure.
bool read_aix(const std::string& path, AixPackage& out, std::string& err);

// The files in a zip archive, by name (stored and deflated entries only).
bool read_zip(const std::string& bytes, std::map<std::string, std::string>& files, std::string& err);

} // namespace sumi::source
