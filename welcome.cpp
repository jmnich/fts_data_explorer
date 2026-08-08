#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "welcome.h"
#include "app_state.h"
#include "config.h"
#include "file_browser.h"
#include "theme.h"
#include "version.h"

#include "imgui.h"
#include <GLFW/glfw3.h>
#include <GL/gl.h>

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

    // Center the welcome screen
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1200, 800));

    // Disable the modal dimmer overlay (which fades in over ~10 frames) so the decorative
    // background pattern behind the popup stays fully visible.
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));

    // Create a modal popup that blocks all interaction
    {
        std::string popupTitle = std::string("Welcome to FTS Data Explorer ") + APP_VERSION;
        ImGui::OpenPopup(popupTitle.c_str());

        if (ImGui::BeginPopupModal(popupTitle.c_str(), NULL,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoBackground)) {

        // Draw semi-transparent dark background on window draw list
        ImDrawList* winDrawList = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        winDrawList->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y),
                                   IM_COL32(0, 0, 0, 100));

        ImGui::Spacing();

        // Recent datasets section
        ImGui::Text("Recent Datasets:");
        ImGui::Spacing();

        // Always use a fixed-height child region
        if (ImGui::BeginChild("RecentDatasetsChild", ImVec2(0, 500), true)) {
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
                            if (!appState.showUnsavedPrompt && !appState.showStaleDropPrompt) {
                                ImGui::CloseCurrentPopup();
                                appState.needsRedraw = true;
                            }
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

        // Primary action: open an .h5 workspace directly (accent-styled).
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
                if (!appState.showUnsavedPrompt && !appState.showStaleDropPrompt) {
                    ImGui::CloseCurrentPopup();
                    appState.needsRedraw = true;
                }
            }
        }
#endif

        ImGui::Spacing();
        // Secondary action: convert foreign formats (legacy/non-.h5 datasets
        // enter via the Conversion screen — phase5 decision 6).
        if (ImGui::Button("Convert Dataset", ImVec2(-FLT_MIN, 0))) {
            openConversionScreen(appState);
        }

        ImGui::EndPopup();

        if (!appState.showWelcomeScreen) {
            appState.welcomeScreenInitialized = true;
        }
    }
    }
    ImGui::PopStyleColor(); // ModalWindowDimBg
}
