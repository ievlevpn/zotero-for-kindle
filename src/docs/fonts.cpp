#include "docs/fonts.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cstdlib>
#include <set>

namespace zk {

std::vector<std::string> readingFontDirs() {
#ifdef ZK_KINDLE
    // The Kindle's own, the app's (Fast Font; replaced by updates), the user's own.
    return {"/usr/java/lib/fonts", "/mnt/us/zotero/share/fonts", "/mnt/us/zotero/fonts"};
#else
    return {"assets/fonts-device", "assets/fonts-fast", "local/fonts"};
#endif
}

bool offeredFamily(const std::string& family, bool systemFont) {
    if (!systemFont) return true;
    static const std::set<std::string> reading = {"Bookerly", "Amazon Ember", "Baskerville", "Caecilia", "Caecilia Regular",
                                                  "Futura", "Helvetica Neue LT", "OpenDyslexic", "Palatino"};
    return reading.count(family) > 0;
}

// From fontconfig's cache, never by opening the files: reading the Kindle's 128 MB of system
// fonts (CJK fonts of 10+ MB) with FreeType took 75 s on the UI thread — the "Display" freeze.
// A directory fontconfig doesn't know yet is added to the app's font set (scanned once).
std::vector<std::string> readingFonts(const std::vector<std::string>& dirs) {
    FcConfig* config = FcConfigGetCurrent();
    std::vector<std::string> prefixes;
    for (std::string dir : dirs) {
        if (FcFileIsDir(reinterpret_cast<const FcChar8*>(dir.c_str())) != FcTrue) continue;
        FcConfigAppFontAddDir(config, reinterpret_cast<const FcChar8*>(dir.c_str()));  // no-op if known
        char* real = realpath(dir.c_str(), nullptr);
        if (real) {
            dir = real;
            free(real);
        }
        prefixes.push_back(dir.back() == '/' ? dir : dir + "/");
    }
    std::set<std::string> families;
    if (prefixes.empty()) return {};
    FcPattern* any = FcPatternCreate();
    FcObjectSet* fields = FcObjectSetBuild(FC_FAMILY, FC_SLANT, FC_WEIGHT, FC_FILE, nullptr);
    FcFontSet* set = FcFontList(config, any, fields);
    for (int i = 0; set && i < set->nfont; ++i) {
        FcPattern* p = set->fonts[i];
        FcChar8 *family = nullptr, *file = nullptr;
        int slant = FC_SLANT_ROMAN, weight = FC_WEIGHT_REGULAR;
        if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch ||
            FcPatternGetString(p, FC_FAMILY, 0, &family) != FcResultMatch)
            continue;
        std::string path = reinterpret_cast<const char*>(file);
        if (char* real = realpath(path.c_str(), nullptr)) {
            path = real;
            free(real);
        }
        const bool inDirs = std::any_of(prefixes.begin(), prefixes.end(),
                                        [&](const std::string& d) { return path.rfind(d, 0) == 0; });
        FcPatternGetInteger(p, FC_SLANT, 0, &slant);
        FcPatternGetInteger(p, FC_WEIGHT, 0, &weight);
        const bool system = path.rfind("/usr/java/lib/fonts/", 0) == 0;
        if (inDirs && offeredFamily(reinterpret_cast<const char*>(family), system) && slant == FC_SLANT_ROMAN && weight >= FC_WEIGHT_BOOK && weight <= FC_WEIGHT_MEDIUM)
            families.insert(reinterpret_cast<const char*>(family));
    }
    if (set) FcFontSetDestroy(set);
    FcObjectSetDestroy(fields);
    FcPatternDestroy(any);
    std::vector<std::string> out(families.begin(), families.end());
    auto it = std::find(out.begin(), out.end(), "Bookerly");
    if (it != out.end()) std::rotate(out.begin(), it, it + 1);
    return out;
}

}  // namespace zk
