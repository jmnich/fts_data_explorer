// Main menu bar (extracted from main.cpp, Phase-1 M1.2c).
#include "menu_bar.h"
#include "app_state.h"
#include "config.h"
#include "conversion_screen.h"
#include "about.h"
#include "theme.h"
#include "file_browser.h"
#include "app_dirs.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <filesystem>

// Main menu bar (docked above the DockSpace). Reads the global appState;
// needs the config + window locals for open/save dialogs.
void renderMainMenuBar(AppConfig& config, const std::string& configFilePath,
                       GLFWwindow* window) {
            if (ImGui::BeginMainMenuBar())
            {
                // File menu
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Convert Dataset...")) {
                        openConversionScreen(appState);
                    }
#if FTS_BUILD_HDF5
                    if (ImGui::MenuItem("Open Workspace (.h5)...")) {
                        std::string defaultFolder = appState.active
                            ? appState.active->currentDirectory : std::string();
                        if (appState.hasWorkspace())
                            defaultFolder = std::filesystem::path(appState.active->workspacePath).parent_path().string();
                        if (!std::filesystem::is_directory(defaultFolder))
                            defaultFolder = std::filesystem::is_directory(config.lastWorkingDirectory)
                                ? config.lastWorkingDirectory : "";
                        std::string path = FileBrowser::showFileOpenDialog(
                            "Open HDF5 Workspace", "HDF5 files", "*.h5",
                            window, defaultFolder);
                        if (!path.empty())
                            requestWorkspaceDiscard(appState, PendingWorkspaceAction::OpenPath, path);
                    }
                    if (ImGui::MenuItem("Close Workspace", nullptr, false, appState.hasWorkspace())) {
                        requestWorkspaceDiscard(appState, PendingWorkspaceAction::CloseWorkspace, "");
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Save", "Ctrl+S", false,
                                        appState.hasWorkspace() ||
                                        appState.sessionTab.multiWorkspaceOpen)) {
                        // Deferred manual save: "Saving..." overlay this frame,
                        // sync save at the next frame top, then the "Saved" toast.
                        // Errors surface via the executor's "Save failed:" popup
                        // (no throw from the request itself).
                        requestSaveEverything(appState);
                    }
                    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, appState.hasWorkspace())) {
                        try {
                            saveWorkspaceAs(appState, window);
                        } catch (const std::exception& e) {
                            appState.adapterErrorMsg = std::string("Save failed:\n") + e.what();
                            appState.showAdapterErrorPopup = true;
                        }
                    }
#endif
                    
                    // Recent datasets menu
                    if (config.recentDatasets.empty()) {
                        // no recent datasets to show
                    } else {
                        if (ImGui::BeginMenu("Recent Datasets")) {
                            for (const auto& entry : config.recentDatasets) {
                                const auto& datasetPath = entry.path;
                                bool exists = std::filesystem::exists(datasetPath)
                                    || std::filesystem::is_directory(datasetPath);

                                if (!exists) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                                    ImGui::BeginDisabled(true);
                                }

                                bool clicked = ImGui::MenuItem(datasetPath.c_str());

                                if (!exists) {
                                    ImGui::EndDisabled();
                                    ImGui::PopStyleColor();
                                }

                                if (clicked && exists) {
#if FTS_BUILD_HDF5
                                    if (std::filesystem::path(datasetPath).extension() == ".h5") {
                                        requestWorkspaceDiscard(appState, PendingWorkspaceAction::OpenPath, datasetPath);
                                    }
                                    // Non-.h5 entries are pruned at startup; any
                                    // forced open fails silently in openWorkspace().
#endif
                                }
                            }
                            ImGui::EndMenu();
                        }
                    }
                    
                    ImGui::EndMenu();
                }
                
                // Settings menu
                if (ImGui::BeginMenu("Settings"))
                {
                    // Display FPS toggle
                    ImGui::MenuItem("Display fps", NULL, &appState.showFPS);
                    ImGui::MenuItem("Show timestamps", NULL, &appState.showTimestamps);
                    ImGui::Text("Grid opacity");
                    ImGui::SameLine();
                    float gridPct = appState.gridAlpha * 100.0f;
                    if (ImGui::SliderFloat("##gridOpacity", &gridPct, 0.0f, 100.0f, "%.0f%%")) {
                        appState.gridAlpha = gridPct / 100.0f;
                    }

                    ImGui::Separator();
                    if (ImGui::MenuItem("Restore layout")) {
                        appState.restoreLayoutRequested = true;
                    }
                    
                    // UI Size selection dropdown
                    if (ImGui::BeginMenu("UI Size"))
                    {
                        if (ImGui::MenuItem("tiny", NULL, appState.currentUiSize == "tiny")) {
                            appState.currentUiSize = "tiny";
                            appState.uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("small", NULL, appState.currentUiSize == "small")) {
                            appState.currentUiSize = "small";
                            appState.uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("normal", NULL, appState.currentUiSize == "normal")) {
                            appState.currentUiSize = "normal";
                            appState.uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("large", NULL, appState.currentUiSize == "large")) {
                            appState.currentUiSize = "large";
                            appState.uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("huge", NULL, appState.currentUiSize == "huge")) {
                            appState.currentUiSize = "huge";
                            appState.uiSizeChanged = true;
                        }
                        ImGui::EndMenu();
                    }

                    // Accent color selection dropdown
                    if (ImGui::BeginMenu("Accent"))
                    {
                        struct AccentOption { const char* name; const char* key; AccentColor color; };
                        AccentOption options[] = {
                            {"Default (Blue)", "default", AccentColor::DefaultBlue},
                            {"Green",          "green",   AccentColor::Green},
                            {"Purple",         "purple",  AccentColor::Purple},
                            {"Red",            "red",     AccentColor::Red},
                            {"Brown",          "brown",   AccentColor::Brown},
                            {"Cyan",           "cyan",    AccentColor::Cyan},
                        };
                        for (const auto& opt : options) {
                            bool isSelected = (appState.currentAccentColor == opt.key);
                            ImVec4 base = GetAccentBase(opt.color);

                            // Draw colored badge in the left margin, horizontally stacked before the text
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            float h = ImGui::GetTextLineHeight();
                            float badgeSize = h * 0.6f;
                            ImVec2 badgeMin(p.x, p.y + (h - badgeSize) * 0.5f);
                            ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
                            dl->AddRectFilled(badgeMin, badgeMax, ImColor(base));

                            // Offset cursor past the badge so text doesn't intersect it
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + badgeSize + 4.0f);

                            ImGui::PushStyleColor(ImGuiCol_Header, GetAccentMuted(opt.color));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, GetAccentHovered(opt.color));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, GetAccentActive(opt.color));
                            if (ImGui::MenuItem(opt.name, NULL, isSelected)) {
                                appState.currentAccentColor = opt.key;
                                appState.accentColorChanged = true;
                            }
                            ImGui::PopStyleColor(3);
                        }
                        ImGui::EndMenu();
                    }

                    // Worker Threads selection
                    if (ImGui::BeginMenu("Worker Threads"))
                    {
                        const char* workerLabels[] = {"AUTO", "1", "2", "4", "8", "16"};
                        int workerValues[] = {-1, 1, 2, 4, 8, 16};
                        int currentWorker = appState.configuredWorkerCount;
                        for (int wi = 0; wi < 6; ++wi) {
                            bool selected = (currentWorker == workerValues[wi]);
                            if (ImGui::MenuItem(workerLabels[wi], NULL, selected)) {
                                appState.reconfigurePool(workerValues[wi]);
                                config.workerThreads = workerValues[wi];
                                config.saveToFile(configFilePath);
                            }
                        }
                        ImGui::EndMenu();
                    }
                    
                    ImGui::EndMenu();
                }
                
                // Help menu
                if (ImGui::BeginMenu("Help"))
                {
                    ImGui::Text("Keyboard Shortcuts:");
                    ImGui::Separator();
                    ImGui::Text("Up/Down Arrows: Navigate files");
                    ImGui::Text("Space: Toggle selection checkboxes for highlighted files");
                    ImGui::Text("Delete: Delete current file");
                    ImGui::Text("Shift + mouse / Right click: X-axis range selection");
                    ImGui::Text("ESC: Reset zoom");
                    ImGui::Text("Mouse Scroll: Zoom in/out");
                    ImGui::Text("Ctrl+Y: Toggle auto-fit Y-axis");
                    ImGui::Text("Ctrl+A: Toggle max at zero");
                    ImGui::Text("Ctrl+D: Toggle downsampling");
                    ImGui::Text("Ctrl+H: Go back to home");
                    ImGui::Text("Ctrl+Q: Toggle tracking cursor");
                    ImGui::Separator();
                    if (ImGui::MenuItem("About")) {
                        openAboutPopup();
                    }
                    ImGui::EndMenu();
                }
                
                ImGui::EndMainMenuBar();
                renderAboutPopup();
            }
}
