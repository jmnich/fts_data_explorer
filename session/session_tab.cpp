// Session tab — the unique browser hub (M2.5).
#include "session_tab.h"

#include <cstdio>
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
#include "wrap_text.h"

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
    if (appState.active && std::filesystem::is_directory(appState.active->currentDirectory))
        defaultFolder = appState.active->currentDirectory;
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

// Accent-tinted, full-width pinned footer button (2x frame height).
void renderAddDatasetButton() {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(ac));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(ac));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetAccentActive(ac));
    if (ImGui::Button("+ Add Dataset",
                      ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 2.0f))) {
        addDatasetFromFileDialog();
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Embeds a copy of the dataset into the multi-workspace file.");
}

// Accent-framed identity card: filename, shortened path (tooltip = full),
// dataset count + on-disk size in MB. Path wraps (clamped to 2 lines) instead
// of the old manual "…" truncation.
void renderMultiWorkspaceCard(const std::string& path) {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    const float cardW =
        ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f - 2.0f;
    const std::vector<std::string> nameLines =
        wrapToLines("Name: " + std::filesystem::path(path).filename().string(),
                    cardW - 20.0f, 2);
    const std::vector<std::string> pathLines = wrapToLines(path, cardW - 20.0f, 2);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, GetAccentVeryMuted(ac));
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
    const bool open = ImGui::BeginChild(
        "##crossCard",
        ImVec2(0.0f, 16.0f + (nameLines.size() + pathLines.size() + 1) * lineH),
        true);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (open) {
        for (const auto& l : nameLines) ImGui::TextUnformatted(l.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        for (const auto& l : pathLines) ImGui::TextUnformatted(l.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        const size_t datasets = appState.sessionTab.sources.size();
        std::error_code ec;
        const auto fileBytes =
            std::filesystem::file_size(appState.sessionTab.multiWorkspacePath, ec);
        const double sizeMB = ec ? 0.0
                                 : static_cast<double>(fileBytes) / (1024.0 * 1024.0);
        ImGui::Text("%zu dataset%s · %.1f MB",
                    datasets, datasets == 1 ? "" : "s", sizeMB);
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

// One clickable row whose content is pre-wrapped lines drawn manually inside
// the Selectable's rect (Selectable labels never wrap — hidden ##row label).
// The first `titleLines` lines are the title (default color), the rest are
// metadata (dim) — the whole title stays highlighted across wrap lines.
struct WrappedRow {
    bool clicked = false;
    bool hovered = false;
    ImVec2 min = {0.0f, 0.0f};
    ImVec2 max = {0.0f, 0.0f};
    ImVec2 content = {0.0f, 0.0f};   // list child's content origin (bar/text anchor)
};

// Shared left-accent layout for every session-tab list (rows AND the
// experiment-type cards), anchored to each list child's CONTENT ORIGIN.
// Borderless row children have zero padding, so their content origin is the
// child edge; the Selectable's item rect sits FramePadding.x (4px) LEFT of
// it — these offsets preserve the pre-existing row pixels exactly.
// The gap between the bar's right edge and the text is 4px.
constexpr float kRowAccentIndent = 12.0f;
constexpr float kRowTextIndent = 20.0f;

WrappedRow renderWrappedRow(int id, const std::vector<std::string>& lines,
                            int titleLines, float indentX = kRowTextIndent) {
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float padY = 4.0f;
    const float height = padY * 2.0f + static_cast<float>(lines.size()) * lineH;
    WrappedRow out;
    ImGui::PushID(id);
    // Content origin: the bar/text anchor (the Selectable's item rect sits
    // FramePadding.x left of it, so it must NOT be the reference).
    out.content = ImGui::GetCursorScreenPos();
    // The Selectable spans the full row (including the Delete button's right
    // zone) and is submitted BEFORE the button. Without AllowOverlap, its
    // ButtonBehavior captures the mouse-down first and the Delete button is
    // never clickable (clicking it opens the record instead). AllowOverlap
    // makes the later-submitted button win the hit-test.
    ImGui::SetNextItemAllowOverlap();
    out.clicked = ImGui::Selectable("##row", false, 0, ImVec2(0.0f, height));
    out.hovered = ImGui::IsItemHovered();
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    out.min = rmin;
    out.max = rmax;
    // Text drawn via the draw list — SetCursorScreenPos inside/after the
    // Selectable trips ImGui's boundary guard (items never registered there).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
    // Meta lines: dim gray, brightened on row hover — TextDisabled is nearly
    // unreadable on the Selectable's HeaderHovered fill.
    const ImU32 dim = out.hovered
        ? ImGui::GetColorU32(ImVec4(0.80f, 0.80f, 0.80f, 1.0f))
        : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImVec2 pos(out.content.x + indentX, rmin.y + padY);
    for (size_t l = 0; l < lines.size(); ++l) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos,
                    static_cast<int>(l) < titleLines ? normal : dim,
                    lines[l].c_str());
        pos.y += lineH;
    }
    ImGui::PopID();
    return out;
}

// "Delete"-labeled row button, styled like the absorbance curve Delete
// (transparent fill, white outline; bright-red fill + black text on hover,
// darker red while clicked). Placed at the record's right edge, with the
// left accent line (shared kRowAccentIndent; white on hover over the record)
// — absorbance-curve-list look. Renders AFTER the row so the button sits on
// top of the row rect. Returns the button's width so callers can reserve the
// right zone in the wrap width.
float rowDeleteButtonWidth() {
    return ImGui::CalcTextSize("Delete").x +
           ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f;
}

bool renderRowRemoveButton(int id, const WrappedRow& row, bool rowHovered) {
    const ImVec2& rowMin = row.min;
    const ImVec2& rowMax = row.max;
    const float delW = rowDeleteButtonWidth();
    const float delH = ImGui::GetFrameHeight();
    const float padX = 10.0f, padY = 4.0f;
    const ImVec2 saveCursor = ImGui::GetCursorScreenPos();   // below the row
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(rowMax.x - delW - padX, rowMin.y + padY));
    // The text color must be known before Button() renders, so probe the
    // exact rect directly (matches ImGui's own hover hit-test modulo rounding).
    const ImVec2 delMin = ImGui::GetCursorScreenPos();
    const bool delHovered = ImGui::IsMouseHoveringRect(
        delMin, ImVec2(delMin.x + delW, delMin.y + delH));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text,
        delHovered ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    const bool pressed = ImGui::Button("Delete", ImVec2(delW, delH));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    // Vertical accent line on the left, anchored to the row's content origin
    // at the shared kRowAccentIndent (white while hovered) — the same inset
    // every session-tab list uses, so all accent bars look identical.
    const ImU32 lineCol = rowHovered
        ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
        : ImGui::ColorConvertFloat4ToU32(modalAccent());
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(row.content.x + kRowAccentIndent, rowMin.y + 2.0f),
        ImVec2(row.content.x + kRowAccentIndent + 4.0f, rowMax.y - 2.0f),
        lineCol);
    // Restore the cursor below the row (in-bounds) and submit a zero-size
    // Dummy: it clears the SetCursorScreenPos flag (implicitly set above),
    // which otherwise trips ImGui's "extend window/parent boundaries" assert
    // at EndChild when this is the last row (no item follows the restore).
    // The Dummy's ItemSize also yields the original one-ItemSpacing gap.
    ImGui::SetCursorScreenPos(ImVec2(saveCursor.x, rowMax.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return pressed;
}

// "2026-08-15T12:30:45Z" (or any "YYYY-MM-DD hh:mm…" / short form) →
// "2026-08-15 12:30". Display-only; stored archives keep ISO-8601.
std::string prettyTimestamp(const std::string& iso) {
    if (iso.size() < 16) return iso;
    std::string out = iso;
    if (out[10] == 'T') out[10] = ' ';
    return out.substr(0, 16);
}

// "12.3 MB" from bytes; "" when sizeBytes is 0 (unknown).
std::string formatSizeMB(uint64_t sizeBytes) {
    if (sizeBytes == 0) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(sizeBytes) / (1024.0 * 1024.0));
    return buf;
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
            if (appState.active && std::filesystem::is_directory(appState.active->currentDirectory))
                defaultFolder = appState.active->currentDirectory;
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
                    crossOpenProject(appState, path, err);
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

// One experiment-type card (column c) — defined below (file scope, after
// the anonymous namespace), used by renderAvailableExperimentsPanel.
static void renderExperimentTypeCard(const char* title, const char* desc, EnvType type);

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
    renderSessionPanel("Active Experiments", [this]() { renderActiveExperimentsPanel(); });
    renderSessionPanel("Available Experiments", [this]() { renderAvailableExperimentsPanel(); });
}

// Multi-workspace mode: three dockable panels docked DIRECTLY in the main
// dock space — Datasets (left), Active Experiments and Available
// Experiments (stacked right) per the default layout in rebuildDefaultLayout.
void SessionTab::renderMultiWorkspace() {
    renderSessionPanel("Datasets", [this]() {
        renderMultiWorkspaceCard(appState.sessionTab.multiWorkspacePath);
        renderDatasetsPanel();
    });
    renderSessionPanel("Active Experiments", [this]() { renderActiveExperimentsPanel(); });
    renderSessionPanel("Available Experiments", [this]() { renderAvailableExperimentsPanel(); });
}

// (a) embedded datasets: wrapped two-line rows (name + metadata) in a
// scrollable list, [+ Add Dataset] pinned at the bottom.
void SessionTab::renderDatasetsPanel() {
    const float rowH = ImGui::GetFrameHeight();
    ImGui::BeginChild("##datasetsList",
                      ImVec2(0.0f, -(rowH * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f)),
                      false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (appState.sessionTab.sources.empty()) {
        renderCenteredEmptyLine("No datasets embedded yet.", 0.25f);
        renderCenteredEmptyLine("Use \"+ Add Dataset\" below to embed one.", 0.05f);
    }
    for (size_t i = 0; i < appState.sessionTab.sources.size(); ++i) {
        const auto& src = appState.sessionTab.sources[i];

        // Right zone: Delete button + accent line; wrap text must stop short.
        const float availW = ImGui::GetContentRegionAvail().x - 10.0f -
                             rowDeleteButtonWidth() - 14.0f;
        const std::vector<std::string> nameLines =
            wrapToLines(src.name, availW, 2);
        std::string meta =
            std::to_string(src.memberCount) + " measurement" +
            (src.memberCount == 1 ? "" : "s") + " · created " +
            prettyTimestamp(src.createdIso);
        const std::string sizeMB = formatSizeMB(src.sizeBytes);
        if (!sizeMB.empty()) meta += " · " + sizeMB;
        std::vector<std::string> lines = nameLines;
        for (auto& l : wrapToLines(meta, availW, 2)) lines.push_back(std::move(l));
        const WrappedRow row =
            renderWrappedRow(static_cast<int>(i), lines,
                             static_cast<int>(nameLines.size()), kRowTextIndent);
        if (renderRowRemoveButton(static_cast<int>(i), row, row.hovered)) {
            g_removeSourceId = src.id;
            g_showRemoveConfirm = true;
            appState.needsRedraw = true;
        }
        if (row.clicked) {
            openEmbeddedInNewTab(appState, appState.sessionTab.multiWorkspacePath,
                                 src.id);
        }
    }
    ImGui::EndChild();
    renderAddDatasetButton();
}

// (b) active experiments: live instances (Phase 3). Rows match the Datasets
// list style (right-aligned Delete + wrapped title/metadata, accent dot when
// active). The comment (if any) is shown grey below the name. Click →
// activate tab.
void SessionTab::renderActiveExperimentsPanel() {
    ImGui::BeginChild("##activeEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (appState.experiments.empty()) {
        renderCenteredEmptyLine("No experiments open.", 0.45f);
    }
    for (size_t i = 0; i < appState.experiments.size(); ++i) {
        auto* env = appState.experiments[i].get();

        // Right zone: Delete button + accent line; wrap text must stop short.
        const float availW = ImGui::GetContentRegionAvail().x - 10.0f -
                             rowDeleteButtonWidth() - 14.0f;
        const std::vector<std::string> titleLines =
            wrapToLines(env->tabLabel(), availW, 2);
        // Absorbance: picked samples. Comparator: the artifact type. Type
        // first, then the count (Absorbance) or artifact (Comparator), then
        // the persisted archive size (Absorbance only — comparator archives
        // hold no per-experiment results).
        std::string meta = std::string(experimentTypeName(env->type)) + " · ";
        if (env->type == EnvType::Absorbance) {
            meta += std::to_string(env->curves.size()) + " curve" +
                    (env->curves.size() == 1 ? "" : "s");
        } else {
            meta += artifactLabel(static_cast<ComparatorArtifact>(env->artifactSelector));
        }
        const std::string sizeMB = formatSizeMB(env->sizeBytes);
        if (!sizeMB.empty() && env->type == EnvType::Absorbance)
            meta += " · " + sizeMB;
        std::vector<std::string> lines = titleLines;
        for (auto& l : wrapToLines(meta, availW, 1)) lines.push_back(std::move(l));
        if (!env->comment.empty())
            for (auto& l : wrapToLines(env->comment, availW, 2))
                lines.push_back(std::move(l));
        const WrappedRow row =
            renderWrappedRow(static_cast<int>(i), lines,
                             static_cast<int>(titleLines.size()), kRowTextIndent);
        if (renderRowRemoveButton(static_cast<int>(i), row, row.hovered)) {
            // Deletion happens HERE (the tab selector's close only
            // deactivates). Dirty/persisted instances confirm via the modal.
            env->requestDelete();
            break;   // experiments vector changed
        }
        if (row.clicked) {
            activateExperiment(appState, static_cast<int>(i));
        }
    }
    ImGui::EndChild();
}

// (c) available experiment types: clickable cards → create instance (Phase 3).
void SessionTab::renderAvailableExperimentsPanel() {
    ImGui::BeginChild("##availableEnvsList", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    renderExperimentTypeCard("Absorbance",
                      "Calculate absorbance and transmission spectra.",
                      EnvType::Absorbance);
    renderExperimentTypeCard("Comparator", "Compare information from multiple datasets",
                      EnvType::Comparator);
    ImGui::EndChild();
}

// One experiment-type card: accent-framed, clickable — creates a new
// instance (auto-name, activated).
static void renderExperimentTypeCard(const char* title, const char* desc, EnvType type) {
    const AccentColor ac = StringToAccentColor(appState.currentAccentColor);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    // Zero X padding: the row lists' children are borderless (padding 0), so
    // this child's content origin must coincide with theirs for the shared
    // content-anchored accent offsets to land on the same pixels.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, GetAccentSubtle(ac));
    // First row: title + 2x-size "+ New" (drawn via the draw list — no second
    // font is loaded; AddText scales the default font's glyphs). The band is
    // tall enough for the big label; the description starts below it.
    const float bigH = ImGui::GetFontSize() * 1.5f;
    const bool open = ImGui::BeginChild(title,
        ImVec2(0.0f, 8.0f + bigH + 6.0f + ImGui::GetTextLineHeightWithSpacing() + 8.0f), true);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open) {
        const ImVec2 wpos = ImGui::GetWindowPos();
        const ImVec2 wsize = ImGui::GetWindowSize();
        // Content origin (zero X padding → == child edge): the shared anchor
        // for the accent line and text offsets.
        const ImVec2 contentMin = ImGui::GetCursorScreenPos();
        // Hover highlight matching the Active Experiments rows (Selectable
        // hover = ImGuiCol_HeaderHovered). Child windows share the parent's
        // draw list, so the fill lands under the text, inside the child rect.
        const bool hovered = ImGui::IsWindowHovered();
        if (hovered) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                ImGui::ColorConvertFloat4ToU32(GetAccentHovered(ac)), 6.0f);
        }
        // Vertical accent line (white on hover) at the shared content-origin
        // inset (kRowAccentIndent) — same position as every row list.
        {
            const ImU32 lineCol = hovered
                ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
                : ImGui::ColorConvertFloat4ToU32(modalAccent());
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(contentMin.x + kRowAccentIndent, wpos.y + 2.0f),
                ImVec2(contentMin.x + kRowAccentIndent + 4.0f,
                       wpos.y + wsize.y - 2.0f),
                lineCol);
        }
        // Title (and description below) start at the shared text indent,
        // matching the row lists. Indent() (not SetCursorPosX): ItemSize
        // resets the cursor X to the window's line start after every item,
        // which would throw the description back to the left edge.
        ImGui::Indent(kRowTextIndent);
        ImGui::TextUnformatted(title);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const char* newLabel = "+ New";
            const float newW = ImGui::CalcTextSize(newLabel).x * 1.5f;
            dl->AddText(ImGui::GetFont(), bigH,
                        ImVec2(wpos.x + ImGui::GetWindowWidth() -
                                   ImGui::GetStyle().WindowPadding.x - newW,
                               wpos.y + 8.0f),
                        ImGui::ColorConvertFloat4ToU32(modalAccent()), newLabel);
        }
        ImGui::SetCursorPosY(8.0f + bigH + 6.0f);
        // Description: dim gray, brightened on hover (TextDisabled is nearly
        // unreadable on the accent hover fill).
        ImGui::PushStyleColor(ImGuiCol_Text, hovered
            ? ImVec4(0.80f, 0.80f, 0.80f, 1.0f)
            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
        ImGui::TextWrapped("%s", desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(kRowTextIndent);
    }
    ImGui::EndChild();
    if (ImGui::IsItemClicked()) {
        createExperiment(appState, type);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create a new %s experiment.", title);
    ImGui::Spacing();
}
