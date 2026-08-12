// Session tab — the unique browser hub (M2.5).
#include "session_tab.h"

#include <filesystem>
#include <functional>

#include "app_state.h"
#include "config.h"
#include "cross_store.h"
#include "file_browser.h"
#include "popup_utils.h"
#include "theme.h"
#include "workspace_session.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_internal.h"   // DockBuilder + ImGuiWindow::DockNode

namespace {

ImVec4 modalAccent() {
    return GetAccentBase(StringToAccentColor(appState.currentAccentColor));
}

// Confirm-before-remove modal for the Datasets panel rows.
bool g_showRemoveConfirm = false;
std::string g_removeSourceId;

void renderRemoveConfirm() {
    static int focus = 0;
    static bool wasOpen = false;
    if (!g_showRemoveConfirm) {
        wasOpen = false;
        return;
    }
    ImGui::OpenPopup("Remove Dataset##session");
    beginModal(460.0f, modalAccent());
    if (ImGui::BeginPopupModal("Remove Dataset##session", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Remove Dataset");
        ImGui::Spacing();
        ImGui::TextWrapped("Remove \"%s\" from this multi-workspace? Its workspace tab "
                           "(if open) will be closed. The source file on disk is "
                           "not affected.", g_removeSourceId.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        int pressed = modalButtonRow({"Remove", "Cancel"}, focus, wasOpen, modalAccent());
        if (pressed == 0) {
            std::string err;
            const std::string id = g_removeSourceId;
            if (crossRemoveSource(appState.sessionTab.multiWorkspacePath, id, err)) {
                // Discard any open tab for this source, then refresh the list.
                // Direct removeTab (never closeTab): the source group is
                // already gone from the file, so a dirty-tab "Save" via
                // crossSaveSource would resurrect it as an orphaned group.
                const std::string key =
                    appState.sessionTab.multiWorkspacePath + "#" + id;
                for (int i = 0; i < static_cast<int>(appState.sessions.size()); ++i) {
                    if (appState.sessions[i]->key == key) {
                        removeTab(appState, i);
                        break;   // sessions vector changed
                    }
                }
                std::string err2;
                crossLoad(appState, appState.sessionTab.multiWorkspacePath, err2);
            } else {
                appState.adapterErrorMsg = "Remove failed:\n" + err;
                appState.showAdapterErrorPopup = true;
            }
            g_showRemoveConfirm = false;
            appState.needsRedraw = true;
            ImGui::CloseCurrentPopup();
        } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_showRemoveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        drawModalAccentFrame(modalAccent());
        ImGui::EndPopup();
        wasOpen = true;
    } else {
        wasOpen = false;
    }
    endModal();
}

// Dock-selection fallback: if the window is docked but its node's tab bar is
// showing another tab (background-tab render), force this window's tab to the
// front. Needed per window — see renderUI's pre-DockSpace force.
void forceDockSelection() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->DockNode && w->DockNode->SelectedTabId != w->TabId) {
        w->DockNode->SelectedTabId = w->TabId;
        if (w->DockNode->TabBar)
            w->DockNode->TabBar->NextSelectedTabId = w->TabId;
        appState.needsRedraw = true;
    }
}

// Resolve the MAIN dock space id. Must be computed from the "DockSpace"
// window's ID stack (the same context the DockSpace() call uses) — a bare
// ImGui::GetID here would hash against a different window and miss.
ImGuiID mainDockSpaceId() {
    if (ImGuiWindow* ds = ImGui::FindWindowByName("DockSpace"))
        return ds->GetID("MainDockSpace_v2");
    return 0;
}

// One dockable panel window of the Session tab, docked DIRECTLY in the main
// dock space (no intermediate host window — bugfix: the old nested "Session"
// dock was redundant). FirstUseEver lets the default layout (rebuildDefault-
// Layout) or the user's imgui.ini decide the position; afterwards the panel
// is freely draggable/redockable like every other panel.
void renderSessionPanel(const char* name, const std::function<void()>& content) {
    ImGui::SetNextWindowDockID(mainDockSpaceId(), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name)) {
        forceDockSelection();
        content();
    }
    ImGui::End();
}

}  // namespace

const std::string& SessionTab::title() const {
    return titleCache_;
}

void SessionTab::render() {
    renderRemoveConfirm();
    if (appState.sessionTab.multiWorkspaceOpen) {
        renderMultiWorkspace();
    } else {
        renderSingleFile();
    }
}

// Single-file mode: info pane + [Create Multi-Workspace…] (HL §3.5), hosted in
// the Datasets panel (the natural home — the other two panels keep their
// empty states so the dock layout is stable across the mode switch).
void SessionTab::renderSingleFile() {
    renderSessionPanel("Datasets", [this]() {
        ImGui::Spacing();
        ImGui::TextWrapped("Not a multi-workspace environment.\n\n"
                           "This session holds a single dataset. Create a multi-workspace "
                           "file to embed datasets and open them side by side as tabs.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const bool anyWorkspace = !appState.sessions.empty();
        const int src = (appState.activeTabKind == ActiveTabKind::Workspace &&
                         appState.activeSessionIdx >= 0)
                            ? appState.activeSessionIdx
                            : appState.lastActiveSessionIdx;
        const bool refEmbedded = anyWorkspace && src >= 0 &&
                                 appState.sessions[src]->path.empty();
        if (anyWorkspace) {
            if (refEmbedded) ImGui::BeginDisabled(true);
            if (ImGui::Button("Create Multi-Workspace...", ImVec2(240, 0))) {
                std::string defaultFolder;
                if (std::filesystem::is_directory(appState.currentDirectory))
                    defaultFolder = appState.currentDirectory;
                else if (appState.configPtr && !appState.configPtr->lastMultiWorkspacePath.empty())
                    defaultFolder = std::filesystem::path(
                        appState.configPtr->lastMultiWorkspacePath).parent_path().string();
                std::string path = FileBrowser::showFileSaveDialog(
                    "New Multi-Workspace", "workspace.cross.h5", "*.h5",
                    defaultFolder, glfwGetCurrentContext());
                if (!path.empty()) {
                    // Embed a copy of the most relevant open dataset from disk.
                    const std::string srcPath = appState.sessions[src]->path;
                    std::string err;
                    if (crossCreateFromDataset(appState, path, srcPath, err)) {
                        crossLoad(appState, path, err);
                        rememberMultiWorkspace(appState, path);
                        appState.needsRedraw = true;
                    } else {
                        appState.adapterErrorMsg = "Create failed:\n" + err;
                        appState.showAdapterErrorPopup = true;
                    }
                }
            }
            if (refEmbedded) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The reference tab is embedded in a multi-workspace. "
                                      "Create from a filesystem dataset instead.");
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Creates an empty .cross.h5 and embeds the currently "
                                  "open dataset into it.");
            }
        } else {
            ImGui::TextDisabled("Open a workspace first — the Session tab can then "
                                "turn it into a multi-workspace.");
        }
    });
    renderSessionPanel("Active Environments", [this]() { renderActiveEnvironmentsPanel(); });
    renderSessionPanel("Available Environments", [this]() { renderAvailableEnvironmentsPanel(); });
}

// Multi-workspace mode (bugfix 3): three dockable panels docked DIRECTLY in
// the main dock space — Datasets (left), Active Environments and Available
// Environments (stacked right) per the default layout in rebuildDefaultLayout.
// Bugfix 4: each panel is a scrollable list; no intermediate host window.
void SessionTab::renderMultiWorkspace() {
    renderSessionPanel("Datasets", [this]() {
        ImGui::TextDisabled("Multi-workspace: %s",
                            appState.sessionTab.multiWorkspacePath.c_str());
        ImGui::Spacing();
        renderDatasetsPanel();
    });
    renderSessionPanel("Active Environments", [this]() { renderActiveEnvironmentsPanel(); });
    renderSessionPanel("Available Environments", [this]() { renderAvailableEnvironmentsPanel(); });
}

// (a) embedded datasets: scrollable list + [+ Add Dataset] pinned at the bottom.
void SessionTab::renderDatasetsPanel() {
    const float rowH = ImGui::GetFrameHeight();
    ImGui::BeginChild("##datasetsList", ImVec2(0.0f, -rowH * 2.6f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (appState.sessionTab.sources.empty()) {
        ImGui::TextDisabled("No datasets embedded yet.");
    }
    for (size_t i = 0; i < appState.sessionTab.sources.size(); ++i) {
        const auto& src = appState.sessionTab.sources[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button("x", ImVec2(rowH, rowH))) {
            g_removeSourceId = src.id;
            g_showRemoveConfirm = true;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        const std::string label = src.name + "##open" + src.id;
        // Width 0 = fill the panel's remaining width (Selectable does NOT
        // support negative sizes here — -FLT_MIN shrank the box to ~1 letter).
        if (ImGui::Selectable(label.c_str(), false, 0,
                              ImVec2(0.0f, rowH))) {
            openEmbeddedInNewTab(appState, appState.sessionTab.multiWorkspacePath,
                                 src.id);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (%zu member%s)", src.originPath.c_str(),
                              src.memberCount, src.memberCount == 1 ? "" : "s");
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("+ Add Dataset", ImVec2(-FLT_MIN, 0))) {
        std::string defaultFolder;
        if (std::filesystem::is_directory(appState.currentDirectory))
            defaultFolder = appState.currentDirectory;
        std::string path = FileBrowser::showFileOpenDialog(
            "Add Dataset to Multi-Workspace", "HDF5 files", "*.h5",
            glfwGetCurrentContext(), defaultFolder);
        if (!path.empty()) {
            std::string err, newId;
            if (crossAddSource(appState.sessionTab.multiWorkspacePath, path, newId, err)) {
                std::string err2;
                crossLoad(appState, appState.sessionTab.multiWorkspacePath, err2);
                appState.needsRedraw = true;
            } else {
                appState.adapterErrorMsg = "Add failed:\n" + err;
                appState.showAdapterErrorPopup = true;
            }
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Embeds a copy of the dataset into the multi-workspace file.");
}

// (b) active environments (Phase 3 populates).
void SessionTab::renderActiveEnvironmentsPanel() {
    ImGui::BeginChild("##activeEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextDisabled("No environments open — create one on the right.");
    ImGui::EndChild();
}

// (c) available environment types (Phase 3).
void SessionTab::renderAvailableEnvironmentsPanel() {
    ImGui::BeginChild("##availableEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::BeginDisabled(true);
    if (ImGui::Selectable("Absorbance", false)) {}
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Available in the next phase.");
    if (ImGui::Selectable("Comparator", false)) {}
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Available in the next phase.");
    ImGui::EndDisabled();
    ImGui::EndChild();
}
