// Phase 4 (M4.4): dock-layout persistence (imgui.ini snapshots).
#include "layout_persistence.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include "imgui_internal.h"   // ImGuiDockContext / ImGuiDockNode

namespace {

// Snapshots live next to the app's imgui.ini so they travel with the user's
// other settings. `name` is either a tab-type ("session"/"experiment") or a
// per-workspace name ("workspace.<keyhash>" — every dataset keeps its own
// docking arrangement).
std::string layoutFilePath(const std::string& name) {
    const char* ini = ImGui::GetIO().IniFilename;
    return std::string(ini ? ini : "imgui.ini") + ".layout." + name;
}

// Sidecar: the SELECTED window per dock node ("0x<NodeID>=<WindowName>").
std::string selectionFilePath(const std::string& name) {
    return layoutFilePath(name) + ".sel";
}

// Stable per-workspace snapshot name: hash the session key (a path, or
// "cross.h5#sourceId") into a filename-safe suffix.
std::string workspaceLayoutName(const std::string& key) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016zx", std::hash<std::string>{}(key));
    return std::string("workspace.") + hex;
}

}  // namespace

// The window of `node` whose TabId == node->SelectedTabId (nullptr when
// none/unknown). SelectedTabId is a TAB id (ImHashStr("#TAB", window-id)),
// NOT the window's own ID — so FindWindowByID(SelectedTabId) never matches.
// This TabId-based resolution is the correct inverse of DockNodeAddTabBar.
ImGuiWindow* nodeSelectedWindow(ImGuiDockNode* node) {
    if (!node || !node->SelectedTabId) return nullptr;
    for (int n = 0; n < node->Windows.Size; ++n)
        if (node->Windows[n]->TabId == node->SelectedTabId)
            return node->Windows[n];
    return nullptr;
}

// Resolve the selected window of a leaf node (replicates ImGui's internal
// DockNodeFindWindowByID). Returns nullptr when none/unknown.
const char* nodeSelectedWindowName(ImGuiDockNode* node) {
    if (ImGuiWindow* w = nodeSelectedWindow(node))
        return w->Name;
    return nullptr;
}

std::map<ImGuiID, std::string> g_restoredNodeSelection;

void saveLayoutTo(const std::string& name) {
    const char* ini = ImGui::SaveIniSettingsToMemory(nullptr);
    if (!ini) return;
    std::ofstream ofs(layoutFilePath(name),
                      std::ios::binary | std::ios::trunc);
    if (ofs)
        ofs.write(ini, static_cast<std::streamsize>(std::strlen(ini)));
    // Sidecar: per-node selected window. ImGui's ini restore skips nodes that
    // already exist, so the selection must be re-applied app-side — capture
    // it here while the live tree is current.
    ImGuiDockContext* dc = &ImGui::GetCurrentContext()->DockContext;
    std::ofstream sel(selectionFilePath(name), std::ios::trunc);
    if (!sel) return;
    for (int n = 0; n < dc->Nodes.Data.Size; ++n) {
        ImGuiDockNode* node = (ImGuiDockNode*)dc->Nodes.Data[n].val_p;
        if (!node || !node->IsLeafNode() || node->Windows.Size < 2) continue;
        if (const char* name = nodeSelectedWindowName(node))
            sel << std::hex << node->ID << "=" << name << "\n";
    }
}

void restoreLayoutFrom(const std::string& name) {
    const std::string path = layoutFilePath(name);
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return;
    const std::streamsize n = ifs.tellg();
    if (n <= 0) return;
    std::vector<char> data(static_cast<size_t>(n));
    ifs.seekg(0);
    if (!ifs.read(data.data(), n)) return;
    ImGui::LoadIniSettingsFromMemory(data.data(), static_cast<size_t>(n));
    // Read the sidecar for app-side re-application (see header comment).
    g_restoredNodeSelection.clear();
    std::ifstream sel(selectionFilePath(name));
    std::string line;
    while (std::getline(sel, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = line.substr(eq + 1);
        if (name.empty()) continue;
        std::istringstream iss(line.substr(0, eq));
        unsigned long long id = 0;
        iss >> std::hex >> id;
        if (id != 0) g_restoredNodeSelection[static_cast<ImGuiID>(id)] = name;
    }
}

const char* tabTypeName(int activeTabKind) {
    switch (activeTabKind) {
        case 0: return "session";
        case 1: return "workspace";
        case 2: return "experiment";
        default: return "workspace";
    }
}

void saveTabLayout(const char* type) {
    // No ImGui context = headless/harness — nothing to snapshot.
    if (!ImGui::GetCurrentContext()) return;
    saveLayoutTo(type);
}

void restoreTabLayout(const char* type) {
    if (!ImGui::GetCurrentContext()) return;
    restoreLayoutFrom(type);
}

void saveWorkspaceLayout(const std::string& key) {
    if (!ImGui::GetCurrentContext()) return;
    saveLayoutTo(workspaceLayoutName(key));
}

void restoreWorkspaceLayout(const std::string& key) {
    if (!ImGui::GetCurrentContext()) return;
    restoreLayoutFrom(workspaceLayoutName(key));
}

void resetTabLayout(const char* type) {
    if (!ImGui::GetCurrentContext()) return;
    std::remove(layoutFilePath(type).c_str());
    std::remove(selectionFilePath(type).c_str());
}

std::map<ImGuiID, std::string> takeRestoredNodeSelection() {
    std::map<ImGuiID, std::string> out;
    out.swap(g_restoredNodeSelection);
    return out;
}

void pruneStaleWorkspaceLayouts(const std::vector<std::string>& keepKeys,
                                 const std::vector<std::string>& recentPaths) {
    if (!ImGui::GetCurrentContext()) return;
    const char* ini = ImGui::GetIO().IniFilename;
    if (!ini) return;

    // Build the set of workspace-layout name suffixes to keep: one per
    // currently-open session key + one per recent-dataset path. Each hashes
    // to "workspace.<16hex>" via workspaceLayoutName.
    std::unordered_set<std::string> keepHex;
    for (const std::string& key : keepKeys)
        keepHex.insert(workspaceLayoutName(key));
    for (const std::string& path : recentPaths)
        keepHex.insert(workspaceLayoutName(path));

    // Snapshots live next to imgui.ini; iterate the directory and delete any
    // workspace.* snapshot (or its .sel sidecar) not in the keep set.
    std::filesystem::path dir = std::filesystem::path(ini).parent_path();
    if (dir.empty()) dir = ".";
    if (!std::filesystem::exists(dir)) return;

    const std::string prefix = "imgui.ini.layout.workspace.";
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string fname = entry.path().filename().string();
        if (fname.rfind(prefix, 0) != 0) continue;
        // fname == "imgui.ini.layout.workspace.<hex>" or "...<hex>.sel"
        const std::string rest = fname.substr(prefix.size());
        const size_t dot = rest.find('.');
        const std::string hex = (dot == std::string::npos) ? rest
                                                           : rest.substr(0, dot);
        if (keepHex.count(hex) == 0)
            std::filesystem::remove(entry.path());
    }
}
