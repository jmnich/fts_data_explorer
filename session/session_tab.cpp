// Session tab — the unique browser hub (M2.5).
#include "session_tab.h"

#include <filesystem>

#include "app_state.h"
#include "config.h"
#include "cross_store.h"
#include "file_browser.h"
#include "popup_utils.h"
#include "theme.h"
#include "workspace_session.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include "imgui_internal.h"   // SESSION-BEGIN debug (temporary)

namespace {

ImVec4 modalAccent() {
    return GetAccentBase(StringToAccentColor(appState.currentAccentColor));
}

// Confirm-before-remove modal for column (a) rows.
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

}  // namespace

const std::string& SessionTab::title() const {
    return titleCache_;
}

void SessionTab::render() {
    fprintf(stderr, "SESSION-RENDER kind=%d multi=%d\n",
            (int)appState.activeTabKind, (int)appState.sessionTab.multiWorkspaceOpen);
    renderRemoveConfirm();
    if (!ImGui::Begin("Session")) {
        fprintf(stderr, "SESSION-BEGIN false\n");
        ImGui::End();
        return;
    }
    fprintf(stderr, "SESSION-BEGIN ok name=%s clip=(%.0f,%.0f,%.0f,%.0f) cmds=%d\n",
            ImGui::GetCurrentWindow()->Name,
            ImGui::GetCurrentWindow()->ClipRect.Min.x, ImGui::GetCurrentWindow()->ClipRect.Min.y,
            ImGui::GetCurrentWindow()->ClipRect.Max.x, ImGui::GetCurrentWindow()->ClipRect.Max.y,
            (int)ImGui::GetCurrentWindow()->DrawList->CmdBuffer.Size);
    // Force the dock node to show this window: without it the Session window
    // stays a background tab in its node (renders vertices, never shown).
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        if (w->DockNode) {
            // The dock node draws its background with LastBgColor (defaults to
            // WHITE on node creation). If the node never hosted another window
            // it stays white — the Session tab then renders white-on-white and
            // looks empty. Pull the node bg to the window bg.
            w->DockNode->LastBgColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
            if (w->DockNode->SelectedTabId != w->TabId) {
                w->DockNode->SelectedTabId = w->TabId;
                if (w->DockNode->TabBar)
                    w->DockNode->TabBar->NextSelectedTabId = w->TabId;
            }
        }
    }
    if (appState.sessionTab.multiWorkspaceOpen)
        renderMultiWorkspace();
    else
        renderSingleFile();
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImDrawList* dl = w->DrawList;
        ImDrawList* hostDl = w->ParentWindow ? w->ParentWindow->DrawList : nullptr;
        fprintf(stderr, "SESSION-DL ownCmds=%d ownVtx=%d hostCmds=%d parent=%s\n",
                (int)dl->CmdBuffer.Size, (int)dl->VtxBuffer.Size,
                hostDl ? (int)hostDl->CmdBuffer.Size : -1,
                w->ParentWindow ? w->ParentWindow->Name : "(none)");
        if (dl->VtxBuffer.Size > 5)
            fprintf(stderr, "SESSION-DL vtx0=(%.0f,%.0f) clip0=(%.0f,%.0f,%.0f,%.0f)\n",
                    dl->VtxBuffer[0].pos.x, dl->VtxBuffer[0].pos.y,
                    dl->CmdBuffer[0].ClipRect.x, dl->CmdBuffer[0].ClipRect.y,
                    dl->CmdBuffer[0].ClipRect.z, dl->CmdBuffer[0].ClipRect.w);
    }
    fprintf(stderr, "SESSION-END cmds=%d vtx=%d cursor=(%.0f,%.0f)\n",
            (int)ImGui::GetCurrentWindow()->DrawList->CmdBuffer.Size,
            (int)ImGui::GetCurrentWindow()->DrawList->VtxBuffer.Size,
            ImGui::GetCursorPos().x, ImGui::GetCursorPos().y);
    if (ImGui::GetCurrentWindow()->DrawList->VtxBuffer.Size > 5) {
        const auto& v = ImGui::GetCurrentWindow()->DrawList->VtxBuffer;
        fprintf(stderr, "SESSION-VTX vtx[0..3]: pos=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) col0=%08X col2=%08X cmd0vtx=%d\n",
                v[0].pos.x, v[0].pos.y, v[1].pos.x, v[1].pos.y,
                v[2].pos.x, v[2].pos.y, v[3].pos.x, v[3].pos.y,
                v[0].col, v[2].col,
                ImGui::GetCurrentWindow()->DrawList->CmdBuffer[0].ElemCount);
        const ImDrawCmd& c0 = ImGui::GetCurrentWindow()->DrawList->CmdBuffer[1];
        fprintf(stderr, "SESSION-CMD1 clip=(%.0f,%.0f,%.0f,%.0f)\n",
                c0.ClipRect.x, c0.ClipRect.y, c0.ClipRect.z, c0.ClipRect.w);
    }
    ImGui::End();
}

// Single-file mode: info pane + [Create Multi-Workspace…] (HL §3.5).
void SessionTab::renderSingleFile() {
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
}

// Multi-workspace mode: 3 columns (HL §3.5).
void SessionTab::renderMultiWorkspace() {
    ImGui::TextWrapped("Multi-workspace: %s",
                       appState.sessionTab.multiWorkspacePath.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(3, "##sessionColumns", true);
    ImGui::Separator();

    // ── (a) embedded datasets ───────────────────────────────────────────────
    ImGui::Text("Datasets");
    ImGui::Separator();
    if (appState.sessionTab.sources.empty()) {
        ImGui::TextDisabled("No datasets embedded yet.");
    }
    const float rowH = ImGui::GetFrameHeight();
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
    ImGui::Spacing();
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

    ImGui::NextColumn();

    // ── (b) active environments (Phase 3 populates) ────────────────────────
    ImGui::Text("Active Environments");
    ImGui::Separator();
    ImGui::TextDisabled("No environments open — create one on the right.");

    ImGui::NextColumn();

    // ── (c) available environment types (Phase 3) ──────────────────────────
    ImGui::Text("Available Environments");
    ImGui::Separator();
    ImGui::BeginDisabled(true);
    if (ImGui::Selectable("Absorbance", false)) {}
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Available in the next phase.");
    if (ImGui::Selectable("Comparator", false)) {}
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Available in the next phase.");
    ImGui::EndDisabled();

    ImGui::Columns(1);
}
