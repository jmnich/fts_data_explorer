#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "welcome.h"
#include "app_state.h"
#include "config.h"
#include "file_browser.h"
#include "theme.h"

#include "imgui.h"
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
        {0.03f, 0.02f, 0.18f, 60},
        {0.28f, 0.01f, 0.12f, 55},
        {0.52f, 0.04f, 0.22f, 65},
        {0.72f, 0.02f, 0.15f, 50},
        {0.02f, 0.20f, 0.24f, 70},
        {0.32f, 0.22f, 0.10f, 55},
        {0.55f, 0.18f, 0.18f, 65},
        {0.80f, 0.22f, 0.20f, 60},
        {0.05f, 0.40f, 0.14f, 60},
        {0.30f, 0.42f, 0.20f, 55},
        {0.50f, 0.38f, 0.16f, 70},
        {0.75f, 0.42f, 0.22f, 60},
        {0.08f, 0.58f, 0.20f, 60},
        {0.35f, 0.60f, 0.14f, 65},
        {0.55f, 0.56f, 0.22f, 55},
        {0.78f, 0.58f, 0.12f, 60},
        {0.05f, 0.90f, 0.18f, 70},
        {0.30f, 0.74f, 0.24f, 60},
        {0.55f, 0.78f, 0.12f, 55},
        {0.78f, 0.86f, 0.18f, 65},
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
static void drawWelcomeBackgroundScatter(ImDrawList* drawList, const ImVec2& vpSize) {
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
            IM_COL32(255, 255, 255, copy.alpha));
    }
}

void renderWelcomeScreen(AppState& appState, AppConfig& config,
                         const std::string& configFilePath) {
    // Draw decorative background scatter on viewport background draw list (full screen, every frame)
    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    drawWelcomeBackgroundScatter(ImGui::GetBackgroundDrawList(), vpSize);

    // Center the welcome screen
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(1200, 800));

    // Disable the modal dimmer overlay (which fades in over ~10 frames) so the decorative
    // background pattern behind the popup stays fully visible.
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));

    // Create a modal popup that blocks all interaction
    ImGui::OpenPopup("Welcome to FTS Data Explorer");

    if (ImGui::BeginPopupModal("Welcome to FTS Data Explorer", NULL,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoBackground)) {

        // Draw semi-transparent dark background on window draw list
        ImDrawList* winDrawList = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        winDrawList->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + winSize.y),
                                   IM_COL32(0, 0, 0, 180));

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
                for (const auto& datasetPath : config.recentDatasets) {
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

                    if (ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0))) {
                        if (std::filesystem::exists(datasetPath) && std::filesystem::is_directory(datasetPath)) {
                            std::string rawDataPath = datasetPath + "/raw_data";
                            if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                                appState.currentDirectory = rawDataPath;
                            } else {
                                appState.currentDirectory = datasetPath;
                            }

                            appState.currentDatasetName = datasetPath.substr(datasetPath.find_last_of("/\\") + 1);

                            appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                            appState.clearAverageSpectrum();
                            appState.clearSnrSpectrum();
                            appState.dataLoaded = false;
                            appState.filesChanged = true;
                            appState.currentSortedFileIndex = 0;
                            appState.showWelcomeScreen = false;
                            appState.welcomeScreenInitialized = true;
                            appState.isFirstDataLoad = true;
                            appState.needsRedraw = true;
                            addToRecentDatasets(config, configFilePath, datasetPath);
                            std::cout << "Opened recent dataset: " << datasetPath << std::endl;
                            ImGui::CloseCurrentPopup();
                        } else {
                            std::cerr << "Recent dataset path no longer exists: " << datasetPath << std::endl;
                        }
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", datasetPath.c_str());
                    }
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

        // Directory selection button - use accent color
        AccentColor accent = StringToAccentColor(appState.currentAccentColor);
        ImGui::PushStyleColor(ImGuiCol_Button, GetAccentMuted(accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetAccentHovered(accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetAccentActive(accent));

        float buttonHeight = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y * 2;
        if (buttonHeight > 60.0f) buttonHeight = 60.0f;
        bool buttonClicked = ImGui::Button("Select Dataset Directory", ImVec2(-FLT_MIN, buttonHeight));
        ImGui::PopStyleColor(3);

        if (buttonClicked) {
            std::string selectedDirectory = FileBrowser::showDirectorySelectionDialog();
            if (!selectedDirectory.empty()) {
                std::string rawDataPath = selectedDirectory + "/raw_data";
                if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                    appState.currentDirectory = rawDataPath;
                    appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                } else {
                    appState.currentDirectory = selectedDirectory;
                    appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                }
                appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                appState.clearAverageSpectrum();
                appState.clearSnrSpectrum();
                appState.dataLoaded = false;
                appState.filesChanged = true;
                appState.currentSortedFileIndex = 0;
                appState.showWelcomeScreen = false;
                appState.welcomeScreenInitialized = true;
                appState.needsRedraw = true;
                addToRecentDatasets(config, configFilePath, selectedDirectory);
                std::cout << "Working directory set to: " << appState.currentDirectory << std::endl;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();

        if (!appState.showWelcomeScreen) {
            appState.welcomeScreenInitialized = true;
        }
    }
    ImGui::PopStyleColor(); // ModalWindowDimBg
}
