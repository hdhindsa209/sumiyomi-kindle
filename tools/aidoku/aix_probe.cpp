// Unpack an Aidoku .aix, load its WebAssembly in wasm3 and report what it needs (M6 S1).
//   aix_probe <file.aix>...
#include "source/aix.h"

#include <cstdio>
#include <string>
#include <vector>

#include <wasm3.h>
#include <m3_env.h>

using namespace sumi::source;

namespace {

// Every import wasm3 found, as "module.name".
std::vector<std::string> imports_of(IM3Module mod)
{
    std::vector<std::string> out;
    for (u32 i = 0; i < mod->numFunctions; ++i) {
        const M3Function* f = &mod->functions[i];
        if (f->import.moduleUtf8 && f->import.fieldUtf8)
            out.push_back(std::string(f->import.moduleUtf8) + "." + f->import.fieldUtf8);
    }
    return out;
}

std::vector<std::string> exports_of(IM3Module mod)
{
    std::vector<std::string> out;
    for (u32 i = 0; i < mod->numFunctions; ++i) {
        const M3Function* f = &mod->functions[i];
        for (u16 n = 0; n < f->numNames; ++n)
            if (f->names[n]) out.push_back(f->names[n]);
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.aix>...\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; ++a) {
        AixPackage pkg;
        std::string err;
        if (!read_aix(argv[a], pkg, err)) {
            std::printf("%s: %s\n", argv[a], err.c_str());
            continue;
        }
        std::printf("%s  %s v%d (%s) %zu KB wasm, min app %s\n", pkg.id.c_str(), pkg.name.c_str(), pkg.version,
                    pkg.language.c_str(), pkg.wasm.size() / 1024,
                    pkg.min_app_version.empty() ? "-" : pkg.min_app_version.c_str());

        IM3Environment env = m3_NewEnvironment();
        IM3Runtime runtime = m3_NewRuntime(env, 256 * 1024, nullptr);
        IM3Module mod = nullptr;
        M3Result res = m3_ParseModule(env, &mod, reinterpret_cast<const uint8_t*>(pkg.wasm.data()),
                                      static_cast<uint32_t>(pkg.wasm.size()));
        if (res) {
            std::printf("  parse failed: %s\n", res);
        } else if ((res = m3_LoadModule(runtime, mod))) {
            std::printf("  load failed: %s\n", res);
        } else {
            std::string needs;
            for (const std::string& i : imports_of(mod)) needs += (needs.empty() ? "" : " ") + i;
            std::printf("  imports: %s\n", needs.c_str());
            std::string has;
            for (const std::string& e : exports_of(mod)) has += (has.empty() ? "" : " ") + e;
            std::printf("  exports: %s\n", has.c_str());
        }
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
    }
    return 0;
}
