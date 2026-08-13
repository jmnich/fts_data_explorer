// Session tab — the unique browser hub (M2.5).
#include "session_tab.h"

#include <filesystem>
#include <functional>

#include "app_state.h"
#include "config.h"
#include "cross_store.h"
#include "environment_session.h"
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

// ── Session panel styling (polish 2026-08) ────────────────────────────────
// All colors from theme.h; no new fonts, textures, or dependencies. Sticks to
// the app's accent language (modalAccent is reused throughout).

// Section header: accent title, right-aligned count chip (omitted when
// count < 0), thin accent underline.
void renderSectionHeader(const char* title, int count = -1) {
    ImGui::PushStyleColor(ImGuiCol_Text, modalAccent());
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (count >= 0) {
        const std::string chip = "· " + std::to_string(count);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                             ImGui::CalcTextSize(chip.c_str()).x -
                             ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(chip.c_str());
        ImGui::PopStyleColor();
    }
    const ImVec2 pos = ImGui::GetWindowPos();
    const float y = ImGui::GetCursorScreenPos().y;
    ImVec4 accent = modalAccent();
    accent.w = 0.35f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(pos.x, y), ImVec2(pos.x + ImGui::GetWindowWidth(), y),
        ImGui::ColorConvertFloat4ToU32(accent));
    ImGui::Spacing();
}

// True when the source has an open workspace tab.
bool sourceOpenInTab(const std::string& id) {
    const std::string key = appState.sessionTab.multiWorkspacePath + "#" + id;
    for (const auto& sess : appState.sessions)
        if (sess->key == key) return true;
    return false;
}

// File-browser → embed flow (shared by the pinned footer button).
void addDatasetFromFileDialog() {
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

// Accent-tinted, full-width pinned footer button.
void renderAddDatasetButton() {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(ac));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(ac));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetAccentActive(ac));
    if (ImGui::Button("+ Add Dataset", ImVec2(-FLT_MIN, 0))) {
        addDatasetFromFileDialog();
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Embeds a copy of the dataset into the multi-workspace file.");
}

// Accent-framed identity card: filename, shortened path (tooltip = full),
// dataset/member counts.
void renderMultiWorkspaceCard(const std::string& path) {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetAccentVeryMuted(ac));
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
    const bool open = ImGui::BeginChild("##crossCard", ImVec2(0.0f, 62.0f), true);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (open) {
        ImGui::TextUnformatted(std::filesystem::path(path).filename().string().c_str());
        std::string shortPath = path;
        if (shortPath.size() > 56)
            shortPath = shortPath.substr(0, 10) + "..." +
                        shortPath.substr(shortPath.size() - 40);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(shortPath.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        size_t members = 0;
        for (const auto& src : appState.sessionTab.sources)
            members += src.memberCount;
        ImGui::Text("%zu dataset%s · %zu member%s",
                    appState.sessionTab.sources.size(),
                    appState.sessionTab.sources.size() == 1 ? "" : "s",
                    members, members == 1 ? "" : "s");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::Spacing();
}

// Centered dim message inside a scrollable panel (empty state).
void renderCenteredEmptyLine(const char* line, float offsetFrac) {
    const float avail = ImGui::GetContentRegionAvail().y;
    if (avail > 90.0f)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * offsetFrac);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
        (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(line).x) * 0.5f);
    ImGui::TextDisabled("%s", line);
}

// Create Multi-Workspace button (single-file mode): accent-tinted, with the
// disabled/hint variants of the original flow.
void renderCreateMultiWorkspaceButton() {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    const bool anyWorkspace = !appState.sessions.empty();
    const int src = (appState.activeTabKind == ActiveTabKind::Workspace &&
                     appState.activeSessionIdx >= 0)
                        ? appState.activeSessionIdx
                        : appState.lastActiveSessionIdx;
    const bool refEmbedded = anyWorkspace && src >= 0 &&
                             appState.sessions[src]->path.empty();
    if (anyWorkspace && !refEmbedded) {
        ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(ac));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(ac));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetAccentActive(ac));
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
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Creates an empty .cross.h5 and embeds the currently "
                              "open dataset into it.");
    } else if (refEmbedded) {
        ImGui::BeginDisabled(true);
        ImGui::Button("Create Multi-Workspace...", ImVec2(240, 0));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The reference tab is embedded in a multi-workspace. "
                              "Create from a filesystem dataset instead.");
    } else {
        ImGui::TextDisabled("Open a workspace first — the Session tab can then "
                            "turn it into a multi-workspace.");
    }
}

}  // namespace

// One environment-type card (column c) — defined below (file scope, after
// the anonymous namespace), used by renderAvailableEnvironmentsPanel.
static void renderEnvTypeCard(const char* title, const char* desc, EnvType type);

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

// Single-file mode: hero card + [Create Multi-Workspace…] (HL §3.5), hosted in
// the Datasets panel (the natural home — the other two panels keep their
// empty states so the dock layout is stable across the mode switch).
void SessionTab::renderSingleFile() {
    renderSessionPanel("Datasets", [this]() {
        renderSectionHeader("DATASETS");
        const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, GetAccentVeryMuted(ac));
        ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
        const bool open = ImGui::BeginChild("##singleCard", ImVec2(0.0f, 0.0f), true);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        if (open) {
            ImGui::TextUnformatted("Single dataset session");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            ImGui::TextWrapped("This session holds a single dataset. Create a "
                               "multi-workspace file to embed datasets and open "
                               "them side by side as tabs.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            renderCreateMultiWorkspaceButton();
        }
        ImGui::EndChild();
    });
    renderSessionPanel("Active Environments", [this]() { renderActiveEnvironmentsPanel(); });
    renderSessionPanel("Available Environments", [this]() { renderAvailableEnvironmentsPanel(); });
}

// Multi-workspace mode: three dockable panels docked DIRECTLY in the main
// dock space — Datasets (left), Active Environments and Available
// Environments (stacked right) per the default layout in rebuildDefaultLayout.
void SessionTab::renderMultiWorkspace() {
    renderSessionPanel("Datasets", [this]() {
        renderSectionHeader("DATASETS",
                            static_cast<int>(appState.sessionTab.sources.size()));
        renderMultiWorkspaceCard(appState.sessionTab.multiWorkspacePath);
        renderDatasetsPanel();
    });
    renderSessionPanel("Active Environments", [this]() { renderActiveEnvironmentsPanel(); });
    renderSessionPanel("Available Environments", [this]() { renderAvailableEnvironmentsPanel(); });
}

// (a) embedded datasets: two-line rows (name + metadata) in a scrollable list,
// [+ Add Dataset] pinned at the bottom.
void SessionTab::renderDatasetsPanel() {
    const float rowH = ImGui::GetFrameHeight();
    const float twoLineH = rowH + ImGui::GetTextLineHeightWithSpacing() + 2.0f;
    ImGui::BeginChild("##datasetsList",
                      ImVec2(0.0f, -(rowH + ImGui::GetStyle().ItemSpacing.y * 2.0f)),
                      false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (appState.sessionTab.sources.empty()) {
        renderCenteredEmptyLine("No datasets embedded yet.", 0.25f);
        renderCenteredEmptyLine("Use \"+ Add Dataset\" below to embed one.", 0.05f);
    }
    for (size_t i = 0; i < appState.sessionTab.sources.size(); ++i) {
        const auto& src = appState.sessionTab.sources[i];
        ImGui::PushID(static_cast<int>(i));

        // Remove: square, dim, brightens on hover (no layout jump).
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button("×", ImVec2(rowH, rowH))) {
            g_removeSourceId = src.id;
            g_showRemoveConfirm = true;
            appState.needsRedraw = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();

        const std::string label = src.name + "##open" + src.id;
        // Width 0 = fill the panel's remaining width (Selectable does NOT
        // support negative sizes here — -FLT_MIN shrank the box to ~1 letter).
        if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.0f, twoLineH))) {
            openEmbeddedInNewTab(appState, appState.sessionTab.multiWorkspacePath,
                                 src.id);
        }
        // Open-in-tab indicator: accent dot at the row's left edge (drawn over
        // the Selectable's padding, clear of the name text).
        if (sourceOpenInTab(src.id)) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(min.x + 2.5f, (min.y + max.y) * 0.5f), 2.0f,
                ImGui::ColorConvertFloat4ToU32(modalAccent()));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s (%zu member%s)", src.originPath.c_str(),
                              src.memberCount, src.memberCount == 1 ? "" : "s");

        // Metadata line, indented to the name column.
        ImGui::Indent(rowH + ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("%zu member%s · created %s", src.memberCount,
                    src.memberCount == 1 ? "" : "s", src.createdIso.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::EndChild();
    renderAddDatasetButton();
}

// (b) active environments: live instances (Phase 3). Click → activate tab.
void SessionTab::renderActiveEnvironmentsPanel() {
    ImGui::BeginChild("##activeEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (appState.environments.empty()) {
        renderCenteredEmptyLine("No environments open.", 0.45f);
    }
    for (size_t i = 0; i < appState.environments.size(); ++i) {
        const auto* env = appState.environments[i].get();
        const bool isActive = (appState.activeTabKind == ActiveTabKind::Environment &&
                               appState.activeEnvIdx == static_cast<int>(i));
        const std::string label = env->title() + "##active" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), isActive)) {
            activateEnvironment(appState, static_cast<int>(i));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                env->type == EnvType::Absorbance ? "Absorbance against a stored reference."
                                                 : "Average-spectrum comparison overlay.");
        }
    }
    ImGui::EndChild();
}

// (c) available environment types: clickable cards → create instance (Phase 3).
void SessionTab::renderAvailableEnvironmentsPanel() {
    ImGui::BeginChild("##availableEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    renderEnvTypeCard("Absorbance",
                      "Absorbance spectra against a stored background reference.",
                      EnvType::Absorbance);
    renderEnvTypeCard("Comparator", "Pairwise comparison of two datasets.",
                      EnvType::Comparator);
    ImGui::EndChild();
}

// One environment-type card: accent-framed, clickable — creates a new
// instance (auto-name, activated).
static void renderEnvTypeCard(const char* title, const char* desc, EnvType type) {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
    const bool open = ImGui::BeginChild(title,
        ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 2.0f + 16.0f), true);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open) {
        ImGui::TextUnformatted(title);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                             ImGui::CalcTextSize("+ New").x -
                             ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, modalAccent());
        ImGui::TextUnformatted("+ New");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
        ImGui::TextWrapped("%s", desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    if (ImGui::IsItemClicked()) {
        createEnvironment(appState, type);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create a new %s instance.", title);
    ImGui::Spacing();
}
