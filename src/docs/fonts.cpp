#include "docs/fonts.h"

#include <fontconfig/fontconfig.h>

// From fontconfig/fcfreetype.h (not in the Kindle's 2.8 header set; same signature since 2.0).
extern "C" FcPattern* FcFreeTypeQuery(const FcChar8* file, int id, FcBlanks* blanks, int* count);

#include <dirent.h>

#include <algorithm>
#include <set>

namespace zk {

std::vector<std::string> readingFontDirs() {
#ifdef ZK_KINDLE
    return {"/usr/java/lib/fonts", "/mnt/us/zotero/fonts"};
#else
    return {"assets/fonts-device", "local/fonts"};
#endif
}

std::vector<std::string> readingFonts(const std::vector<std::string>& dirs) {
    std::set<std::string> families;
    for (const auto& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext != ".ttf" && ext != ".otf") continue;
            const std::string path = dir + "/" + name;
            int count = 0;
            FcPattern* p = FcFreeTypeQuery(reinterpret_cast<const FcChar8*>(path.c_str()), 0, nullptr, &count);
            if (!p) continue;
            FcChar8* family = nullptr;
            int slant = FC_SLANT_ROMAN, weight = FC_WEIGHT_REGULAR;
            FcPatternGetInteger(p, FC_SLANT, 0, &slant);
            FcPatternGetInteger(p, FC_WEIGHT, 0, &weight);
            if (FcPatternGetString(p, FC_FAMILY, 0, &family) == FcResultMatch && slant == FC_SLANT_ROMAN &&
                weight >= FC_WEIGHT_BOOK && weight <= FC_WEIGHT_MEDIUM)
                families.insert(reinterpret_cast<const char*>(family));
            FcPatternDestroy(p);
        }
        closedir(d);
    }
    std::vector<std::string> out(families.begin(), families.end());
    auto it = std::find(out.begin(), out.end(), "Bookerly");
    if (it != out.end()) std::rotate(out.begin(), it, it + 1);
    return out;
}

}  // namespace zk
