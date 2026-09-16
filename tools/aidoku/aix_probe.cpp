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

char type_letter(u8 t)
{
    switch (t) {
    case c_m3Type_i32: return 'i';
    case c_m3Type_i64: return 'I';
    case c_m3Type_f32: return 'f';
    case c_m3Type_f64: return 'F';
    case c_m3Type_none: return 'v';
    default: return '?';
    }
}

// Every import wasm3 found, as "module.name sig" with wasm3's signature spelling (return(args)).
std::vector<std::string> imports_of(IM3Module mod)
{
    std::vector<std::string> out;
    for (u32 i = 0; i < mod->numFunctions; ++i) {
        const M3Function* f = &mod->functions[i];
        if (!f->import.moduleUtf8 || !f->import.fieldUtf8) continue;
        std::string sig;
        if (f->funcType) {
            sig += type_letter(f->funcType->numRets ? f->funcType->types[0] : c_m3Type_none);
            sig += '(';
            for (u32 a = 0; a < f->funcType->numArgs; ++a) sig += type_letter(f->funcType->types[f->funcType->numRets + a]);
            sig += ')';
        }
        out.push_back(std::string(f->import.moduleUtf8) + "." + f->import.fieldUtf8 + " " + sig);
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
            for (const std::string& i : imports_of(mod)) std::printf("  import %s\n", i.c_str());
            std::string has;
            for (const std::string& e : exports_of(mod)) has += (has.empty() ? "" : " ") + e;
            std::printf("  exports: %s\n", has.c_str());
        }
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
    }
    return 0;
}
