// Phase 4 (M4.4): per-tab-type dock-layout persistence (imgui.ini snapshots).
#include "layout_persistence.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>

namespace {

// The per-type snapshot file lives next to the app's imgui.ini so it travels
// with the user's other settings ("imgui.ini.layout.session" etc.).
std::string layoutFilePath(const char* type) {
    const char* ini = ImGui::GetIO().IniFilename;
    return std::string(ini ? ini : "imgui.ini") + ".layout." + type;
}

}  // namespace

const char* tabTypeName(int activeTabKind) {
    switch (activeTabKind) {
        case 0: return "session";
        case 1: return "workspace";
        case 2: return "environment";
        default: return "workspace";
    }
}

void saveTabLayout(const char* type) {
    // No ImGui context = headless/harness — nothing to snapshot.
    if (!ImGui::GetCurrentContext()) return;
    const char* ini = ImGui::SaveIniSettingsToMemory(nullptr);
    if (!ini) return;
    std::ofstream ofs(layoutFilePath(type),
                      std::ios::binary | std::ios::trunc);
    if (ofs)
        ofs.write(ini, static_cast<std::streamsize>(std::strlen(ini)));
}

void restoreTabLayout(const char* type) {
    if (!ImGui::GetCurrentContext()) return;
    const std::string path = layoutFilePath(type);
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return;
    const std::streamsize n = ifs.tellg();
    if (n <= 0) return;
    std::vector<char> data(static_cast<size_t>(n));
    ifs.seekg(0);
    if (!ifs.read(data.data(), n)) return;
    ImGui::LoadIniSettingsFromMemory(data.data(), static_cast<size_t>(n));
}
