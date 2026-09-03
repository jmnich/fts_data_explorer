#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "welcome.h"
#include "app_state.h"
#include "config.h"
#include "file_browser.h"
#include "theme.h"
#include "version.h"
#include "session/cross_store.h"
#include "session/workspace_session.h"

#include "imgui.h"
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>

// xxd-generated header from assets/interferogram_curve.png
#include "interferogram_curve.h"

struct ScatterCopy {
    float vpX, vpY;      // position as fraction of viewport (0-1)
    float scale;          // size multiplier relative to original image
    int alpha;            // opacity (0-255)
};

struct WelcomeBg {
    GLuint textureID = 0;
    int width = 0;
    int height = 0;
    std::vector<ScatterCopy> scatterCopies;
};
static WelcomeBg g_bg;

void initWelcomeBackground() {
    int w, h, n;
    unsigned char* rgba = stbi_load_from_memory(
        assets_interferogram_curve_png,
        assets_interferogram_curve_png_len,
        &w, &h, &n, 4);
    if (!rgba) {
        std::cerr << "Failed to decode interferogram curve PNG" << std::endl;
        return;
    }

    // Replace all non-transparent pixel RGB with white so the tint color
    // (accent-colored via IM_COL32) completely controls the pattern appearance.
    int totalPixels = w * h;
    for (int i = 0; i < totalPixels; i++) {
        int idx = i * 4;
        if (rgba[idx + 3] > 0) {
            rgba[idx] = 255;
            rgba[idx + 1] = 255;
            rgba[idx + 2] = 255;
        }
    }

    glGenTextures(1, &g_bg.textureID);
    glBindTexture(GL_TEXTURE_2D, g_bg.textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(rgba);

    g_bg.width = w;
    g_bg.height = h;

    // ── Scatter copy layout ────────────────────────────────────────────────
    // Each entry: { viewportX (0-1), viewportY (0-1), scale, alpha }
    // (vpX, vpY) is the top-left corner as a fraction of the viewport size.
    // scale is relative to the original 2400×900 texture (0.10 ≈ 240×90 px).
    // Alpha values reduced to 30-80 for subtle decorative effect.
    g_bg.scatterCopies = {
        {0.03f, 0.02f, 0.18f, 90},
        {0.28f, 0.01f, 0.12f, 83},
        {0.52f, 0.04f, 0.22f, 98},
        {0.72f, 0.02f, 0.15f, 75},
        {0.02f, 0.20f, 0.24f, 105},
        {0.32f, 0.22f, 0.10f, 83},
        {0.55f, 0.18f, 0.18f, 98},
        {0.80f, 0.22f, 0.20f, 90},
        {0.05f, 0.40f, 0.14f, 90},
        {0.30f, 0.42f, 0.20f, 83},
        {0.50f, 0.38f, 0.16f, 105},
        {0.75f, 0.42f, 0.22f, 90},
        {0.08f, 0.58f, 0.20f, 90},
        {0.35f, 0.60f, 0.14f, 98},
        {0.55f, 0.56f, 0.22f, 83},
        {0.78f, 0.58f, 0.12f, 90},
        {0.05f, 0.90f, 0.18f, 105},
        {0.30f, 0.74f, 0.24f, 90},
        {0.55f, 0.78f, 0.12f, 83},
        {0.78f, 0.86f, 0.18f, 98},
    };

    std::cout << "Welcome background texture loaded (" << w << "x" << h
              << ") with " << g_bg.scatterCopies.size() << " scatter copies" << std::endl;
}

void destroyWelcomeBackground() {
    if (g_bg.textureID) {
        glDeleteTextures(1, &g_bg.textureID);
        g_bg.textureID = 0;
    }
}

void addToRecentDatasets(AppConfig& config, const std::string& configFilePath,
                         const std::string& datasetPath) {
    config.addRecentDataset(datasetPath);
    config.saveToFile(configFilePath);
}

// Draw scattered copies of the decorative curve on the viewport background (full screen)
static void drawWelcomeBackgroundScatter(ImDrawList* drawList, const ImVec2& vpSize,
                                         const ImVec4& accentColor) {
    if (!g_bg.textureID) return;

    const float baseW = static_cast<float>(g_bg.width);
    const float baseH = static_cast<float>(g_bg.height);

    for (const auto& copy : g_bg.scatterCopies) {
        float w = baseW * copy.scale;
        float h = baseH * copy.scale;
        float x = copy.vpX * vpSize.x;
        float y = copy.vpY * vpSize.y;

        drawList->AddImage(
            (ImTextureID)g_bg.textureID,
            ImVec2(x, y),
            ImVec2(x + w, y + h),
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(
                (int)(accentColor.x * 255.0f),
                (int)(accentColor.y * 255.0f),
                (int)(accentColor.z * 255.0f),
                copy.alpha));
    }
}

// Truncate text to fit maxTextWidth px, keeping equal-length head and tail
// portions separated by "...". Only for the welcome-screen list rows.
static std::string truncateToWidth(const std::string& text, float maxTextWidth) {
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= maxTextWidth)
        return text;
    const char* ellipsis = "...";
    const float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    const float innerW = maxTextWidth - ellipsisW;
    if (innerW <= 0.0f) return ellipsis;
    const size_t half = text.length() / 2;
    size_t lo = 0, hi = half;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        const std::string candidate = text.substr(0, mid) + text.substr(text.length() - mid);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= innerW)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo == 0) return ellipsis;
    return text.substr(0, lo) + ellipsis + text.substr(text.length() - lo);
}

void renderWelcomeScreen(AppState& appState, AppConfig& config,
                         const std::string& configFilePath, bool showPopup) {
    // Keep the recent list clean: drop stale/non-.h5 entries (files deleted on
    // disk, legacy dataset directories) and persist when anything changed.
    if (config.pruneRecentToH5()) {
        config.saveToFile(configFilePath);
    }

    // Draw decorative background scatter on viewport background draw list (full screen, every frame)
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    AccentColor accentScatter = StringToAccentColor(appState.currentAccentColor);
    ImVec4 scatterColor = GetAccentBase(accentScatter);
    float scatterMax = std::max({scatterColor.x, scatterColor.y, scatterColor.z});
    if (scatterMax > 0.0f) {
        float inv = 1.0f / scatterMax;
        scatterColor.x *= inv;
        scatterColor.y *= inv;
        scatterColor.z *= inv;
    }
    drawWelcomeBackgroundScatter(ImGui::GetBackgroundDrawList(), vpSize, scatterColor);

    if (!showPopup) return;

    // Launch-only welcome (M2.6): a plain two-column window — left = recent
    // single-dataset workspaces + converter, right = recent multi-workspace
    // files + New Multi-Workspace. Not a modal: the Session tab takes over
    // after the first open/create and is never closable afterwards.
    // Size tracks the app window the same way the Convert modal does
    // (conversion_screen.cpp): a proportional share of the work viewport with
    // clamped bounds, re-applied every frame so it scales with the viewport
    // and the UI size setting instead of hitting fixed pixel caps. The height
    // is 1/4 shorter than the Convert modal.
    const char* title = "Welcome to FTS Data Explorer";
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    const float winW = std::clamp(work.x * 0.85f, 720.0f, 2000.0f);
    const float winH = std::clamp(work.y * 0.85f, 700.0f, 1600.0f) * 0.75f;
    const float listChildH = std::max(100.0f, winH - 340.0f);
    const float crossChildH = std::max(100.0f, winH - 160.0f);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 525.0f),
                                        ImVec2(2000.0f, 1200.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.09f, 0.12f, 0.96f));
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);

    // Semi-transparent dark fill (matches the old popup look).
    ImDrawList* winDl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    winDl->AddRectFilled(winPos, ImVec2(winPos.x + winW, winPos.y + winH),
                         IM_COL32(0, 0, 0, 100));

    ImGui::TextColored(GetAccentBase(StringToAccentColor(appState.currentAccentColor)),
                       "%s %s", title, APP_VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Two side-by-side child windows (NOT ImGui::Columns — columns corrupt
    // their cursor when a BeginChild runs inside one, which pushed the right
    // column down to the left column's cursor and broke the whole layout).
    const float colGap = ImGui::GetStyle().ItemSpacing.x;
    const float colW = (ImGui::GetContentRegionAvail().x - colGap) * 0.5f;
    const float colH = winH - 55.0f;

    // ── Left: recent single-dataset workspaces + converter ─────────────────
    ImGui::BeginChild("##welcomeLeft", ImVec2(colW, colH), true);
    ImGui::Text("Recent Datasets");
    ImGui::Separator();
    if (ImGui::BeginChild("RecentDatasetsChild", ImVec2(0, listChildH), true)) {
        if (config.recentDatasets.empty()) {
            float childHeight = ImGui::GetContentRegionAvail().y;
            float textHeight = ImGui::GetTextLineHeightWithSpacing() * 3;
            float offsetY = (childHeight - textHeight) * 0.5f;
            if (offsetY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

            ImGui::Text("No recent datasets found.");
            ImGui::Text("Use the button below to select a dataset directory.");
        } else {
            static int selectedIdx = 0;
            if (selectedIdx >= (int)config.recentDatasets.size())
                selectedIdx = config.recentDatasets.empty() ? 0 : (int)config.recentDatasets.size() - 1;

            // Arrow key navigation
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                int next = selectedIdx - 1;
                if (next >= 0) selectedIdx = next;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                int next = selectedIdx + 1;
                if (next < (int)config.recentDatasets.size()) selectedIdx = next;
            }

            for (size_t i = 0; i < config.recentDatasets.size(); ) {
                const auto& entry = config.recentDatasets[i];
                const auto& datasetPath = entry.path;
                bool exists = std::filesystem::exists(datasetPath)
                    || std::filesystem::is_directory(datasetPath);
#if FTS_BUILD_HDF5
                bool isH5 = std::filesystem::path(datasetPath).extension() == ".h5";
#endif

                std::string displayName = datasetPath;
                size_t last_slash = displayName.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    displayName = displayName.substr(last_slash + 1);
                    if (displayName == "raw_data" && last_slash > 0) {
                        size_t parent_slash = datasetPath.substr(0, last_slash).find_last_of("/\\");
                        if (parent_slash != std::string::npos) {
                            displayName = datasetPath.substr(parent_slash + 1, last_slash - parent_slash - 1);
                        }
                    }
                }

                ImGui::PushID(datasetPath.c_str());

                float btnH = ImGui::GetFrameHeight();

                // Highlight selected row
                if ((int)i == selectedIdx) {
                    ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    float rowH = ImGui::GetFrameHeight();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        rowMin,
                        ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + rowH),
                        IM_COL32(80, 80, 200, 120));
                }

                // Cross (×) button — delete from recent list (always active)
                if (ImGui::Button("×", ImVec2(btnH, btnH))) {
                    config.recentDatasets.erase(config.recentDatasets.begin() + i);
                    config.saveToFile(configFilePath);
                    if (selectedIdx > (int)i) selectedIdx--;
                    ImGui::PopID();
                    continue;
                }
                ImGui::SameLine();

                {
                    float availW = ImGui::GetContentRegionAvail().x;
                    float maxTextW = availW - 2.0f * ImGui::GetStyle().FramePadding.x;
                    if (!exists)
                        maxTextW -= ImGui::CalcTextSize("(unreachable)").x +
                                    ImGui::GetStyle().ItemInnerSpacing.x;
                    displayName = truncateToWidth(displayName, maxTextW);
                }

                if (!exists) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::BeginDisabled(true);
                }

                // Dataset name button (mouse click or Enter on selected row)
                bool shouldOpen = false;
                if (ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0))) {
                    shouldOpen = true;
                }
                if (!shouldOpen && exists && (int)i == selectedIdx && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    shouldOpen = true;
                }
                if (shouldOpen) {
#if FTS_BUILD_HDF5
                    if (isH5) {
                        requestWorkspaceDiscard(appState, PendingWorkspaceAction::OpenPath, datasetPath);
                        appState.needsRedraw = true;
                    }
                    // Non-.h5 entries are pruned on render; any forced open
                    // of such a path fails silently in openWorkspace().
#endif
                }

                if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Path not reachable: %s", datasetPath.c_str());
                }

                if (!exists) {
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor(4);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(unreachable)");
                } else {
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", datasetPath.c_str());
                    }
                }

                ImGui::PopID();
                i++;
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // UI Size selection dropdown
    ImGui::Text("UI Size:");
    if (ImGui::BeginCombo("##UISizeCombo", appState.currentUiSize.c_str())) {
        if (ImGui::Selectable("tiny", appState.currentUiSize == "tiny")) {
            appState.currentUiSize = "tiny";
            appState.uiSizeChanged = true;
        }
        if (ImGui::Selectable("small", appState.currentUiSize == "small")) {
            appState.currentUiSize = "small";
            appState.uiSizeChanged = true;
        }
        if (ImGui::Selectable("normal", appState.currentUiSize == "normal")) {
            appState.currentUiSize = "normal";
            appState.uiSizeChanged = true;
        }
        if (ImGui::Selectable("large", appState.currentUiSize == "large")) {
            appState.currentUiSize = "large";
            appState.uiSizeChanged = true;
        }
        if (ImGui::Selectable("huge", appState.currentUiSize == "huge")) {
            appState.currentUiSize = "huge";
            appState.uiSizeChanged = true;
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

#if FTS_BUILD_HDF5
    AccentColor accent = StringToAccentColor(appState.currentAccentColor);
    ImVec4 btnBg = GetAccentMuted(accent);
    btnBg.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, btnBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetAccentActive(accent));

    float buttonHeight = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y * 2;
    if (buttonHeight > 60.0f) buttonHeight = 60.0f;
    bool openClicked = ImGui::Button("Open .h5", ImVec2(-FLT_MIN, buttonHeight));
    ImGui::PopStyleColor(3);

    if (openClicked) {
        std::string defaultFolder;
        if (std::filesystem::is_directory(config.lastWorkingDirectory))
            defaultFolder = config.lastWorkingDirectory;
        std::string path = FileBrowser::showFileOpenDialog(
            "Open HDF5 Workspace", "HDF5 files", "*.h5",
            glfwGetCurrentContext(), defaultFolder);
        if (!path.empty()) {
            requestWorkspaceDiscard(appState, PendingWorkspaceAction::OpenPath, path);
            appState.needsRedraw = true;
        }
    }
#endif

    ImGui::Spacing();
    // Secondary action: convert foreign formats (legacy/non-.h5 datasets
    // enter via the Conversion screen — phase5 decision 6).
    if (ImGui::Button("Convert Dataset", ImVec2(-FLT_MIN, 0))) {
        openConversionScreen(appState);
    }

    ImGui::EndChild();   // ##welcomeLeft
    ImGui::SameLine();

    // ── Right: recent multi-workspace files + New Multi-Workspace ──────────
    ImGui::BeginChild("##welcomeRight", ImVec2(colW, colH), true);
    ImGui::Text("Recent Multi-Workspaces");
    ImGui::Separator();
    if (ImGui::BeginChild("RecentCrossChild", ImVec2(0, crossChildH), true)) {
        if (config.recentMultiWorkspaces.empty()) {            float childHeight = ImGui::GetContentRegionAvail().y;
            float textHeight = ImGui::GetTextLineHeightWithSpacing() * 3;
            float offsetY = (childHeight - textHeight) * 0.5f;
            if (offsetY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);
            ImGui::Text("No multi-workspaces yet.");
            ImGui::Text("Create one below, or embed datasets later from the Session tab.");
        } else {
            for (size_t i = 0; i < config.recentMultiWorkspaces.size(); ) {
                const auto& path = config.recentMultiWorkspaces[i];
                bool exists = std::filesystem::exists(path);
                ImGui::PushID(path.c_str());
                float btnH = ImGui::GetFrameHeight();
                if (ImGui::Button("×", ImVec2(btnH, btnH))) {
                    config.recentMultiWorkspaces.erase(config.recentMultiWorkspaces.begin() + i);
                    config.saveToFile(configFilePath);
                    ImGui::PopID();
                    continue;
                }
                ImGui::SameLine();
                std::string displayName = std::filesystem::path(path).filename().string();
                {
                    float availW = ImGui::GetContentRegionAvail().x;
                    float maxTextW = availW - 2.0f * ImGui::GetStyle().FramePadding.x;
                    if (!exists)
                        maxTextW -= ImGui::CalcTextSize("(unreachable)").x +
                                    ImGui::GetStyle().ItemInnerSpacing.x;
                    displayName = truncateToWidth(displayName, maxTextW);
                }
                if (exists) {
                    if (ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0))) {
                        requestWorkspaceDiscard(appState, PendingWorkspaceAction::OpenPath, path);
                        appState.needsRedraw = true;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", path.c_str());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::BeginDisabled(true);
                    ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0));
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(unreachable)");
                }
                ImGui::PopID();
                i++;
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // [New Multi-Workspace…]: empty multi-workspace .h5; only the Session tab
    // exists — start populating via its column (a).
    if (ImGui::Button("New Multi-Workspace...", ImVec2(-FLT_MIN, 0))) {
        std::string defaultFolder;
        if (std::filesystem::is_directory(config.lastWorkingDirectory))
            defaultFolder = config.lastWorkingDirectory;
        else if (!config.lastMultiWorkspacePath.empty())
            defaultFolder = std::filesystem::path(config.lastMultiWorkspacePath).parent_path().string();
        std::string path = FileBrowser::showFileSaveDialog(
            "New Multi-Workspace", "HDF5 files", "*.h5",
            defaultFolder, "workspace.h5", glfwGetCurrentContext());
        if (!path.empty()) {
            ensureSessionTab(appState);
            std::string err;
            if (crossCreate(path, err)) {
                if (crossOpenProject(appState, path, err)) {
                    focusSessionTab(appState);
                    config.lastMultiWorkspacePath = path;
                    config.addRecentMultiWorkspace(path);
                    config.saveToFile(configFilePath);
                    appState.showWelcomeScreen = false;
                    appState.welcomeScreenInitialized = true;
                }
            }
            if (!err.empty()) {
                appState.adapterErrorMsg = "Create failed:\n" + err;
                appState.showAdapterErrorPopup = true;
            }
            appState.needsRedraw = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Creates an empty multi-workspace .h5. Datasets are embedded from "
                          "the Session tab's column (a).");

    ImGui::EndChild();   // ##welcomeRight
    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
}
