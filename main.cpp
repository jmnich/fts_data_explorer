#include <iostream>
#include <ostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

// Include config header
#include "config.h"
#include "app_state.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "snr_spectrum.h"
#include "allan_variance.h"
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "adapters/adapter_registry.h"
#include "adapters/wust_mini_fts_adapter.h"
#include "adapters/arcoptix_igms_adapter.h"
#include "adapters/arcoptix_spectra_adapter.h"
#include "tinyfiledialogs.h"
#include "icon.h"
#include "stb_image.h"
#include "file_browser.h"
#include "welcome.h"
#include "about.h"
#include "theme.h"
#include "headless.h"
#include "version.h"

// Include imgui and other dependencies
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "implot3d.h"
#include <GLFW/glfw3.h>

// Simple file dialog implementation (replaces NFD)
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>





static void SetupAxisTicksLimited(ImAxis axis, double min, double max, int maxTicks = 12) {
    double range = max - min;
    if (range <= 0.0) return;

    double roughStep = range / (maxTicks - 1);
    double exponent = std::floor(std::log10(roughStep));
    double fraction = roughStep / std::pow(10.0, exponent);

    double niceFraction;
    if (fraction <= 1.0) niceFraction = 1.0;
    else if (fraction <= 2.0) niceFraction = 2.0;
    else if (fraction <= 5.0) niceFraction = 5.0;
    else niceFraction = 10.0;

    double step = niceFraction * std::pow(10.0, exponent);
    double firstTick = std::ceil(min / step) * step;

    std::vector<double> ticks;
    ticks.reserve(maxTicks);
    for (double tick = firstTick; tick <= max + step * 0.5; tick += step) {
        ticks.push_back(tick);
    }

    if (!ticks.empty()) {
        ImPlot::SetupAxisTicks(axis, ticks.data(), ticks.size(), nullptr);
    }
}

// Natural sort comparison function for filenames with numbers
static bool naturalSortCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        // Skip non-digit characters
        if (!isdigit(a[i]) || !isdigit(b[j])) {
            if (a[i] != b[j]) {
                return a[i] < b[j];
            }
            i++; j++;
        } else {
            // Compare numeric sequences
            size_t numStartA = i;
            size_t numStartB = j;
            while (i < a.size() && isdigit(a[i])) i++;
            while (j < b.size() && isdigit(b[j])) j++;
            
            std::string numStrA = a.substr(numStartA, i - numStartA);
            std::string numStrB = b.substr(numStartB, j - numStartB);
            
            // Convert to numbers and compare
            int numA = std::stoi(numStrA);
            int numB = std::stoi(numStrB);
            
            if (numA != numB) {
                return numA < numB;
            }
        }
    }
    return a.size() < b.size();
}



// ── Window icon (accent-color-aware, rebuilt from white template) ──────────

static int s_iconW = 0, s_iconH = 0;
static unsigned char* s_iconTemplate = nullptr; // cached white-on-transparent RGBA

static void freeIconTemplate() {
    if (s_iconTemplate) {
        stbi_image_free(s_iconTemplate);
        s_iconTemplate = nullptr;
        s_iconW = s_iconH = 0;
    }
}

static void applyWindowIcon(GLFWwindow* window, const ImVec4& accent) {
    if (!s_iconTemplate) {
        int ch;
        s_iconTemplate = stbi_load_from_memory(
            assets_icon_png, assets_icon_png_len, &s_iconW, &s_iconH, &ch, 4);
        if (!s_iconTemplate) {
            std::cerr << "Failed to decode icon PNG" << std::endl;
            return;
        }
    }

    int totalPixels = s_iconW * s_iconH;
    auto* pixels = (unsigned char*)malloc((size_t)totalPixels * 4);
    if (!pixels) return;

    // Dark background (matches app theme)
    memset(pixels, 26, (size_t)totalPixels * 4);

    // Tint waveform pixels with accent color
    // Boost saturation for icon visibility (1.5x, clamped)
    float sr = std::min(accent.x * 2.0f, 1.0f);
    float sg = std::min(accent.y * 2.0f, 1.0f);
    float sb = std::min(accent.z * 2.0f, 1.0f);
    int r = (int)(sr * 255.0f);
    int g = (int)(sg * 255.0f);
    int b = (int)(sb * 255.0f);
    for (int i = 0; i < totalPixels; i++) {
        int idx = i * 4;
        if (s_iconTemplate[idx + 3] > 0) {
            pixels[idx + 0] = (unsigned char)r;
            pixels[idx + 1] = (unsigned char)g;
            pixels[idx + 2] = (unsigned char)b;
            pixels[idx + 3] = s_iconTemplate[idx + 3];
        }
    }

    GLFWimage icon = { s_iconW, s_iconH, pixels };
    glfwSetWindowIcon(window, 1, &icon);
    free(pixels);
}

// ────────────────────────────────────────────────────────────────────────────

/**
 * Initialize GLFW, ImGui, and application state
 * @param config Application configuration
 * @param window Reference to GLFW window pointer
 * @return true if initialization successful, false otherwise
 */
bool initializeApplication(AppConfig& config, GLFWwindow*& window) {
    std::cout << "FTS Data Explorer " << APP_VERSION << " - Starting application..." << std::endl;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

    // Configure OpenGL context for better performance
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    // Enable hardware acceleration and prefer dedicated GPU
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);

    // Create window with saved settings
    {
        std::string title = std::string("FTS Data Explorer ") + APP_VERSION;
        window = glfwCreateWindow(config.windowWidth, config.windowHeight, title.c_str(), nullptr, nullptr);
    }
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    // Set window position if saved (not centered)
    if (config.windowPosX != -1 && config.windowPosY != -1) {
        glfwSetWindowPos(window, config.windowPosX, config.windowPosY);
    }

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1);

    glfwSetWindowUserPointer(window, &appState);

    // Install dirty-flag callbacks BEFORE ImGui GLFW init (ImGui wraps them)
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double, double) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double, double) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int, int, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetDropCallback(window, [](GLFWwindow* w, int, const char**) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowPosCallback(window, [](GLFWwindow* w, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
    });

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // Initialize ImPlot context
    ImPlot::CreateContext();
    ImPlot3D::CreateContext();

    // Use a lighter grid color for improved visibility
    ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid] = ImVec4(0.85f, 0.85f, 0.85f, 0.85f);
    ImPlot::GetStyle().MajorGridSize = ImVec2(2.5f, 2.5f);
    ImPlot::GetStyle().MinorGridSize = ImVec2(1.5f, 1.5f);
    ImPlot3D::GetStyle().Colors[ImPlot3DCol_AxisGrid] = ImVec4(0.85f, 0.85f, 0.85f, 0.85f);
    
    // Optimize for large datasets - disable anti-aliasing (will be conditionally applied)
    // ImGuiStyle& style = ImGui::GetStyle();
    // style.AntiAliasedLines = false;

    return true;
}



/**
 * Process window events and update configuration
 * @param window GLFW window pointer
 * @param config Application configuration
 */
void handleWindowEvents(GLFWwindow* window, AppConfig& config) {
    // Track window state changes
    int newWidth, newHeight;
    glfwGetWindowSize(window, &newWidth, &newHeight);
    if (newWidth != config.windowWidth || newHeight != config.windowHeight) {
        config.windowWidth = newWidth;
        config.windowHeight = newHeight;
    }

    int newPosX, newPosY;
    glfwGetWindowPos(window, &newPosX, &newPosY);
    if (newPosX != config.windowPosX || newPosY != config.windowPosY) {
        config.windowPosX = newPosX;
        config.windowPosY = newPosY;
    }

    // Check if window is maximized
    config.windowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
}

void applyAdapterSelection(const std::string& adapterName, const std::string& directoryPath);

void selectAdapterForDirectory(const std::string& directoryPath) {
    // Clear any stale incompatible-adapter state from previous interactions
    appState.showIncompatibleAdapterPopup = false;
    appState.pendingAdapterName.clear();
    appState.pendingAdapterDirectory.clear();

    // Commented out: auto-filtering of adapters based on directory contents.
    // For now, always show all registered adapters so user can pick.
    // auto adapters = AdapterRegistry::instance().findAdaptersForDirectory(directoryPath);
    const auto& allAdapters = AdapterRegistry::instance().getAll();
    std::vector<DataAdapter*> adapters;
    for (const auto& a : allAdapters) adapters.push_back(a.get());

    if (adapters.empty()) {
        appState.adapterErrorMsg = "No compatible data format found in:\n" + directoryPath;
        appState.showAdapterErrorPopup = true;
        appState.showWelcomeScreen = true;
        appState.welcomeScreenInitialized = false;
        appState.csvFiles.clear();
    } else {
        // Always show popup — user picks adapter each time
        appState.compatibleAdapters = adapters;
        appState.showAdapterSelectionPopup = true;
        // Keep welcome screen active so popup shows on top of it
        appState.showWelcomeScreen = true;
        appState.welcomeScreenInitialized = false;
        appState.csvFiles.clear();
    }
}

void applyAdapterSelection(const std::string& adapterName, const std::string& directoryPath) {
    auto* adapter = AdapterRegistry::instance().getAdapter(adapterName);
    if (!adapter) return;

    appState.currentAdapter.reset();
    // Create appropriate concrete adapter based on name
    if (adapterName == "WUST Mini FTS Raw")
        appState.currentAdapter = std::make_unique<WustMiniFtsAdapter>();
    else if (adapterName == "ArcOptix raw IGMs")
        appState.currentAdapter = std::make_unique<ArcoptixIgmsAdapter>();
    else if (adapterName == "ArcOptix Spectra Sequence")
        appState.currentAdapter = std::make_unique<ArcoptixSpectraAdapter>();
    else return;

    appState.datasetInfo = adapter->getDatasetInfo();
    appState.csvFiles = adapter->listFiles(directoryPath);
    appState.showAdapterSelectionPopup = false;
    appState.showIncompatibleAdapterPopup = false;
    appState.pendingAdapterName.clear();
    appState.pendingAdapterDirectory.clear();

    // Apply feature gates based on dataset info
    if (appState.datasetInfo.axisIsCorrected) {
        appState.xAxisBase = 1; // Force OPD mode
    }
    appState.clearAverageSpectrum();
    appState.clearSnrSpectrum();
    appState.clearAllanVariance();
    appState.clearT100Spectrum();
    appState.showWelcomeScreen = false;
    appState.welcomeScreenInitialized = true;
    appState.dataLoaded = false;
    appState.loadedData.clear();
    appState.rawDataCache.clear();
    appState.hilbertXCache.clear();
    appState.peakPositionsCache.clear();
    appState.hilbertCacheLaserWavelength = 0.0f;
    appState.selectedFiles.clear();
    appState.selectedFilenames.clear();
    appState.filesChanged = true;
    appState.currentSortedFileIndex = 0;
    appState.isFirstDataLoad = true;
    appState.needsRedraw = true;

    // Save adapter to recent datasets if pendingRecentDatasetAdapterSave is set
    if (appState.configPtr && !appState.pendingRecentDatasetAdapterSave.empty()) {
        for (auto& entry : appState.configPtr->recentDatasets) {
            if (entry.path == appState.pendingRecentDatasetAdapterSave) {
                entry.adapterName = adapterName;
                break;
            }
        }
        appState.configPtr->saveToFile(appState.configFilePath);
        appState.pendingRecentDatasetAdapterSave.clear();
    }

    std::cout << "Adapter selected: " << adapterName << " for " << directoryPath << std::endl;
}

static std::string shortenFilename(const std::string& filename) {
    const size_t maxLen = 38;
    if (filename.length() <= maxLen) return filename;
    const size_t keepStart = 8;
    const size_t keepEnd = 24;
    return filename.substr(0, keepStart) + "..." + filename.substr(filename.length() - keepEnd);
}

static void renderAdapterSelectionPopup() {
    if (!appState.showAdapterSelectionPopup) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(800, 250), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));

    if (ImGui::BeginPopupModal("Select Data Adapter##adapterSelect", &appState.showAdapterSelectionPopup,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("Dataset: %s", appState.currentDirectory.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        static int selectedIdx = 0;
        if (selectedIdx >= static_cast<int>(appState.compatibleAdapters.size()))
            selectedIdx = 0;

        for (size_t i = 0; i < appState.compatibleAdapters.size(); i++) {
            auto* adapter = appState.compatibleAdapters[i];
            bool compatible = adapter->canLoadDirectory(appState.currentDirectory);
            std::string label = std::string("- ") + adapter->getName() + " (" + adapter->getFileExtension() + ")";

            if (!compatible)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

            if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == selectedIdx)) {
                if (compatible) {
                    applyAdapterSelection(adapter->getName(), appState.currentDirectory);
                } else {
                    appState.pendingAdapterName = adapter->getName();
                    appState.pendingAdapterDirectory = appState.currentDirectory;
                    appState.showIncompatibleAdapterPopup = true;
                    appState.showAdapterSelectionPopup = false;
                    appState.compatibleAdapters.clear();
                }
                selectedIdx = 0;
                if (!compatible)
                    ImGui::PopStyleColor(); // pop text color
                ImGui::PopStyleColor(); // pop dim bg
                ImGui::EndPopup();
                return;
            }

            if (!compatible) {
                ImVec2 textMin = ImGui::GetItemRectMin();
                ImVec2 textMax = ImGui::GetItemRectMax();
                float lineY = (textMin.y + textMax.y) * 0.5f;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(textMin.x, lineY),
                    ImVec2(textMax.x, lineY),
                    IM_COL32(128, 128, 128, 128), 1.0f);
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Keyboard navigation
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selectedIdx > 0)
            selectedIdx--;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selectedIdx < static_cast<int>(appState.compatibleAdapters.size()) - 1)
            selectedIdx++;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && selectedIdx >= 0 && selectedIdx < static_cast<int>(appState.compatibleAdapters.size())) {
            auto* adapter = appState.compatibleAdapters[selectedIdx];
            bool enterCompatible = adapter->canLoadDirectory(appState.currentDirectory);
            if (enterCompatible) {
                applyAdapterSelection(adapter->getName(), appState.currentDirectory);
            } else {
                appState.pendingAdapterName = adapter->getName();
                appState.pendingAdapterDirectory = appState.currentDirectory;
                appState.showIncompatibleAdapterPopup = true;
                appState.showAdapterSelectionPopup = false;
                appState.compatibleAdapters.clear();
            }
            selectedIdx = 0;
            ImGui::PopStyleColor(); // pop dim bg
            ImGui::EndPopup();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            appState.showAdapterSelectionPopup = false;
            appState.compatibleAdapters.clear();
            appState.currentDirectory = "";
            selectedIdx = 0;
            ImGui::PopStyleColor();
            ImGui::EndPopup();
            return;
        }

        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            appState.showAdapterSelectionPopup = false;
            appState.compatibleAdapters.clear();
            appState.currentDirectory = "";
            selectedIdx = 0;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

static void performFileDeletion(AppState& appState, size_t index) {
    const auto& file = appState.sortedFiles[index];

    std::error_code ec;
    bool removed = std::filesystem::remove(file, ec);
    if (!removed || ec) {
        std::cerr << "Failed to delete file: " << file << " (" << ec.message() << ")" << std::endl;
        return;
    }

    std::cout << "Deleted file: " << file << std::endl;

    // Remove from csvFiles
    auto csvIt = std::find(appState.csvFiles.begin(), appState.csvFiles.end(), file);
    if (csvIt != appState.csvFiles.end())
        appState.csvFiles.erase(csvIt);

    // Remove from sortedFiles at index
    appState.sortedFiles.erase(appState.sortedFiles.begin() + index);

    // Remove from filesSelectedForAveraging
    if (index < appState.filesSelectedForAveraging.size())
        appState.filesSelectedForAveraging.erase(appState.filesSelectedForAveraging.begin() + index);

    // If the file was in selectedFiles, remove it there too
    auto selIt = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file);
    if (selIt != appState.selectedFiles.end()) {
        size_t selIdx = std::distance(appState.selectedFiles.begin(), selIt);
        appState.selectedFiles.erase(appState.selectedFiles.begin() + selIdx);
        appState.selectedFilenames.erase(appState.selectedFilenames.begin() + selIdx);
        appState.loadedData.erase(appState.loadedData.begin() + selIdx);
        appState.rawDataCache.erase(appState.rawDataCache.begin() + selIdx);
    }

    // Adjust currentSortedFileIndex: jump to previous file when deleting current
    if (index < appState.currentSortedFileIndex) {
        appState.currentSortedFileIndex--;
    } else if (index == appState.currentSortedFileIndex) {
        if (appState.currentSortedFileIndex > 0)
            appState.currentSortedFileIndex--;
        appState.filesChanged = true; // trigger reload from new position
    }
    if (appState.currentSortedFileIndex >= appState.sortedFiles.size())
        appState.currentSortedFileIndex = appState.sortedFiles.empty() ? 0 : appState.sortedFiles.size() - 1;

    if (appState.loadedData.empty())
        appState.dataLoaded = false;

    appState.needsRedraw = true;
}

static void renderIncompatibleAdapterPopup() {
    static int focusIdx = 0;
    static bool prevPopupOpen = false;
    if (!appState.showIncompatibleAdapterPopup) {
        prevPopupOpen = false;
        return;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(450, 160), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));

    bool popupOpened = ImGui::BeginPopupModal("Incompatible##incompatAdapter", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (popupOpened) {
        ImGui::TextWrapped("Incompatible data adapter. Continue anyway?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter);
        if (popupOpened && !prevPopupOpen)
            focusIdx = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            focusIdx = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            focusIdx = 1;

        // Highlight focused button with accent color
        if (focusIdx == 0)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Back", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (enterPressed && prevPopupOpen && focusIdx == 0)) {
            if (focusIdx == 0) ImGui::PopStyleColor();
            const auto& allAdapters = AdapterRegistry::instance().getAll();
            std::vector<DataAdapter*> adapters;
            for (const auto& a : allAdapters) adapters.push_back(a.get());
            appState.compatibleAdapters = adapters;
            appState.showAdapterSelectionPopup = true;
            appState.showIncompatibleAdapterPopup = false;
            appState.pendingAdapterName.clear();
            appState.pendingAdapterDirectory.clear();
            ImGui::CloseCurrentPopup();
        } else if (focusIdx == 0) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (focusIdx == 1)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Yes", ImVec2(120, 0)) ||
            (enterPressed && prevPopupOpen && focusIdx == 1)) {
            if (focusIdx == 1) ImGui::PopStyleColor();
            applyAdapterSelection(appState.pendingAdapterName, appState.pendingAdapterDirectory);
            appState.showIncompatibleAdapterPopup = false;
            appState.pendingAdapterName.clear();
            appState.pendingAdapterDirectory.clear();
            ImGui::CloseCurrentPopup();
        } else if (focusIdx == 1) {
            ImGui::PopStyleColor();
        }

        ImGui::EndPopup();
    }
    prevPopupOpen = popupOpened;
    ImGui::PopStyleColor();
}

/**
 * Handle keyboard navigation for file selection
 * @param csvFiles List of available CSV files
 * @param currentSortedFileIndex Current file index reference
 * @param filesChanged Reference to files changed flag
 * @param keyboardNavigation Reference to keyboard navigation flag
 */
void handleKeyboardNavigation(const std::vector<std::string>& csvFiles, 
                             size_t& currentSortedFileIndex, 
                             bool& filesChanged, 
                             bool& keyboardNavigation, 
                             bool shiftSelectMode, 
                             std::vector<std::string>& selectedFiles, 
                             std::vector<std::string>& selectedFilenames, 
                             std::vector<InterferogramData>& loadedData, 
                             std::vector<InterferogramData>& rawDataCache, 
                             bool& dataLoaded, 
                             const std::vector<std::string>& sortedFiles, 
                             bool enableDownsampling, 
                             size_t maxPointsBeforeDownsampling, 
                             size_t maxSelectableFiles) {
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && !csvFiles.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            // Navigate up in file list (with wrapping)
            if (currentSortedFileIndex > 0) {
                currentSortedFileIndex--;
            } else {
                currentSortedFileIndex = csvFiles.size() - 1; // Wrap to bottom
            }
            filesChanged = true;
            keyboardNavigation = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            // Navigate down in file list (with wrapping)
            if (currentSortedFileIndex < csvFiles.size() - 1) {
                currentSortedFileIndex++;
            } else {
                currentSortedFileIndex = 0; // Wrap to top
            }
            filesChanged = true;
            keyboardNavigation = true;
        }
        
        // Handle Shift+Arrow for adding next file to selection
        if (shiftSelectMode) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                // Add the current file to selection with FIFO behavior if limit reached
                std::string fullPath = sortedFiles[currentSortedFileIndex];
                
                // Check if file is already selected
                auto it = std::find(selectedFiles.begin(), selectedFiles.end(), fullPath);
                if (it == selectedFiles.end()) {
                    // File not already selected, add it
                    try {
                        if (!appState.currentAdapter) {
                            filesChanged = false;
                            return;
                        }
                        InterferogramData data = appState.currentAdapter->loadFile(fullPath);
                        InterferogramData rawData = data; // Store raw data before any processing
                        
                        // Apply downsampling
                        if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                            size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                            std::vector<double> downsampledRef, downsampledPrim;
                            for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                downsampledRef.push_back(data.referenceDetector[j]);
                                downsampledPrim.push_back(data.primaryDetector[j]);
                            }
                            data.referenceDetector = downsampledRef;
                            data.primaryDetector = downsampledPrim;
                        }
                        
                        // Enforce 5-file limit with FIFO behavior
                        if (selectedFiles.size() >= maxSelectableFiles) {
                            // Remove oldest file (FIFO)
                            selectedFiles.erase(selectedFiles.begin());
                            selectedFilenames.erase(selectedFilenames.begin());
                            loadedData.erase(loadedData.begin());
                            rawDataCache.erase(rawDataCache.begin()); // Also remove from raw data cache
                        }
                        
                        loadedData.push_back(data);
                        rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                        selectedFiles.push_back(fullPath);
                        
                        // Extract filename for legend
                        std::string filename = fullPath;
                        size_t last_slash = filename.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            filename = filename.substr(last_slash + 1);
                        }
                        selectedFilenames.push_back(filename);
                        
                        dataLoaded = true;
                    } catch (const std::exception& e) {
                        std::cerr << "Error loading file: " << e.what() << std::endl;
                    }
                }
                
                // Don't change filesChanged since we're adding to selection, not replacing
                filesChanged = false;
            }
        }
    }
}

/**
 * Handle UI scaling changes
 * @param io ImGuiIO reference for DPI scaling
 * @param uiScale UI scale factor reference
 * @param currentUiSize Current UI size setting
 * @param uiSizeChanged Reference to UI size changed flag
 */
void handleUIScaling(ImGuiIO& io, float& uiScale, const std::string& currentUiSize, bool& uiSizeChanged) {
    if (uiSizeChanged) {
        float dpi_scale = io.DisplayFramebufferScale.x;
        
        // Update scale based on new UI size
        if (currentUiSize == "tiny") {
            uiScale = 0.75f;
        } else if (currentUiSize == "small") {
            uiScale = 0.9f;
        } else if (currentUiSize == "normal") {
            uiScale = 1.0f;
        } else if (currentUiSize == "large") {
            uiScale = 1.4f;
        } else if (currentUiSize == "huge") {
            uiScale = 1.8f;
        }
        
        // Reapply scaling (font only to avoid UI issues)
        io.FontGlobalScale = dpi_scale * uiScale;
        
        uiSizeChanged = false; // Reset flag
    }
}

/**
 * Clean up application resources
 * @param window GLFW window pointer
 */
void cleanupApplication(GLFWwindow* window) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot3D::DestroyContext();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    freeIconTemplate();
    
    glfwDestroyWindow(window);
    glfwTerminate();
}

static void rebuildDefaultLayout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace_id,
        ImVec2(vp->Size.x, vp->Size.y - ImGui::GetFrameHeight()));

    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.16f, &dock_left, &dock_right);

    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.40f, &dock_left_top, &dock_left_bottom);

    ImGuiID dock_left_bottom_top, dock_left_bottom_bottom;
    ImGui::DockBuilderSplitNode(dock_left_bottom, ImGuiDir_Up, 0.50f, &dock_left_bottom_top, &dock_left_bottom_bottom);

    ImGuiID dock_center, dock_right_panel;
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Left, 0.48f, &dock_center, &dock_right_panel);

    ImGuiID dock_right_top, dock_right_bottom;
    ImGui::DockBuilderSplitNode(dock_right_panel, ImGuiDir_Up, 0.50f, &dock_right_top, &dock_right_bottom);

    ImGui::DockBuilderDockWindow("Files",              dock_left_top);
    ImGui::DockBuilderDockWindow("Metadata",           dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Export",             dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("SNR",                dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("100% T",             dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Allan",              dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Spectrum",           dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Interferogram",      dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Average",            dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Interferogram View", dock_center);
    ImGui::DockBuilderDockWindow("100% T View",        dock_center);
    ImGui::DockBuilderDockWindow("Allan View",         dock_center);
    ImGui::DockBuilderDockWindow("SNR View",           dock_right_top);
    ImGui::DockBuilderDockWindow("Average View",       dock_right_top);
    ImGui::DockBuilderDockWindow("Spectrum View",      dock_right_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

int main(int argc, char* argv[]) {
    // Parse headless mode flags before any GUI initialization
    HeadlessConfig headlessCfg;
    if (parseHeadlessArgs(argc, argv, headlessCfg)) return 1;
    if (runHeadlessCommand(headlessCfg)) return 0;

    // Set environment variables to prefer dedicated GPU on NVIDIA systems
    #ifdef _WIN32
    _putenv("D3D12_ENABLE_LAYERED_DRIVER_QUERY=1");
    _putenv("D3D12_ENABLE_EXPERIMENTAL_FEATURES=1");
    #else
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
    #endif
    
    std::cout << "FTS Data Explorer " << APP_VERSION << " - Starting application..." << std::endl;
    
    // Initialize configuration
    AppConfig config;
    std::string configFilePath = getConfigFilePath();
    
    // Load existing config if available
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
        std::cout << "Loaded configuration from " << configFilePath << std::endl;
    } else {
        std::cout << "No existing config found, using defaults" << std::endl;
    }

    // Store config pointers for use by adapter selection
    appState.configPtr = &config;
    appState.configFilePath = configFilePath;

    // UI size settings
    appState.currentUiSize = config.uiSize;
    appState.currentAccentColor = config.accentColor;
    appState.reconfigurePool(config.workerThreads);

    // Register data adapters
    AdapterRegistry::instance().registerAdapter(std::make_unique<WustMiniFtsAdapter>());
    AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixIgmsAdapter>());
    AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixSpectraAdapter>());

    // Handle -o flag: auto-apply adapter (skips welcome screen) before GUI init
    if (headlessCfg.command == HeadlessConfig::Command::OpenGUI) {
        std::cout << "Auto-selecting adapter: " << headlessCfg.adapter
                  << " for " << headlessCfg.path << std::endl;
        appState.currentDirectory = headlessCfg.path;
        applyAdapterSelection(headlessCfg.adapter, headlessCfg.path);
    }

    // Initialize application
    GLFWwindow* window = nullptr;
    if (!initializeApplication(config, window)) {
        return -1;
    }
    
    // Get ImGui IO (context already created in initializeApplication)
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
    // Use explicit imgui.ini path for first-launch layout detection
    io.IniFilename = "imgui.ini";
    
    ImGui::StyleColorsDark();

    // Apply accent theme
    ImGuiStyle& style = ImGui::GetStyle();
    ImPlotStyle& plotStyle = ImPlot::GetStyle();
    ApplyTheme(style, plotStyle, StringToAccentColor(appState.currentAccentColor));

    // Set window icon tinted with current accent color
    applyWindowIcon(window, GetAccentBase(StringToAccentColor(appState.currentAccentColor)));

    // Load welcome screen background texture
    initWelcomeBackground();
    
    // Customize colors to use black background
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Customize plot colors
    ImVec4 yellow_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Bright yellow
    ImVec4 purple_color = ImVec4(0.5f, 0.0f, 0.5f, 1.0f); // Purple for selection
    ImVec4 background_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f); // Black background
    
    // Set plot colors
    style.Colors[ImGuiCol_PlotLines] = yellow_color;
    style.Colors[ImGuiCol_PlotLinesHovered] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogram] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogramHovered] = yellow_color;
    
    // Set ImPlot selection color to match our custom purple
    plotStyle.Colors[ImPlotCol_Selection] = purple_color;
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 460");
    
    // ImPlot context already created in initializeApplication
    
    // Set up for high DPI displays AFTER backend initialization
    // Apply scaling based on UI size setting
    float dpi_scale = io.DisplayFramebufferScale.x;
    
    // Apply UI scaling based on selected size
    if (appState.currentUiSize == "tiny") {
        appState.uiScale = 0.75f;
    } else if (appState.currentUiSize == "small") {
        appState.uiScale = 0.9f;
    } else if (appState.currentUiSize == "normal") {
        appState.uiScale = 1.0f;
    } else if (appState.currentUiSize == "large") {
        appState.uiScale = 1.4f;
    } else if (appState.currentUiSize == "huge") {
        appState.uiScale = 1.8f;
    }
    
    // Apply the scaling (font only initially to avoid UI issues)
    io.FontGlobalScale = dpi_scale * appState.uiScale;
    // Note: We don't call ScaleAllSizes here to avoid UI element issues
    // The font scaling will handle most of the UI size adjustment
    
    // Main application state
    // Use config settings if available, otherwise use empty path
    if (!config.lastWorkingDirectory.empty() && std::filesystem::exists(config.lastWorkingDirectory)) {
        appState.currentDirectory = config.lastWorkingDirectory;
    } else {
        appState.currentDirectory = "";
    }
    
    appState.maxAtZero = config.maxAtZero; // Use config setting for peak alignment
    appState.autoFitYAxis = config.autoFitYAxis; // Load from config
    appState.enableDownsampling = config.enableDownsampling; // Load from config
    appState.xAxisBase = config.xAxisBase; // Load from config
    appState.showFPS = config.showFPS; // Load from config
    appState.gridAlpha = config.gridAlpha; // Load from config
    appState.xCorrectionMethod = config.xCorrectionMethod;
    appState.peakProminenceThreshold = config.peakProminence;
    appState.showPeakIndicators = config.showPeakIndicators;
    appState.currentAccentColor = config.accentColor; // Load accent color from config

    // Load docking layout flag from config (persisted so DockBuilder runs only once)
    appState.defaultLayoutApplied = config.defaultLayoutApplied;

    // If imgui.ini is missing (e.g. user deleted it to reset layout),
    // force DockBuilder to run regardless of config state.
    if (!std::filesystem::exists(io.IniFilename)) {
        appState.defaultLayoutApplied = false;
    }
    
    // Load spectrum window settings from config
    appState.spectrum.yAxisMode           = config.spectrumYAxisMode;
    appState.spectrum.prevYAxisMode       = config.spectrumYAxisMode;
    appState.spectrum.xUnitSelector       = config.spectrumXUnitSelector;
    appState.spectrum.yScaleSelector  = config.spectrumYScaleSelector;
    appState.spectrum.prevXUnitSelector = config.spectrumXUnitSelector;
    appState.spectrum.prevYScaleSelector = config.spectrumYScaleSelector;
    appState.spectrum.forcedYMin      = config.spectrumForcedYMin;
    appState.spectrum.forcedYMax    = config.spectrumForcedYMax;
    appState.spectrum.apodizationSelector = config.apodizationSelector;
    appState.spectrum.apodizationParams.gaussSigma = config.apodGaussSigma;
    appState.spectrum.apodizationParams.rectWidth  = config.apodRectWidth;
    appState.spectrum.apodizationParams.nortonBeerFwhm = config.apodNortonBeerFwhm;
    appState.spectrum.apodizationParams.dolphChebyshevAt = config.apodDolphChebyshevAt;
    appState.spectrum.apodizationParams.hammingAlpha = config.apodHammingAlpha;
    appState.spectrum.apodizationParams.kaiserBeta = config.apodKaiserBeta;
    appState.spectrum.apodizationParams.rectAsymMode = config.apodRectAsymMode;
    appState.spectrum.detectorSensitivity = config.spectrumDetectorSensitivity;
    if (config.spectrumDetectorSensitivity == 0.0f)
        snprintf(appState.spectrum.detectorSensitivityText,
                 sizeof(appState.spectrum.detectorSensitivityText), "NA");
    else
        snprintf(appState.spectrum.detectorSensitivityText,
                 sizeof(appState.spectrum.detectorSensitivityText), "%.4f",
                 config.spectrumDetectorSensitivity);
    appState.spectrum.refLaserTextbox = config.spectrumRefLaser;
    
    // Set the appState pointer in the spectrum object for raw data access
    appState.spectrum.appState = &appState;
    appState.averageSpectrum.appState = &appState;
    appState.snrSpectrum.appState = &appState;
    appState.allanVariance.appState = &appState;
    appState.t100.appState = &appState;
    appState.exportPanel.appState = &appState;
    
    // Load average window settings from config
    appState.averageSpectrum.yAxisMode           = config.avgYAxisMode;
    appState.averageSpectrum.prevYAxisMode       = config.avgYAxisMode;
    appState.averageSpectrum.xUnitSelector       = config.avgXUnitSelector;
    appState.averageSpectrum.yScaleSelector      = config.avgYScaleSelector;
    appState.averageSpectrum.prevXUnitSelector   = config.avgXUnitSelector;
    appState.averageSpectrum.prevYScaleSelector  = config.avgYScaleSelector;
    appState.averageSpectrum.forcedYMin          = config.avgForcedYMin;
    appState.averageSpectrum.forcedYMax          = config.avgForcedYMax;

    // Load SNR window settings from config
    appState.snrSpectrum.yAxisMode           = config.snrYAxisMode;
    appState.snrSpectrum.prevYAxisMode       = config.snrYAxisMode;
    appState.snrSpectrum.xUnitSelector       = config.snrXUnitSelector;
    appState.snrSpectrum.yScaleSelector      = config.snrYScaleSelector;
    appState.snrSpectrum.prevXUnitSelector   = config.snrXUnitSelector;
    appState.snrSpectrum.prevYScaleSelector  = config.snrYScaleSelector;
    appState.snrSpectrum.forcedYMin          = config.snrForcedYMin;
    appState.snrSpectrum.forcedYMax          = config.snrForcedYMax;

    appState.allanVariance.xUnitSelector        = config.allanXUnitSelector;
    appState.allanVariance.wavelengthDecimation  = config.allanWavelengthDecimation;
    appState.allanVariance.selectedSliceIndex    = config.allanSliceIndex;
    appState.allanVariance.xRangeMin             = config.allanXRangeMin;
    appState.allanVariance.xRangeMax             = config.allanXRangeMax;
    appState.allanVariance.calcBaseSelector      = config.allanCalcBaseSelector;

    // Load 100% T window settings from config
    appState.t100.yAxisMode           = config.t100YAxisMode;
    appState.t100.prevYAxisMode       = config.t100YAxisMode;
    appState.t100.xUnitSelector       = config.t100XUnitSelector;
    appState.t100.prevXUnitSelector   = config.t100XUnitSelector;
    appState.t100.forcedYMin          = config.t100ForcedYMin;
    appState.t100.forcedYMax          = config.t100ForcedYMax;
    strncpy(appState.t100.energyRatioNumA, config.t100EnergyRatioNumA, 31);
    strncpy(appState.t100.energyRatioDenA, config.t100EnergyRatioDenA, 31);
    strncpy(appState.t100.energyRatioNumB, config.t100EnergyRatioNumB, 31);
    strncpy(appState.t100.energyRatioDenB, config.t100EnergyRatioDenB, 31);
    strncpy(appState.t100.energyRatioNumC, config.t100EnergyRatioNumC, 31);
    strncpy(appState.t100.energyRatioDenC, config.t100EnergyRatioDenC, 31);

    // No initialization needed for simple file dialog
    
    // Scroll carry-over buckets for the rate-limiter below (persist across frames)
    float scrollCarryOverY = 0.0f;
    float scrollCarryOverX = 0.0f;
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Poll pending async spectrum computations
        if (!appState.spectrum.pendingSpectra_.empty()) {
            appState.needsRedraw = true;
            appState.spectrum.pollPendingSpectra();
        }

        // Tick average spectrum calculation (parallel batch-submit + poll)
        if (appState.averageSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.averageSpectrum.tickCalculation();
        }

        // Tick SNR spectrum calculation (parallel batch-submit + poll)
        if (appState.snrSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.snrSpectrum.tickCalculation();
        }

        // Tick Allan variance calculation (parallel batch-submit + poll)
        if (appState.allanVariance.calcInProgress) {
            appState.needsRedraw = true;
            appState.allanVariance.tickCalculation();
        }

        // Tick T100 standard deviation calculation (parallel batch-submit + poll)
        if (appState.t100.calcStdInProgress) {
            appState.needsRedraw = true;
            if (appState.t100.tickStdCalculation()) {
                appState.needsRedraw = true;
            }
        }

        // FPS overlay: force periodic redraw when idle so the counter stays live
        static double lastForceRedrawTime = 0.0;
        if (!appState.needsRedraw && appState.showFPS) {
            double now = glfwGetTime();
            if (now - lastForceRedrawTime >= 1.0) {
                appState.needsRedraw = true;
                lastForceRedrawTime = now;
            }
        }

        // Skip rendering when UI is static — saves CPU/GPU
        if (!appState.needsRedraw) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!appState.showWelcomeScreen || appState.welcomeScreenInitialized) {
            appState.needsRedraw = false;
        }

        appState.multiSelectMode = ImGui::GetIO().KeyCtrl;
        appState.shiftSelectMode = ImGui::GetIO().KeyShift;
        
        // Handle keyboard shortcuts - only trigger once per key press
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            bool yKeyPressed = glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool aKeyPressed = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool dKeyPressed = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool qKeyPressed = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            
            // 'Ctrl+Y' - Toggle auto-fit Y-axis (only on initial press)
            if (yKeyPressed && !appState.yKeyPressedLastFrame) {
                appState.autoFitYAxis = !appState.autoFitYAxis;
                if (appState.autoFitYAxis && appState.dataLoaded) {
                    if (!appState.loadedData[0].referenceDetector.empty()) {
                        auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                        appState.ref_y_min = *ref_min_max.first;
                        appState.ref_y_max = *ref_min_max.second;
                    }
                    auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                    appState.prim_y_min = *prim_min_max.first;
                    appState.prim_y_max = *prim_min_max.second;
                }
            }
            
            // 'Ctrl+A' - Toggle max at zero (only on initial press)
            if (aKeyPressed && !appState.aKeyPressedLastFrame) {
                appState.maxAtZero = !appState.maxAtZero;
                appState.shouldAutoscale = true;
            }
            
            // 'Ctrl+D' - Toggle downsampling (only on initial press)
            if (dKeyPressed && !appState.dKeyPressedLastFrame) {
                appState.enableDownsampling = !appState.enableDownsampling;
                appState.hilbertXCache.clear();
                if (appState.dataLoaded) {
                    // Reload all selected files with new downsampling setting while preserving selection
                    std::vector<InterferogramData> reloadedData;
                    for (const auto& filePath : appState.selectedFiles) {
                        try {
                            InterferogramData data = appState.currentAdapter->loadFile(filePath);
                            
                            // Apply downsampling if enabled and dataset is large
                            if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                
                                // Downsample both reference and primary detectors
                                std::vector<double> downsampledRef, downsampledPrim;
                                for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[j]);
                                    downsampledPrim.push_back(data.primaryDetector[j]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            
                            reloadedData.push_back(data);
                        } catch (const std::exception& e) {
                            std::cerr << "Error reloading file: " << e.what() << std::endl;
                        }
                    }
                    
                    if (!reloadedData.empty()) {
                        appState.loadedData = reloadedData;
                        // Also update raw data cache - need to reload raw data
                        // IMPORTANT: We need to reload the ORIGINAL raw data, not the processed data
                        appState.rawDataCache.clear();
                        size_t reloadedIdx = 0;
                        for (const auto& file : appState.selectedFiles) {
                            try {
                                InterferogramData rawData = appState.currentAdapter->loadFile(file);
                                appState.rawDataCache.push_back(rawData);
                            } catch (const std::exception& e) {
                                std::cerr << "Error reloading raw data for spectrum: " << e.what() << std::endl;
                                if (reloadedIdx < reloadedData.size())
                                    appState.rawDataCache.push_back(reloadedData[reloadedIdx]);
                            }
                            reloadedIdx++;
                        }
                        // Force X-axis to show all data when downsampling is toggled (same behavior as menu)
                        appState.zoomRange = {0, 0};
                        appState.shouldAutoscale = true;
                        appState.forceXAutofit = true; // Set global flag to force X-axis autofit
                        std::cout << "Reloaded " << appState.loadedData.size() << " datasets with " 
                                  << (appState.enableDownsampling ? "enabled" : "disabled") << " downsampling" << std::endl;
                    }
                }
            }
            
            // 'Ctrl+Q' - Toggle tracking cursor (only on initial press)
            if (qKeyPressed && !appState.qKeyPressedLastFrame) {
                appState.spectrum.showTrackingCursor = !appState.spectrum.showTrackingCursor;
                appState.needsRedraw = true;
            }

            // Update key state tracking for next frame
            appState.yKeyPressedLastFrame = yKeyPressed;
            appState.aKeyPressedLastFrame = aKeyPressed;
            appState.dKeyPressedLastFrame = dKeyPressed;
            appState.qKeyPressedLastFrame = qKeyPressed;
        } else {
            // Reset key states when keyboard is captured (e.g., typing in text field)
            appState.yKeyPressedLastFrame = false;
            appState.aKeyPressedLastFrame = false;
            appState.qKeyPressedLastFrame = false;
        }

        // Reapply UI scaling if size changed
        handleUIScaling(io, appState.uiScale, appState.currentUiSize, appState.uiSizeChanged);

        // Apply accent color theme if changed
        if (appState.accentColorChanged) {
            ImGuiStyle& style = ImGui::GetStyle();
            ImPlotStyle& plotStyle = ImPlot::GetStyle();
            ApplyTheme(style, plotStyle, StringToAccentColor(appState.currentAccentColor));
            applyWindowIcon(window, GetAccentBase(StringToAccentColor(appState.currentAccentColor)));
            appState.accentColorChanged = false;
            appState.needsRedraw = true;
        }
        
        // Calculate FPS
        float currentTime = ImGui::GetTime();
        appState.frameCount++;
        if (currentTime - appState.lastTime >= 1.0f) {
            appState.fps = static_cast<float>(appState.frameCount) / (currentTime - appState.lastTime);
            appState.frameCount = 0;
            appState.lastTime = currentTime;
        }
        
        // Track window state changes
        handleWindowEvents(window, config);
        
        // Update sorted files list for keyboard navigation
        appState.sortedFiles = appState.csvFiles;
        std::sort(appState.sortedFiles.begin(), appState.sortedFiles.end(), [](const std::string& a, const std::string& b) {
            std::string nameA = a;
            std::string nameB = b;
            size_t last_slash_a = nameA.find_last_of("/\\");
            size_t last_slash_b = nameB.find_last_of("/\\");
            if (last_slash_a != std::string::npos) nameA = nameA.substr(last_slash_a + 1);
            if (last_slash_b != std::string::npos) nameB = nameB.substr(last_slash_b + 1);
            return naturalSortCompare(nameA, nameB);
        });
        
        // Ensure averaging checkboxes match the sorted files size
        if (appState.filesSelectedForAveraging.size() != appState.sortedFiles.size()) {
            appState.filesSelectedForAveraging.clear();
            appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);
        }

        // Handle keyboard navigation for file selection
        handleKeyboardNavigation(appState.csvFiles, appState.currentSortedFileIndex, appState.filesChanged, appState.keyboardNavigation, 
                                appState.shiftSelectMode, appState.selectedFiles, appState.selectedFilenames, appState.loadedData, appState.rawDataCache, appState.dataLoaded, 
                                appState.sortedFiles, appState.enableDownsampling, appState.maxPointsBeforeDownsampling, appState.MAX_SELECTABLE_FILES);
        

        

        
        // Handle Delete key to remove currently navigated file
        // (OpenPopup is deferred to the Files panel, after NewFrame)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete) &&
            !appState.sortedFiles.empty()) {
            if (appState.skipDeleteConfirm) {
                performFileDeletion(appState, appState.currentSortedFileIndex);
            } else {
                appState.deleteConfirmIndex = appState.currentSortedFileIndex;
                appState.showDeleteConfirmPopup = true;
                appState.needsRedraw = true;
            }
        }

        // Space toggles the selection checkbox for all highlighted files
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
            !ImGui::GetIO().WantCaptureKeyboard &&
            ImGui::IsKeyPressed(ImGuiKey_Space) &&
            appState.dataLoaded) {
            for (const auto& selFile : appState.selectedFiles) {
                auto it = std::find(appState.sortedFiles.begin(), appState.sortedFiles.end(), selFile);
                if (it != appState.sortedFiles.end()) {
                    size_t idx = std::distance(appState.sortedFiles.begin(), it);
                    if (idx < appState.filesSelectedForAveraging.size())
                        appState.filesSelectedForAveraging[idx] = !appState.filesSelectedForAveraging[idx];
                }
            }
            appState.needsRedraw = true;
        }

        // Handle Ctrl+H to return to welcome screen
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && ImGui::IsKeyPressed(ImGuiKey_H) && ImGui::GetIO().KeyCtrl) {
            // Reset to welcome screen state
            appState.showWelcomeScreen = true;
            appState.welcomeScreenInitialized = false;
            appState.dataLoaded = false;
            appState.filesChanged = false;
            appState.loadedData.clear();
            appState.selectedFiles.clear();
            appState.selectedFilenames.clear();
            appState.rawDataCache.clear();
    appState.hilbertXCache.clear();
    appState.hilbertCacheLaserWavelength = 0.0f;
    appState.spectrum.cachedSpectra.clear();
    appState.spectrum.cachedFrequencies.clear();
    appState.spectrum.lastPrimaryDetectors.clear();
    appState.spectrum.lastSpectrumParams.clear();
    appState.spectrum.pendingSpectra_.clear();
    appState.clearAverageSpectrum();
            appState.clearSnrSpectrum();
            appState.clearAllanVariance();
            appState.needsRedraw = true;
        }

        // 'Left/Right Arrow' - Pan left by 10% of current visible range
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS && !appState.leftArrowPressedLastFrame) {

            appState.leftArrowPressedLastFrame = true;
            appState.leftArrowHandleFlag = true;
        }
        else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_RELEASE) {
            appState.leftArrowPressedLastFrame = false;
            appState.leftArrowHandleFlag = false;
        }

        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS && !appState.rightArrowPressedLastFrame) {

            appState.rightArrowPressedLastFrame = true;
            appState.rightArrowHandleFlag = true;
        }
        else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_RELEASE) {
            appState.rightArrowPressedLastFrame = false;
            appState.rightArrowHandleFlag = false;
        }

        // Load file if navigation changed
        if (appState.filesChanged && !appState.csvFiles.empty() && appState.currentAdapter) {
            try {
                // Load the currently selected file
                InterferogramData data = appState.currentAdapter->loadFile(appState.sortedFiles[appState.currentSortedFileIndex]);
                

                
                // Store raw data in cache before any processing for spectrum computation
                appState.rawDataCache.clear();
                appState.rawDataCache.push_back(data);
                
                // Create a copy for processing (downsampling, etc.)
                InterferogramData processedData = data;
                
                // Apply downsampling if enabled and dataset is large
                if (appState.enableDownsampling && processedData.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                    size_t localDownsampleFactor = processedData.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                    
                    // Downsample both reference and primary detectors
                    std::vector<double> downsampledRef, downsampledPrim;
                    for (size_t i = 0; i < processedData.referenceDetector.size(); i += localDownsampleFactor) {
                        downsampledRef.push_back(processedData.referenceDetector[i]);
                        downsampledPrim.push_back(processedData.primaryDetector[i]);
                    }
                    processedData.referenceDetector = downsampledRef;
                    processedData.primaryDetector = downsampledPrim;
                    std::cout << "Downsampled dataset from " << (downsampledRef.size() * localDownsampleFactor) 
                              << " to " << downsampledRef.size() << " points (factor: " << localDownsampleFactor << ")" << std::endl;
                }
                
                // For single selection (no Ctrl), replace current selection
                appState.loadedData.clear();
                // DON'T clear raw data cache - we need it for spectrum calculation!
                // appState.rawDataCache.clear(); // Clear raw data cache too
                appState.selectedFiles.clear();
                appState.selectedFilenames.clear();
                
                // Always load the processed data
                appState.loadedData.push_back(processedData);
                // Raw data is already in cache from line 718, no need to add again
                // appState.rawDataCache.push_back(data); // Store raw data for spectrum computation

                appState.selectedFiles.push_back(appState.sortedFiles[appState.currentSortedFileIndex]);
                
                // Extract filename for legend
                std::string filename = appState.sortedFiles[appState.currentSortedFileIndex];
                size_t last_slash = filename.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    filename = filename.substr(last_slash + 1);
                }
                appState.selectedFilenames.push_back(filename);
                
                // Update current dataset name (extract from current directory path)
                std::string dirPath = appState.currentDirectory;
                size_t dir_last_slash = dirPath.find_last_of("/\\");
                if (dir_last_slash != std::string::npos) {
                    appState.currentDatasetName = dirPath.substr(dir_last_slash + 1);
                    // If this is "raw_data", get the parent directory name
                    if (appState.currentDatasetName == "raw_data" && dir_last_slash > 0) {
                        size_t parent_slash = dirPath.substr(0, dir_last_slash).find_last_of("/\\");
                        if (parent_slash != std::string::npos) {
                            appState.currentDatasetName = dirPath.substr(parent_slash + 1, dir_last_slash - parent_slash - 1);
                        }
                    }
                }
                
                appState.dataLoaded = true;
                appState.needsRedraw = true;
                
                // Handle autoscale behavior based on AGENTS.md requirements:
                // "when the application loads a file for display for the first time after launch or work directory switch, axes zoom to fit all data."
                if (appState.isFirstDataLoad) {
                    appState.zoomRange = {0, 0};
                    appState.shouldAutoscale = true; // Trigger autoscale
                    
                    // Recalculate Y-axis limits from the actual data for autoscale
                    if (!data.referenceDetector.empty()) {
                        auto ref_min_max = std::minmax_element(data.referenceDetector.begin(), data.referenceDetector.end());
                        appState.ref_y_min = *ref_min_max.first;
                        appState.ref_y_max = *ref_min_max.second;
                    }
                    auto prim_min_max = std::minmax_element(data.primaryDetector.begin(), data.primaryDetector.end());
                    appState.prim_y_min = *prim_min_max.first;
                    appState.prim_y_max = *prim_min_max.second;
                    
                    // Reset first load flag after handling
                    appState.isFirstDataLoad = false;
                }
                appState.filesChanged = false;
                
                // Add parent directory to recent datasets from the loaded file path
                if (!appState.selectedFiles.empty()) {
                    std::string datasetPath = appState.selectedFiles[0];
                    size_t last_slash = datasetPath.find_last_of("/\\");
                    if (last_slash != std::string::npos) {
                        std::string parentDir = datasetPath.substr(0, last_slash);
                        size_t raw_data_pos = parentDir.find_last_of("/\\");
                        if (raw_data_pos != std::string::npos &&
                            parentDir.substr(raw_data_pos + 1) == "raw_data") {
                            parentDir = parentDir.substr(0, raw_data_pos);
                        }
                        addToRecentDatasets(config, configFilePath, parentDir, appState.currentAdapter ? appState.currentAdapter->getName() : "");
                    }
                }
                
            } catch (const std::exception& e) {
                std::cerr << "Error loading file: " << e.what() << std::endl;
                appState.dataLoaded = false;
                appState.filesChanged = false;
            }
        }
        
        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Rate-limit mouse wheel to at most one notch per frame, with
        // carry-over for excess. Prevents extreme zoom from batch-drained
        // events without dropping input entirely.
        {
            ImGuiIO& io = ImGui::GetIO();

            // Direction reversal: if carry-over and fresh events have opposite
            // signs, the user changed scroll direction. Reset carry-over so
            // zoom follows the new direction immediately.
            if (scrollCarryOverY != 0.0f && io.MouseWheel != 0.0f &&
                (scrollCarryOverY > 0.0f) != (io.MouseWheel > 0.0f))
                scrollCarryOverY = 0.0f;
            if (scrollCarryOverX != 0.0f && io.MouseWheelH != 0.0f &&
                (scrollCarryOverX > 0.0f) != (io.MouseWheelH > 0.0f))
                scrollCarryOverX = 0.0f;

            float totalY = scrollCarryOverY + io.MouseWheel;
            float totalX = scrollCarryOverX + io.MouseWheelH;

            io.MouseWheel  = std::clamp(totalY, -1.0f, 1.0f);
            io.MouseWheelH = std::clamp(totalX, -1.0f, 1.0f);

            // Cap carry-over to ±60 (~1 s at 60 fps) so long freezes
            // don't cause minutes of delayed zoom after input stops.
            scrollCarryOverY = std::clamp(totalY - io.MouseWheel, -60.0f, 60.0f);
            scrollCarryOverX = std::clamp(totalX - io.MouseWheelH, -60.0f, 60.0f);
        }

        // Keep rendering while carry-over drains
        if (scrollCarryOverY != 0.0f || scrollCarryOverX != 0.0f)
            appState.needsRedraw = true;

        // Conditionally disable anti-aliasing for large datasets (>50k points)
        if (appState.dataLoaded && appState.loadedData[0].dataSize() > 50000) {
            ImGui::GetStyle().AntiAliasedLines = false;
        }
        
        // Show welcome screen if no data is loaded and we haven't initialized yet
        if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
            bool showPopup = !appState.showAdapterSelectionPopup && !appState.showAdapterErrorPopup && !appState.showIncompatibleAdapterPopup;
            renderWelcomeScreen(appState, config, configFilePath, showPopup);
        }

        // Render adapter selection popup on top of welcome screen or main interface
        if (appState.showAdapterSelectionPopup) {
            ImGui::OpenPopup("Select Data Adapter##adapterSelect");
            appState.needsRedraw = true;
        }
        renderAdapterSelectionPopup();

        if (appState.showIncompatibleAdapterPopup) {
            ImGui::OpenPopup("Incompatible##incompatAdapter");
            appState.needsRedraw = true;
        }
        renderIncompatibleAdapterPopup();

        // Render adapter error popup
        if (appState.showAdapterErrorPopup) {
            ImGui::OpenPopup("Adapter Error##adapterError");
            appState.needsRedraw = true;
        }
        if (ImGui::BeginPopupModal("Adapter Error##adapterError", &appState.showAdapterErrorPopup,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", appState.adapterErrorMsg.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                appState.showAdapterErrorPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        
        // Only render main docking interface if welcome screen is not active
        if (appState.welcomeScreenInitialized) {
            // Set up docking
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::SetNextWindowViewport(viewport->ID);
            
            // Create ribbon menu first, before docking
            if (ImGui::BeginMainMenuBar())
            {
                // File menu
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Set Working Directory")) {
                        // Implement proper directory selection dialog
                        std::string selectedDirectory = FileBrowser::showDirectorySelectionDialog(window);
                        if (!selectedDirectory.empty()) {
                            // Check if the selected directory has a raw_data subdirectory
                            std::string rawDataPath = selectedDirectory + "/raw_data";
                            if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                                appState.currentDirectory = rawDataPath; // Use the raw_data subdirectory
                                // Update dataset name from parent directory
                                appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                            } else {
                                appState.currentDirectory = selectedDirectory; // Fallback to selected directory
                                // Update dataset name from selected directory
                                appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                            }
                            selectAdapterForDirectory(appState.currentDirectory);
                            addToRecentDatasets(config, configFilePath, selectedDirectory);
                            std::cout << "Working directory set to: " << appState.currentDirectory << std::endl;
                        }
                    }
                    
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
                                    if (std::filesystem::is_directory(datasetPath)) {
                                        std::string rawDataPath = datasetPath + "/raw_data";
                                        if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                                            appState.currentDirectory = rawDataPath;
                                        } else {
                                            appState.currentDirectory = datasetPath;
                                        }

                                        appState.currentDatasetName = datasetPath.substr(datasetPath.find_last_of("/\\") + 1);

                                        if (!entry.adapterName.empty() && AdapterRegistry::instance().getAdapter(entry.adapterName)) {
                                            applyAdapterSelection(entry.adapterName, appState.currentDirectory);
                                        } else {
                                            selectAdapterForDirectory(appState.currentDirectory);
                                        }
                                        std::cout << "Opened recent dataset: " << datasetPath << std::endl;
                                    } else {
                                        std::cerr << "Recent dataset path no longer exists: " << datasetPath << std::endl;
                                    }
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

            // Always create dockspace, but make background transparent when welcome screen is active
            // Push style variables for full viewport docking
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            // Make dockspace background transparent when welcome screen is active to show pattern
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            }
            
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            
            // Adjust window position to account for menu bar
            ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + ImGui::GetFrameHeight()));
            ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - ImGui::GetFrameHeight()));
            
            // Create main dockspace window
            ImGui::Begin("DockSpace", nullptr, window_flags);
            ImGui::PopStyleVar(2);
            
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PopStyleColor(); // Restore window background color
            }
            
            // Create docking space
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v2");

                // Handle manual layout restore request (from Settings menu)
                if (appState.restoreLayoutRequested) {
                    appState.restoreLayoutRequested = false;
                    rebuildDefaultLayout(dockspace_id);
                }

                // Apply default layout only on first launch (persisted via config)
                if (!appState.defaultLayoutApplied) {
                    appState.defaultLayoutApplied = true;
                    config.defaultLayoutApplied = true;
                    config.saveToFile(configFilePath);
                    rebuildDefaultLayout(dockspace_id);
                }

ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), 0);
                
                ImGui::End();
            
        // Only render panels when welcome screen is not active
        if (!appState.showWelcomeScreen || appState.welcomeScreenInitialized) {
        // Files panel (left)
        ImGui::Begin("Files");
        // Open delete confirmation popup if pending (called within frame context)
        if (appState.showDeleteConfirmPopup) {
            ImGui::OpenPopup("Delete File##confirm");
        }
        ImGui::PushTextWrapPos(); // Enable text wrapping
        ImGui::Text("Current Dataset: %s", appState.currentDatasetName.c_str());
        ImGui::Separator();
        ImGui::Text("Select:");
        ImGui::SameLine();
        if (ImGui::Button("All##FilesAll")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = true;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("None##FilesNone")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = false;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("10")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = (i < 10);
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("50%")) {
            size_t half = appState.filesSelectedForAveraging.size() / 2;
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = (i < half);
            appState.needsRedraw = true;
            }

            ImGui::Separator();
        ImGui::BeginChild("##FileList", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // Use the pre-sorted files list
        size_t currentSortedIndex = appState.currentSortedFileIndex;

        // Only calculate scroll position when using keyboard navigation
        if (appState.keyboardNavigation) {
            if (currentSortedIndex > 0 && ImGui::GetScrollY() + ImGui::GetWindowHeight() < (currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing()) {
                ImGui::SetScrollY((currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight());
            } else if (currentSortedIndex == 0) {
                ImGui::SetScrollY(0);
            }
        }
        
        for (size_t i = 0; i < appState.sortedFiles.size(); ) {
            const auto& file = appState.sortedFiles[i];
            // Extract just the filename without path
            std::string filename = file;
            size_t last_slash = filename.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                filename = filename.substr(last_slash + 1);
            }
            std::string fullFilename = filename;
            filename = shortenFilename(filename);
            
            // Delete button (left) — unique label per row avoids needing PushID
            float btnH = ImGui::GetFrameHeight();
            std::string delBtnId = "×##del" + std::to_string(i);
            if (ImGui::Button(delBtnId.c_str(), ImVec2(btnH, btnH))) {
                if (appState.skipDeleteConfirm) {
                    performFileDeletion(appState, i);
                    continue;
                } else {
                    appState.deleteConfirmIndex = i;
                    appState.showDeleteConfirmPopup = true;
                }
            }
            
            ImGui::SameLine();
            
            ImGui::PushID(static_cast<int>(i));
            
            // Enhanced highlighting for the currently selected file
            int stylesPushed = 1; // Default: push 1 style
            bool isFileSelected = (std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file) != appState.selectedFiles.end());
            
            if (isFileSelected) {
                // Find the index of this file in the selectedFiles vector to determine its color
                auto it = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file);
                size_t fileIndex = std::distance(appState.selectedFiles.begin(), it);
                
                // Get the color matching the plot curve color
                ImVec4 buttonColor;
                ImVec4 hoverColor;
                
                if (fileIndex == 0) {
                    buttonColor = ImVec4(0.6f, 0.5f, 0.1f, 0.8f); // Dark yellow - matches plot
                    hoverColor = ImVec4(0.7f, 0.6f, 0.2f, 0.9f); // Lighter yellow on hover
                } else if (fileIndex == 1) {
                    buttonColor = ImVec4(0.75f, 0.05f, 0.05f, 0.8f); // #C00E0E - Red
                    hoverColor = ImVec4(0.85f, 0.15f, 0.15f, 0.9f); // Lighter red on hover
                } else if (fileIndex == 2) {
                    buttonColor = ImVec4(0.15f, 0.45f, 0.28f, 0.8f); // #257448 - Green
                    hoverColor = ImVec4(0.25f, 0.55f, 0.38f, 0.9f); // Lighter green on hover
                } else if (fileIndex == 3) {
                    buttonColor = ImVec4(0.07f, 0.29f, 0.59f, 0.8f); // #114A97 - Blue
                    hoverColor = ImVec4(0.17f, 0.39f, 0.69f, 0.9f); // Lighter blue on hover
                } else if (fileIndex == 4) {
                    buttonColor = ImVec4(0.5f, 0.5f, 0.5f, 0.8f); // Grey
                    hoverColor = ImVec4(0.6f, 0.6f, 0.6f, 0.9f); // Lighter grey on hover
                }
                
                ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White text
                stylesPushed = 3; // Selected: push 3 styles
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.5f)); // Default button color
            }
            
            // Compute widths: delete button already placed, then filename, then checkbox
            float chkWidth = ImGui::GetFrameHeight();
            float btnWidth = ImGui::GetContentRegionAvail().x - chkWidth - ImGui::GetStyle().ItemSpacing.x;
            
            if (ImGui::Button(filename.c_str(), ImVec2(btnWidth, 0))) {
                // Handle multi-select with Ctrl key
                if (appState.multiSelectMode) {
                    // Toggle selection for this file
                    std::string fullPath = appState.sortedFiles[i];
                    auto it = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), fullPath);
                    if (it != appState.selectedFiles.end()) {
                        // File already selected, remove it
                        size_t index = std::distance(appState.selectedFiles.begin(), it);
                        appState.selectedFiles.erase(appState.selectedFiles.begin() + index);
                        appState.selectedFilenames.erase(appState.selectedFilenames.begin() + index);
                        appState.loadedData.erase(appState.loadedData.begin() + index);
                        appState.rawDataCache.erase(appState.rawDataCache.begin() + index); // Also remove from raw data cache
                    } else {
                        // Check if we would exceed the limit
                        if (appState.selectedFiles.size() < appState.MAX_SELECTABLE_FILES) {
                            try {
                                InterferogramData data = appState.currentAdapter->loadFile(fullPath);
        
                                
                                // Store raw data in cache before downsampling
                                InterferogramData rawData = data;
                                
                                // Apply downsampling to multi-selected files too
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                    
                                    // Downsample both reference and primary detectors
                                    std::vector<double> downsampledRef, downsampledPrim;
                                    for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                        downsampledRef.push_back(data.referenceDetector[j]);
                                        downsampledPrim.push_back(data.primaryDetector[j]);
                                    }
                                    data.referenceDetector = downsampledRef;
                                    data.primaryDetector = downsampledPrim;
                                }
                                
                                appState.loadedData.push_back(data);
                                appState.rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                                appState.selectedFiles.push_back(fullPath);
                                appState.selectedFilenames.push_back(filename);
                            } catch (const std::exception& e) {
                                std::cerr << "Error loading file: " << e.what() << std::endl;
                            }
                        } else {
                            ImGui::OpenPopup("Selection Limit");
                            appState.needsRedraw = true;
                        }
                    }
                } else if (appState.shiftSelectMode) {
                    // Handle Shift+Click for range selection
                    size_t startIndex = std::min(appState.lastSelectedIndex, i);
                    size_t endIndex = std::max(appState.lastSelectedIndex, i);
                    
                    // Clear current selection
                    appState.selectedFiles.clear();
                    appState.selectedFilenames.clear();
                    appState.loadedData.clear();
                    appState.rawDataCache.clear(); // Clear raw data cache too
                    
                    // Add all files in the range, respecting the 5-file limit
                    size_t filesToAdd = 0;
                    for (size_t j = startIndex; j <= endIndex; j++) {
                        if (filesToAdd >= appState.MAX_SELECTABLE_FILES) break;
                        
                        try {
                            std::string fullPath = appState.sortedFiles[j];
                            InterferogramData data = appState.currentAdapter->loadFile(fullPath);
                            
                            // Store raw data in cache before downsampling
                            InterferogramData rawData = data;
                            
                            // Apply downsampling
                            if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                std::vector<double> downsampledRef, downsampledPrim;
                                for (size_t k = 0; k < data.referenceDetector.size(); k += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[k]);
                                    downsampledPrim.push_back(data.primaryDetector[k]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            
                            appState.loadedData.push_back(data);
                            appState.rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                            appState.selectedFiles.push_back(fullPath);
                            
                            // Extract filename for legend
                            std::string filename = appState.sortedFiles[j];
                            size_t last_slash = filename.find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                filename = filename.substr(last_slash + 1);
                            }
                            appState.selectedFilenames.push_back(filename);
                            filesToAdd++;
                        } catch (const std::exception& e) {
                            std::cerr << "Error loading file: " << e.what() << std::endl;
                        }
                    }
                    
                    // Update last selected index
                    appState.lastSelectedIndex = i;
                    appState.dataLoaded = !appState.selectedFiles.empty();
                } else {
                    // Single selection - replace current selection
                    appState.currentSortedFileIndex = i;
                    appState.filesChanged = true;
                    // Update last selected index for future Shift+Click
                    appState.lastSelectedIndex = i;
                }
            }
            
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fullFilename.c_str());
            
            // Pop the correct number of styles
            ImGui::PopStyleColor(stylesPushed);
            
            // Averaging checkbox (right-aligned)
            ImGui::SameLine();
            if (i < appState.filesSelectedForAveraging.size()) {
                bool chk = appState.filesSelectedForAveraging[i];
                bool wasUnchecked = !chk;
                if (wasUnchecked) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
                }
                ImGui::PushID(static_cast<int>(i + 100000));
                if (ImGui::Checkbox("##AvgSel", &chk)) {
                    appState.filesSelectedForAveraging[i] = chk;
                    appState.needsRedraw = true;
                }
                ImGui::PopID();
                if (wasUnchecked) {
                    ImGui::PopStyleColor(2);
                }
            }

            // Auto-scroll to keep selected item visible only when selection changes via keyboard
            if (i == appState.currentSortedFileIndex && appState.keyboardNavigation) {
                ImGui::SetScrollHereY(0.5f); // Scroll to center the selected item
            }

            ImGui::PopID();
            i++;
        }
        ImGui::EndChild();

        // Show selection limit popup if needed
        if (ImGui::BeginPopupModal("Selection Limit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Maximum of %zu files can be selected at once.", appState.MAX_SELECTABLE_FILES);
            ImGui::Text("Please deselect some files first.");
            
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }

        // Delete confirmation popup
        {
            static int focusIdx = 0;
            static bool prevPopupOpen = false;
            if (!appState.showDeleteConfirmPopup)
                prevPopupOpen = false;

            if (ImGui::BeginPopupModal("Delete File##confirm", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                size_t idx = appState.deleteConfirmIndex;
                std::string fname = idx < appState.sortedFiles.size()
                    ? appState.sortedFiles[idx].substr(appState.sortedFiles[idx].find_last_of("/\\") + 1)
                    : "";

                ImGui::Text("Are you sure you want to delete?");
                ImGui::TextWrapped("%s", fname.c_str());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter);

                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && focusIdx > 0)
                    focusIdx--;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && focusIdx < 2)
                    focusIdx++;

                // Cancel
                if (focusIdx == 0)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                    (enterPressed && prevPopupOpen && focusIdx == 0)) {
                    if (focusIdx == 0) ImGui::PopStyleColor();
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                } else if (focusIdx == 0) {
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();

                // Yes
                if (focusIdx == 1)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Yes") ||
                    (enterPressed && prevPopupOpen && focusIdx == 1)) {
                    if (focusIdx == 1) ImGui::PopStyleColor();
                    if (idx < appState.sortedFiles.size())
                        performFileDeletion(appState, idx);
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                } else if (focusIdx == 1) {
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();

                // Yes, don't ask again
                if (focusIdx == 2)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button("Yes, don't ask again") ||
                    (enterPressed && prevPopupOpen && focusIdx == 2)) {
                    if (focusIdx == 2) ImGui::PopStyleColor();
                    appState.skipDeleteConfirm = true;
                    if (idx < appState.sortedFiles.size())
                        performFileDeletion(appState, idx);
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                } else if (focusIdx == 2) {
                    ImGui::PopStyleColor();
                }

                ImGui::EndPopup();
                prevPopupOpen = true;
            }
        }
        
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();
        
        // Interferogram panel (main)
        bool isMainWindowFocused = false;
        ImGui::Begin("Interferogram View");
        isMainWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        
        // Handle ESC key to reset zoom (only when main window is focused)
        if (isMainWindowFocused && appState.dataLoaded && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Reset X-axis zoom when ESC is pressed (only for main window)
            appState.zoomRange = {0, 0};
            appState.shouldAutoscale = true; // Always force redraw with full range when ESC is pressed
        }
        
        if (appState.dataLoaded && !appState.loadedData.empty()) {
            if (!appState.datasetInfo.hasInterferograms) {
                ImGui::Text("Interferograms not available for this data type.");
            } else {
            // Y-axis limits are now handled by the auto-fit toggle
            // When autoFitYAxis is true, ImPlot will auto-calculate Y-axis limits
            // When autoFitYAxis is false, we use the manually calculated limits
            
            // Determine zoom range
            size_t ref_start =  0;
            size_t ref_end =  appState.datasetInfo.hasReferenceChannel
                              ? appState.loadedData[0].referenceDetector.size()
                              : appState.loadedData[0].dataSize();
            size_t prim_start =  0;
            size_t prim_end =  appState.loadedData[0].primaryDetector.size();
            // Compute peak positions for X-axis alignment (from raw data for OPD accuracy)
            std::vector<size_t> peakPositions;
            if (appState.maxAtZero) {
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    const auto& prim = (i < appState.rawDataCache.size() && !appState.rawDataCache[i].primaryDetector.empty())
                        ? appState.rawDataCache[i].primaryDetector
                        : appState.loadedData[i].primaryDetector;
                    auto peakIt = std::max_element(prim.begin(), prim.end());
                    peakPositions.push_back(static_cast<size_t>(std::distance(prim.begin(), peakIt)));
                }
            }
            // Map raw peak index to downsampled space for sample-mode X-axis shifts
            auto getDsPeak = [&](size_t i) -> size_t {
                if (!appState.enableDownsampling || i >= appState.rawDataCache.size()) return peakPositions[i];
                const auto& rawPrim = appState.rawDataCache[i].primaryDetector;
                if (rawPrim.empty()) return peakPositions[i];
                size_t rs = rawPrim.size();
                size_t ls = appState.loadedData[i].primaryDetector.size();
                if (rs <= ls || ls == 0) return peakPositions[i];
                return static_cast<size_t>(static_cast<double>(peakPositions[i]) * ls / rs + 0.5);
            };
            
            if (appState.loadedData.size() > 1) {
                ImGui::BeginGroup(); // Start horizontal group
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    ImVec4 color;
                    // Assign same colors as used in plots
                    if (i == 0) {
                        color = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                    } else if (i == 1) {
                        color = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                    } else if (i == 2) {
                        color = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                    } else if (i == 3) {
                        color = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                    } else if (i == 4) {
                        color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                    }
                    
                    // Draw colored square patch
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                    ImVec2 square_size(12, 12); // Size of the color square
                    
                    // Draw square patch
                    draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(color));
                    draw_list->AddRect(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f))); // Border
                    
                    // Move cursor forward and add text
                    ImGui::Dummy(square_size);
                    ImGui::SameLine();
                    ImGui::Text("%s", appState.selectedFilenames[i].c_str());
                    if (i < appState.loadedData.size() - 1) {
                        ImGui::SameLine();
                        ImGui::Text("  "); // Add some spacing between items
                        ImGui::SameLine();
                    }
                }
                ImGui::EndGroup(); // End horizontal group
                ImGui::Separator();
            }
            
            // Create ImPlot subplots - two vertically stacked plots with custom height ratio
            // Reference plot: 1 unit height, Primary plot: 2 units height (2x taller)
            const bool hasRef = appState.datasetInfo.hasReferenceChannel;
            float row_ratios[2] = {1.0f, 2.0f};
            float row_ratios1[1] = {1.0f};
            int numRows = hasRef ? 2 : 1;
            
            // Pre-allocate plot specs to avoid repeated construction in rendering loop
            std::vector<ImPlotSpec> plotSpecs;
            if (appState.dataLoaded) {
                plotSpecs.resize(appState.loadedData.size());
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    plotSpecs[i].LineWeight = 2.0f;
                    
                    // Assign specific colors based on the requested scheme (yellow first)
                    if (i == 0) {
                        plotSpecs[i].LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                    } else if (i == 1) {
                        plotSpecs[i].LineColor = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                    } else if (i == 2) {
                        plotSpecs[i].LineColor = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                    } else if (i == 3) {
                        plotSpecs[i].LineColor = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                    } else if (i == 4) {
                        plotSpecs[i].LineColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                    }
                }
            }

            // Handle box selection zoom manually using ImGui mouse input
            // This completely bypasses ImPlot's input system
            ImVec2 mousePos = ImGui::GetMousePos();
            bool isOverPlot = ImGui::IsWindowHovered();
            appState.isMouseOverPlot = isOverPlot;

            // Handle X-range selection with Shift key - state management only (only when main window is focused)
            bool shiftPressed = ImGui::GetIO().KeyShift;
            if (isMainWindowFocused && isOverPlot && shiftPressed && !appState.isSelectingXRange) {
                // Start selection when Shift is pressed over plot
                appState.isSelectingXRange = true;
                // Reset selection positions
                appState.selectionStartX = 0.0;
                appState.selectionEndX = 0.0;
                std::cout << "DEBUG: Started X-range selection" << std::endl;
            } else if (!shiftPressed && appState.isSelectingXRange) {
                // End selection when Shift is released
                appState.isSelectingXRange = false;
                
                // Only finalize if we have valid selection
                if(appState.selectionStartX != appState.selectionEndX) {
                    appState.applyXRangeSelection = true;
                    
                    if(appState.selectionStartX > appState.selectionEndX)
                    {
                        // make sure start is always smaller
                        double dum = appState.selectionStartX;
                        appState.selectionStartX = appState.selectionEndX;
                        appState.selectionEndX = dum;
                    }
                    
                    std::cout << "DEBUG: Finalizing X-range selection: Start=" << appState.selectionStartX << ", End=" << appState.selectionEndX << std::endl;
                } else {
                    std::cout << "DEBUG: X-range selection cancelled (no valid range)" << std::endl;
                }
            }

            
            // Ensure X-axis cache is populated for OPD mode or peak-finding
            // (runs before subplots regardless of hasRef)
            if ((appState.xAxisBase == 1 || appState.xCorrectionMethod == 1) && appState.dataLoaded) {
                if (appState.datasetInfo.axisIsCorrected) {
                    for (size_t i = 0; i < appState.loadedData.size(); i++) {
                        const std::string& fileId = appState.selectedFilenames[i];
                        if (appState.hilbertXCache.find(fileId) == appState.hilbertXCache.end()) {
                            const auto& opd = appState.loadedData[i].opdAxis;
                            if (!opd.empty()) {
                                std::vector<double> hilbX(opd.size());
                                for (size_t j = 0; j < opd.size(); j++)
                                    hilbX[j] = opd[j] * 1e6;
                                appState.hilbertXCache[fileId] = std::move(hilbX);
                            }
                        }
                    }
                } else {
                    if (appState.hilbertCacheLaserWavelength != appState.spectrum.refLaserTextbox) {
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.hilbertCacheLaserWavelength = appState.spectrum.refLaserTextbox;
                    }
                    for (size_t i = 0; i < appState.loadedData.size(); i++) {
                        const std::string& fileId = appState.selectedFilenames[i];
                        if (appState.hilbertXCache.find(fileId) == appState.hilbertXCache.end()) {
                            std::vector<double> hilbX;
                            // Always use full-resolution raw data for computation
                            const auto& refDet = (i < appState.rawDataCache.size())
                                ? appState.rawDataCache[i].referenceDetector
                                : appState.loadedData[i].referenceDetector;
                            if (appState.xCorrectionMethod == 1) {
                                std::vector<size_t> peakIdxs;
                                SpectralToolbox::xAxisFromPeaks(
                                    refDet, appState.hilbertCacheLaserWavelength,
                                    appState.peakProminenceThreshold,
                                    hilbX, &peakIdxs);
                                if (!hilbX.empty()) {
                                    appState.peakPositionsCache[fileId] = std::move(peakIdxs);
                                }
                            } else {
                                SpectralToolbox::xAxisFromHilbert(refDet, appState.hilbertCacheLaserWavelength, hilbX);
                            }
                            if (!hilbX.empty()) {
                                appState.hilbertXCache[fileId] = std::move(hilbX);
                            }
                        }
                    }
                }
            }

            if (ImPlot::BeginSubplots("Detector Plots", numRows, 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend, hasRef ? row_ratios : row_ratios1)) {

                if (hasRef) {
                // Reference detector plot (top)
                ImPlotFlags ref_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
                if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                    ref_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }
                // Never show crosshairs
                {
                    ImVec4 refGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
                    refGridCol.w *= appState.gridAlpha;
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, refGridCol);
                }
                if (ImPlot::BeginPlot("Reference", ImVec2(-1, -1), ref_flags)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (appState.autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }
                    const char* refXLabel = (appState.xAxisBase == 1) ? "OPD [\xC2\xB5m]" : "Sample num";
                    ImPlot::SetupAxes(refXLabel, "Voltage [V]", ImPlotAxisFlags_NoTickMarks, y_flags);
                    // Conditionally optimize grid rendering for large datasets
                    if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.6f, 0.6f, 0.6f, 0.75f * appState.gridAlpha));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    if (appState.shouldAutoscale || appState.forceXAutofit) {
                        // Set initial view to show all data when new data is loaded or when downsampling is toggled
                        if (appState.xAxisBase == 1 && appState.dataLoaded) {
                            double xMin = std::numeric_limits<double>::max();
                            double xMax = std::numeric_limits<double>::lowest();
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[i]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && i < peakPositions.size()) ? hx[peakPositions[i]] : 0.0;
                                    xMin = std::min(xMin, hx.front() - off);
                                    xMax = std::max(xMax, hx.back() - off);
                                }
                            }
                            if (xMin < xMax) {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(xMin, xMax, appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                }
                            }
                        } else {
                            if (appState.maxAtZero && !peakPositions.empty()) {
                                double xMin = std::numeric_limits<double>::max();
                                double xMax = std::numeric_limits<double>::lowest();
                                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                    double N = static_cast<double>(appState.loadedData[i].referenceDetector.size());
                                    double off = static_cast<double>(getDsPeak(i));
                                    xMin = std::min(xMin, -off);
                                    xMax = std::max(xMax, N - 1.0 - off);
                                }
                                if (xMin < xMax) {
                                    if (!appState.autoFitYAxis) {
                                        ImPlot::SetupAxesLimits(xMin, xMax, appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                    } else {
                                        ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                    }
                                }
                            } else {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(0, appState.loadedData[0].referenceDetector.size(), appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].referenceDetector.size(), ImPlotCond_Always);
                                }
                            }
                        }
                        // Reset the force flag after use
                        if (appState.forceXAutofit) {
                            appState.forceXAutofit = false;
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (appState.applyXRangeSelection && appState.selectionStartX != appState.selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, appState.selectionStartX, appState.selectionEndX, ImPlotCond_Always);
                        appState.applyXRangeSelection = false; // Reset flag after applying
                    }
                    {
                        double xMin = appState.last_x_min;
                        double xMax = appState.last_x_max;
                        if (xMin >= xMax && appState.dataLoaded) {
                            if (appState.xAxisBase == 1) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[0]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && !peakPositions.empty()) ? hx[peakPositions[0]] : 0.0;
                                    xMin = hx.front() - off;
                                    xMax = hx.back() - off;
                                } else {
                                    xMin = 0.0;
                                    xMax = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                                }
                            } else if (appState.maxAtZero && !peakPositions.empty()) {
                                double N = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                                double off = static_cast<double>(getDsPeak(0));
                                xMin = -off;
                                xMax = N - 1.0 - off;
                            } else {
                                xMin = 0.0;
                                xMax = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                            }
                        }
                        float yMin = appState.last_ref_y_min;
                        float yMax = appState.last_ref_y_max;
                        if (yMin >= yMax) {
                            yMin = appState.ref_y_min;
                            yMax = appState.ref_y_max;
                        }
                        SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
                        SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
                    }
                    // Plot all selected datasets with pre-allocated specs
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].referenceDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& refData = appState.loadedData[i].referenceDetector;
                                if (ref_start < refData.size()) {
                                    size_t actual_count = std::min(data_count, refData.size() - ref_start);
                                    if (appState.xAxisBase == 1) {
                                        const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[i]];
                                        if (!hilbX.empty()) {
                                            // Map downsampled index to full-res OPD cache proportionally
                                            double ratio = static_cast<double>(hilbX.size()) / refData.size();
                                            auto mapX = [&](size_t j) -> double {
                                                size_t idx = static_cast<size_t>((ref_start + j) * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                return hilbX[idx];
                                            };
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = mapX(j) - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                std::vector<double> mappedX(actual_count);
                                                for (size_t j = 0; j < actual_count; j++)
                                                    mappedX[j] = mapX(j);
                                                ImPlot::PlotLine("", mappedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(getDsPeak(i));
                                        for (size_t j = 0; j < actual_count; j++)
                                            shiftedX[j] = static_cast<double>(static_cast<int>(ref_start + j) - peak);
                                        ImPlot::PlotLine("", shiftedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                    } else {
                                        ImPlot::PlotLine("", 
                                                       &refData[ref_start], 
                                                       actual_count, 1.0, 0.0, plotSpecs[i]);
                                    }
                                }
                            }
                        } else {
                            std::cout << "DEBUG: Invalid data range for plotting: start=" << ref_start << ", end=" << ref_end << ", size=" << appState.loadedData[0].referenceDetector.size() << std::endl;
                        }
                    }

                    // Peak markers overlay (peak-finding mode, sample-mode axis only)
                    if (appState.showPeakIndicators && appState.xCorrectionMethod == 1 && appState.xAxisBase == 0 && appState.dataLoaded) {
                        for (size_t i = 0; i < appState.loadedData.size(); i++) {
                            const std::string& fileId = appState.selectedFilenames[i];
                            auto pit = appState.peakPositionsCache.find(fileId);
                            if (pit == appState.peakPositionsCache.end() || pit->second.empty()) continue;
                            const auto& ref = (i < appState.rawDataCache.size() && !appState.rawDataCache[i].referenceDetector.empty())
                                ? appState.rawDataCache[i].referenceDetector
                                : appState.loadedData[i].referenceDetector;
                            double dsScale = 1.0;
                            if (appState.enableDownsampling && i < appState.rawDataCache.size() && !appState.rawDataCache[i].referenceDetector.empty()) {
                                size_t rawSize = appState.rawDataCache[i].referenceDetector.size();
                                size_t loadedSize = appState.loadedData[i].referenceDetector.size();
                                if (rawSize > loadedSize && loadedSize > 0)
                                    dsScale = static_cast<double>(loadedSize) / rawSize;
                            }
                            int xOffset = (appState.maxAtZero && i < peakPositions.size()) ? static_cast<int>(getDsPeak(i)) : 0;
                            std::vector<double> mx(pit->second.size());
                            std::vector<double> my(pit->second.size());
                            for (size_t j = 0; j < pit->second.size(); j++) {
                                size_t idx = pit->second[j];
                                mx[j] = static_cast<double>(idx) * dsScale - xOffset;
                                my[j] = ref[idx];
                            }
                            ImPlotSpec pkSpec;
                            pkSpec.Marker = ImPlotMarker_Circle;
                            pkSpec.MarkerSize = 4.0f;
                            pkSpec.MarkerFillColor = ImVec4(1,0,0,1);
                            ImPlot::PlotScatter("##PeakMarkers", mx.data(), my.data(), (int)mx.size(), pkSpec);
                        }
                    }
                    
                    // Handle X-range selection within plot context
                    if (appState.isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (appState.selectionStartX == 0.0 && appState.selectionEndX == 0.0) {
                            appState.selectionStartX = mousePos.x;
                        }
                        appState.selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(appState.selectionStartX, appState.selectionEndX);
                        double selection_right = std::max(appState.selectionStartX, appState.selectionEndX);
                        
                        // Create arrays for shaded region - need X array and two Y arrays (bottom and top)
                        double shade_x[2] = {selection_left, selection_right};
                        double shade_y1[2] = {y_min, y_min};  // Bottom edge
                        double shade_y2[2] = {y_max, y_max};  // Top edge
                        
                        // Create spec for dark purple translucent fill
                        ImPlotSpec fillSpec;
                        fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f); // Dark purple with 30% opacity
                        
                        // Draw translucent dark purple fill between selection lines
                        ImPlot::PlotShaded("##SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                        
                        // Create arrays for vertical line points
                        double start_x[2] = {appState.selectionStartX, appState.selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {appState.selectionEndX, appState.selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                                                
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    appState.last_ref_y_min = static_cast<float>(ImPlot::GetPlotLimits().Y.Min);
                    appState.last_ref_y_max = static_cast<float>(ImPlot::GetPlotLimits().Y.Max);
                    ImPlot::EndPlot();
                    if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                        ImPlot::PopStyleColor(); // Pop grid color only if we pushed it
                    }
                }
                ImPlot::PopStyleColor(); // Restore original grid color
                } // end of hasRef block

                
                // Primary detector plot (bottom)
                ImPlotFlags prim_flags = ImPlotFlags_NoTitle;
                if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                    prim_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }

                // Never show crosshairs
                {
                    ImVec4 primGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
                    primGridCol.w *= appState.gridAlpha;
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, primGridCol);
                }
                if (ImPlot::BeginPlot("Primary", ImVec2(-1, -1), prim_flags)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (appState.autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }

                    const char* primXLabel = (appState.xAxisBase == 1) ? "OPD [\xC2\xB5m]" : "Sample num";
                    ImPlot::SetupAxes(primXLabel, "Voltage [V]", ImPlotAxisFlags_NoTickMarks, y_flags);

                    // Conditionally optimize grid rendering for large datasets
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.6f, 0.6f, 0.6f, 0.75f * appState.gridAlpha));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    // Only apply arrow key navigation when main window is focused
                    if (isMainWindowFocused) {
                        if(appState.leftArrowHandleFlag) {
                            float translationAmount = (appState.last_x_max - appState.last_x_min) / 10;
                            ImPlot::SetupAxisLimits(ImAxis_X1, appState.last_x_min - translationAmount, appState.last_x_max - translationAmount, ImPlotCond_Always);
                            appState.leftArrowHandleFlag = false;
                        } else if(appState.rightArrowHandleFlag) {
                            float translationAmount = (appState.last_x_max - appState.last_x_min) / 10;
                            ImPlot::SetupAxisLimits(ImAxis_X1, appState.last_x_min + translationAmount, appState.last_x_max + translationAmount, ImPlotCond_Always);
                            appState.rightArrowHandleFlag = false;
                        }
                    }

                    if (appState.shouldAutoscale || appState.forceXAutofit) {
                        // Set initial view to show all data when new data is loaded or when downsampling is toggled
                        if (appState.xAxisBase == 1 && appState.dataLoaded) {
                            double xMin = std::numeric_limits<double>::max();
                            double xMax = std::numeric_limits<double>::lowest();
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[i]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && i < peakPositions.size()) ? hx[peakPositions[i]] : 0.0;
                                    xMin = std::min(xMin, hx.front() - off);
                                    xMax = std::max(xMax, hx.back() - off);
                                }
                            }
                            if (xMin < xMax) {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(xMin, xMax, appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                }
                            }
                        } else {
                            if (appState.maxAtZero && !peakPositions.empty()) {
                                double xMin = std::numeric_limits<double>::max();
                                double xMax = std::numeric_limits<double>::lowest();
                                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                    double N = static_cast<double>(appState.loadedData[i].primaryDetector.size());
                                    double off = static_cast<double>(getDsPeak(i));
                                    xMin = std::min(xMin, -off);
                                    xMax = std::max(xMax, N - 1.0 - off);
                                }
                                if (xMin < xMax) {
                                    if (!appState.autoFitYAxis) {
                                        ImPlot::SetupAxesLimits(xMin, xMax, appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                    } else {
                                        ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                    }
                                }
                            } else {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(0, appState.loadedData[0].primaryDetector.size(), appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].primaryDetector.size(), ImPlotCond_Always);
                                }
                            }
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (appState.applyXRangeSelection && appState.selectionStartX != appState.selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, appState.selectionStartX, appState.selectionEndX, ImPlotCond_Always);
                        appState.applyXRangeSelection = false; // Reset flag after applying
                    }
                    {
                        double xMin = appState.last_x_min;
                        double xMax = appState.last_x_max;
                        if (xMin >= xMax && appState.dataLoaded) {
                            if (appState.xAxisBase == 1) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[0]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && !peakPositions.empty()) ? hx[peakPositions[0]] : 0.0;
                                    xMin = hx.front() - off;
                                    xMax = hx.back() - off;
                                } else {
                                    xMin = 0.0;
                                    xMax = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                                }
                            } else if (appState.maxAtZero && !peakPositions.empty()) {
                                double N = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                                double off = static_cast<double>(getDsPeak(0));
                                xMin = -off;
                                xMax = N - 1.0 - off;
                            } else {
                                xMin = 0.0;
                                xMax = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                            }
                        }
                        float yMin = appState.last_prim_y_min;
                        float yMax = appState.last_prim_y_max;
                        if (yMin >= yMax) {
                            yMin = appState.prim_y_min;
                            yMax = appState.prim_y_max;
                        }
                        SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
                        SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
                    }
                    // Reuse the same plot specs as reference plot (already pre-allocated)
                    // Plot all selected datasets with same colors as reference
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].primaryDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& primData = appState.loadedData[i].primaryDetector;
                                if (ref_start < primData.size()) {
                                    size_t actual_count = std::min(data_count, primData.size() - ref_start);
                                    if (appState.xAxisBase == 1) {
                                        const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[i]];
                                        if (!hilbX.empty()) {
                                            // Map downsampled index to full-res OPD cache proportionally
                                            double ratio = static_cast<double>(hilbX.size()) / primData.size();
                                            auto mapX = [&](size_t j) -> double {
                                                size_t idx = static_cast<size_t>((ref_start + j) * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                return hilbX[idx];
                                            };
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = mapX(j) - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                std::vector<double> mappedX(actual_count);
                                                for (size_t j = 0; j < actual_count; j++)
                                                    mappedX[j] = mapX(j);
                                                ImPlot::PlotLine("", mappedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(getDsPeak(i));
                                        for (size_t j = 0; j < actual_count; j++)
                                            shiftedX[j] = static_cast<double>(static_cast<int>(ref_start + j) - peak);
                                        ImPlot::PlotLine("", shiftedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                    } else {
                                        ImPlot::PlotLine("", 
                                                       &primData[ref_start], 
                                                         actual_count, 1.0, 0.0, plotSpecs[i]);
                                    }
                                }
                            }
                        }
                        
                        // Draw apodization window overlay (spectrum view is always available)
                        if (appState.dataLoaded) {
                            const auto& primDataOverlay = appState.loadedData[0].primaryDetector;
                            if (!primDataOverlay.empty()) {
                                auto w = static_cast<ApodizationWindow>(appState.spectrum.apodizationSelector);
                                auto window = Apodization::createWindow(
                                    w, primDataOverlay.size(),
                                    std::max_element(primDataOverlay.begin(), primDataOverlay.end()) - primDataOverlay.begin(),
                                    appState.spectrum.apodizationParams);
                                double scale = *std::max_element(primDataOverlay.begin(), primDataOverlay.end());
                                if (scale > 0.0) {
                                    for (auto& v : window) v *= scale;
                                }
                                ImPlotSpec windowSpec;
                                windowSpec.LineColor = ImVec4(0.0f, 1.0f, 1.0f, 0.5f);
                                windowSpec.LineWeight = 2.0f;
                                if (appState.xAxisBase == 1 && !appState.selectedFilenames.empty()) {
                                    const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[0]];
                                    if (!hilbX.empty()) {
                                        double ratio = static_cast<double>(hilbX.size()) / primDataOverlay.size();
                                        std::vector<double> overlayX(window.size());
                                        if (appState.maxAtZero && !peakPositions.empty()) {
                                            double peakHilbX = hilbX[peakPositions[0]];
                                            for (size_t j = 0; j < window.size(); j++) {
                                                size_t idx = static_cast<size_t>(j * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                overlayX[j] = hilbX[idx] - peakHilbX;
                                            }
                                        } else {
                                            for (size_t j = 0; j < window.size(); j++) {
                                                size_t idx = static_cast<size_t>(j * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                overlayX[j] = hilbX[idx];
                                            }
                                        }
                                        ImPlot::PlotLine("##ApodWindow", overlayX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                    }
                                } else if (appState.maxAtZero && !peakPositions.empty()) {
                                    std::vector<double> shiftedX(window.size());
                                    int peak = static_cast<int>(getDsPeak(0));
                                    for (size_t j = 0; j < window.size(); j++)
                                        shiftedX[j] = static_cast<double>(static_cast<int>(j) - peak);
                                    ImPlot::PlotLine("##ApodWindow", shiftedX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                } else {
                                    ImPlot::PlotLine("##ApodWindow", window.data(), static_cast<int>(window.size()),
                                                     1.0, 0.0, windowSpec);
                                }
                            }
                        }
                    }

                    // Handle X-range selection within plot context
                    if (appState.isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (appState.selectionStartX == 0.0 && appState.selectionEndX == 0.0) {
                            appState.selectionStartX = mousePos.x;
                        }
                        appState.selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(appState.selectionStartX, appState.selectionEndX);
                        double selection_right = std::max(appState.selectionStartX, appState.selectionEndX);
                        
                        // Create arrays for shaded region - need X array and two Y arrays (bottom and top)
                        double shade_x[2] = {selection_left, selection_right};
                        double shade_y1[2] = {y_min, y_min};  // Bottom edge
                        double shade_y2[2] = {y_max, y_max};  // Top edge
                        
                        // Create spec for dark purple translucent fill
                        ImPlotSpec fillSpec;
                        fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f); // Dark purple with 30% opacity
                        
                        // Draw translucent dark purple fill between selection lines
                        ImPlot::PlotShaded("##SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                        
                        // Create arrays for vertical line points
                        double start_x[2] = {appState.selectionStartX, appState.selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {appState.selectionEndX, appState.selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    // Add "LARGE DATA" indicator for large datasets (>50k points)
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        // Use ImPlot's annotation system for reliable positioning
                        // Position at top-right of plot with small offset
                        ImPlot::Annotation(ImPlot::GetPlotLimits().X.Max, ImPlot::GetPlotLimits().Y.Max, 
                                         ImVec4(1.0f, 1.0f, 1.0f, 1.0f), ImVec2(-10, 10), true, "LARGE DATA");
                    }
                    
                    appState.last_x_max = ImPlot::GetPlotLimits().X.Max;
                    appState.last_x_min = ImPlot::GetPlotLimits().X.Min;
                    appState.last_prim_y_min = static_cast<float>(ImPlot::GetPlotLimits().Y.Min);
                    appState.last_prim_y_max = static_cast<float>(ImPlot::GetPlotLimits().Y.Max);

                    ImPlot::EndPlot();
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        ImPlot::PopStyleColor(); // Pop grid color only if we pushed it
                    }
                }
                ImPlot::PopStyleColor(); // Restore original grid color
                
                // Reset autoscale flag after use
                if (appState.shouldAutoscale) {
                    appState.shouldAutoscale = false;
                }
                
                ImPlot::EndSubplots();
            }
            
            
            } // end of hasInterferograms else block
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

// Spectrum panel (bottom)
        ImGui::Begin("Spectrum");
        if (appState.dataLoaded) {
            // Spectrum panel controls
            ImGui::Separator();


            // Lambda helper: invalidate spectrum caches when a control editing is finished
            auto invalidateSpectrumCaches = [&]() {
                appState.spectrum.cachedSpectra.clear();
                appState.spectrum.cachedFrequencies.clear();
                appState.spectrum.lastPrimaryDetectors.clear();
                appState.spectrum.lastSpectrumParams.clear();
                appState.spectrum.pendingSpectra_.clear();
                appState.needsRedraw = true;
            };

            // Detector sensitivity textbox
            ImGui::Text("Detector sensitivity [kV/W]:");
            ImGui::SameLine();

            float remWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remWidth);
            ImGui::InputText("##DetectorSensitivity",
                appState.spectrum.detectorSensitivityText,
                sizeof(appState.spectrum.detectorSensitivityText));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string s(appState.spectrum.detectorSensitivityText);
                s.erase(0, s.find_first_not_of(" \t\n\r"));
                s.erase(s.find_last_not_of(" \t\n\r") + 1);

                if (s == "NA" || s == "na" || s == "n/a" || s == "none") {
                    appState.spectrum.detectorSensitivity = 0.0f;
                    snprintf(appState.spectrum.detectorSensitivityText,
                             sizeof(appState.spectrum.detectorSensitivityText), "NA");
                    invalidateSpectrumCaches();
                } else {
                    char* end = nullptr;
                    float val = std::strtof(s.c_str(), &end);
                    if (end != s.c_str() && *end == '\0') {
                        appState.spectrum.detectorSensitivity = val;
                        if (val == 0.0f)
                            snprintf(appState.spectrum.detectorSensitivityText,
                                     sizeof(appState.spectrum.detectorSensitivityText), "NA");
                        else
                            snprintf(appState.spectrum.detectorSensitivityText,
                                     sizeof(appState.spectrum.detectorSensitivityText), "%.4f", val);
                        invalidateSpectrumCaches();
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Detector sensitivity in kV/W.\n"
                    "Set to 0 or enter 'NA' to normalize spectrum to max=1 (0 dB).");
            }

            // Reference laser textbox
            ImGui::Text("Ref laser [\xC2\xB5""m]:");
            ImGui::SameLine();
            if (appState.datasetInfo.axisIsCorrected) ImGui::BeginDisabled();

            float remainingWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remainingWidth);
            ImGui::InputFloat("##RefLaserTextbox", &(appState.spectrum.refLaserTextbox), 0.001, 0.01);
            if (ImGui::IsItemDeactivatedAfterEdit() && !appState.datasetInfo.axisIsCorrected) {
                invalidateSpectrumCaches();
            }
            if (appState.datasetInfo.axisIsCorrected) ImGui::EndDisabled();

            // Zero-pad factor K
            ImGui::Text("Zero-pad K:");
            ImGui::SameLine();
            if (appState.datasetInfo.hasPrecomputedSpectra) ImGui::BeginDisabled();

            remainingWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remainingWidth);
            if (ImGui::InputInt("##Kpadding", &appState.spectrum.Kpadding, 1, 1)) {
                appState.spectrum.Kpadding = std::clamp(appState.spectrum.Kpadding, 0, 16);
                invalidateSpectrumCaches();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Zero-pad factor K. Output bins = N*(K+1).\n0 disables padding.");
            }

            // Apodization window selector
            ImGui::Text("Apodization");
            ImGui::SameLine();
            const auto& windowNames = Apodization::getWindowNames();
            if (ImGui::Combo("##ApodizationSelector", &appState.spectrum.apodizationSelector,
                             windowNames.data(), static_cast<int>(windowNames.size()))) {
                invalidateSpectrumCaches();
            }

            // Conditional parametric controls based on selected window
            if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Gauss)) {
                if (ImGui::SliderFloat("Sigma##GaussSigma", &appState.spectrum.apodizationParams.gaussSigma,
                                       1.0f, 3.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Gauss sigma fraction (1.0-3.0).\n1.0 = narrow, 3.0 = wide.");
                }
            } else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular)) {
                ImGui::Text("Mode");
                ImGui::SameLine();
                const bool rectSym  = !appState.spectrum.apodizationParams.rectAsymMode;
                const bool rectAsym =  appState.spectrum.apodizationParams.rectAsymMode;
                const ImVec4 rectBtnClr[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        rectBtnClr[rectSym ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  rectSym ? rectBtnClr[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   rectBtnClr[1]);
                if (ImGui::Button("Sym##RectMode")) {
                    appState.spectrum.apodizationParams.rectAsymMode = false;
                    invalidateSpectrumCaches();
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        rectBtnClr[rectAsym ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  rectAsym ? rectBtnClr[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   rectBtnClr[1]);
                if (ImGui::Button("Asym##RectMode")) {
                    appState.spectrum.apodizationParams.rectAsymMode = true;
                    invalidateSpectrumCaches();
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Sym: uses the longer side's distance on both sides (shorter side saturates).\nAsym: each side extends proportionally to its own distance from peak.");
                }
                if (ImGui::SliderFloat("Width##RectWidth", &appState.spectrum.apodizationParams.rectWidth,
                                       0.05f, 1.0f, "%.2f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Rectangular window width fraction (0.05-1.0).\n1.0 = full signal, 0.05 = 5% of signal.");
                }
            } else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer)) {
                if (ImGui::SliderFloat("FWHM##NortonBeerFwhm", &appState.spectrum.apodizationParams.nortonBeerFwhm,
                                       1.0f, 2.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Norton-Beer FWHM parameter (1.0-2.0 step 0.1).\nControls the relative full-width at half maximum.");
                }
            } else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev)) {
                float at = appState.spectrum.apodizationParams.dolphChebyshevAt;
                ImGui::SliderFloat("Attenuation##DolphChebyshevAt", &at,
                                   50.0f, 160.0f, "%.0f dB");
                at = std::round(at / 10.0f) * 10.0f;
                if (at != appState.spectrum.apodizationParams.dolphChebyshevAt) {
                    appState.spectrum.apodizationParams.dolphChebyshevAt = at;
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Dolph-Chebyshev attenuation (50-160 dB, step 10).\nHigher values produce lower sidelobes.");
                }
            } else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Hamming)) {
                if (ImGui::SliderFloat("Alpha##HammingAlpha", &appState.spectrum.apodizationParams.hammingAlpha, 0.36f, 1.0f, "%.2f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Generalized Hamming alpha (0.36-1.0).\n0.54 = standard Hamming, 1.0 = rectangular.");
                }
            } else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Kaiser)) {
                if (ImGui::SliderFloat("Beta##KaiserBeta", &appState.spectrum.apodizationParams.kaiserBeta, 0.5f, 12.0f, "%.1f")) {
                    invalidateSpectrumCaches();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Kaiser beta (0.5-12.0).\nHigher values suppress sidelobes at the cost of a broader mainlobe.\nDefault 6.0 is similar to Hamming.");
                }
            }

            if (appState.datasetInfo.hasPrecomputedSpectra) ImGui::EndDisabled();

            ImGui::Separator();

            // Navigation block: Cursor, Y scale, X unit, Y Axis (moved to bottom)
            {
                // Shared button style colors for X-unit and Y-scale toggle buttons
                const ImVec4 btnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f), // unselected: visible gray
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) // selected: highlight
                };

                // Tracking cursor toggle
                ImGui::Text("Cursor");
                ImGui::SameLine();
                const bool cursorOn = appState.spectrum.showTrackingCursor;

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("On##CursorOn")) {
                    if (!cursorOn) {
                        appState.spectrum.showTrackingCursor = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[!cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("Off##CursorOff")) {
                    if (cursorOn) {
                        appState.spectrum.showTrackingCursor = false;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                // Y scale selector (lin / log / dB) - rendering only, no cache invalidation needed
                ImGui::Text("Y scale");
                ImGui::SameLine();

                const bool linSelected = (appState.spectrum.yScaleSelector == 0);
                const bool logSelected = (appState.spectrum.yScaleSelector == 1);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[linSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  linSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("lin##YScaleLin")) {
                    if (!linSelected) {
                        appState.spectrum.yScaleSelector = 0;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[logSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  logSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("log##YScaleLog")) {
                    if (!logSelected) {
                        appState.spectrum.yScaleSelector = 1;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                const bool dbSelected = (appState.spectrum.yScaleSelector == 2);
                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[dbSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dbSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("dB##YScaleDb")) {
                    if (!dbSelected) {
                        appState.spectrum.yScaleSelector = 2;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                // X unit selector (cm-1 / µm / THz) - changing unit invalidates spectrum cache
                ImGui::Text("X unit");
                ImGui::SameLine();

                const bool cmSelected  = (appState.spectrum.xUnitSelector == 0);
                const bool umSelected  = (appState.spectrum.xUnitSelector == 1);
                const bool thzSelected = (appState.spectrum.xUnitSelector == 2);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cmSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cmSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("cm-1##XUnitCm")) {
                    if (!cmSelected) {
                        appState.spectrum.xUnitSelector = 0;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[umSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  umSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("\xC2\xB5""m##XUnitUm")) {
                    if (!umSelected) {
                        appState.spectrum.xUnitSelector = 1;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[thzSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  thzSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("THz##XUnitTHz")) {
                    if (!thzSelected) {
                        appState.spectrum.xUnitSelector = 2;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                // Y-axis mode selector (all / tight / force) - matching X-unit / Y-scale button style
                ImGui::Text("Y Axis");
                ImGui::SameLine();

                const bool allSelected   = (appState.spectrum.yAxisMode == 0);
                const bool tightSelected = (appState.spectrum.yAxisMode == 1);
                const bool forceSelected = (appState.spectrum.yAxisMode == 2);

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("all##YAxisAll")) {
                    if (!allSelected) {
                        appState.spectrum.yAxisMode = 0;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[tightSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  tightSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("tight##YAxisTight")) {
                    if (!tightSelected) {
                        appState.spectrum.yAxisMode = 1;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[forceSelected ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  forceSelected ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("force##YAxisForce")) {
                    if (!forceSelected) {
                        appState.spectrum.yAxisMode = 2;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                      "tight: auto-fit Y to visible data only\n"
                                      "force: lock Y to the given min/max");
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();
        
        // Average config panel
        ImGui::Begin("Average");
        if (appState.dataLoaded) {
            // Button / progress bar (mutually exclusive)
            if (!appState.averageSpectrum.calcInProgress) {
                if (ImGui::Button("Calculate average")) {
                    appState.averageSpectrum.startCalculation();
                    appState.needsRedraw = true;
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
                char pctBuf[48];
                float pct = appState.averageSpectrum.progressTotal > 0
                    ? (float)appState.averageSpectrum.progressCurrent /
                      (float)appState.averageSpectrum.progressTotal
                    : 0.0f;
                std::snprintf(pctBuf, sizeof(pctBuf), "Calculating average (%.0f%%)", pct * 100.0f);
                ImGui::ProgressBar(pct,
                    ImVec2(ImGui::GetContentRegionAvail().x, 0), pctBuf);
                ImGui::PopStyleColor();
            }

            ImGui::Separator();

            const ImVec4 btnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // ---- Cursor toggle (SYNCHRONIZED with Spectrum) ----
            ImGui::Text("Cursor");
            ImGui::SameLine();
            const bool cursorOn = appState.spectrum.showTrackingCursor;

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("On##AvgCursorOn")) {
                if (!cursorOn) {
                    appState.spectrum.showTrackingCursor = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[!cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("Off##AvgCursorOff")) {
                if (cursorOn) {
                    appState.spectrum.showTrackingCursor = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // ---- Y scale selector (INDEPENDENT) ----
            ImGui::Text("Y scale");
            ImGui::SameLine();
            const bool linSel = (appState.averageSpectrum.yScaleSelector == 0);
            const bool logSel = (appState.averageSpectrum.yScaleSelector == 1);
            const bool dbSel  = (appState.averageSpectrum.yScaleSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[linSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  linSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("lin##AvgYScaleLin")) { appState.averageSpectrum.yScaleSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[logSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  logSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("log##AvgYScaleLog")) { appState.averageSpectrum.yScaleSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[dbSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dbSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("dB##AvgYScaleDb")) { appState.averageSpectrum.yScaleSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

            // ---- X unit selector (INDEPENDENT) ----
            ImGui::Text("X unit");
            ImGui::SameLine();
            const bool cmSel  = (appState.averageSpectrum.xUnitSelector == 0);
            const bool umSel  = (appState.averageSpectrum.xUnitSelector == 1);
            const bool thzSel = (appState.averageSpectrum.xUnitSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("cm-1##AvgXUnitCm")) { appState.averageSpectrum.xUnitSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[umSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  umSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##AvgXUnitUm")) { appState.averageSpectrum.xUnitSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[thzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  thzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##AvgXUnitTHz")) { appState.averageSpectrum.xUnitSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

                // Match X to Spectrum View
                if (ImGui::Button("Match X to Spectrum View##AvgMatchX")) {
                    int newXUnit = appState.spectrum.xUnitSelector;
                    int oldUnit = appState.averageSpectrum.prevXUnitSelector;
                    double specMin = appState.spectrum.manualXMin;
                    double specMax = appState.spectrum.manualXMax;

                    if (specMin < specMax) {
                        appState.averageSpectrum.manualXMin = specMin;
                        appState.averageSpectrum.manualXMax = specMax;
                        appState.averageSpectrum.pendingNextXMin = specMin;
                        appState.averageSpectrum.pendingNextXMax = specMax;
                        appState.averageSpectrum.shouldAutoscale = false;
                    } else {
                        appState.averageSpectrum.shouldAutoscale = true;
                    }

                    if (appState.averageSpectrum.averageAvailable && !appState.averageSpectrum.cachedAverageX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (double& x : appState.averageSpectrum.cachedAverageX)
                            x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }

                    appState.averageSpectrum.xUnitSelector = newXUnit;
                    appState.averageSpectrum.prevXUnitSelector = newXUnit;
                    appState.needsRedraw = true;
                }

            // ---- Y Axis mode selector (INDEPENDENT) ----
            ImGui::Text("Y Axis");
            ImGui::SameLine();
            const bool allSel   = (appState.averageSpectrum.yAxisMode == 0);
            const bool tightSel = (appState.averageSpectrum.yAxisMode == 1);
            const bool forceSel = (appState.averageSpectrum.yAxisMode == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("all##AvgYAxisAll")) { appState.averageSpectrum.yAxisMode = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[tightSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  tightSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("tight##AvgYAxisTight")) { appState.averageSpectrum.yAxisMode = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[forceSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  forceSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("force##AvgYAxisForce")) { appState.averageSpectrum.yAxisMode = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                  "tight: auto-fit Y to visible data only\n"
                                  "force: lock Y to the given min/max");
            }

            if (appState.averageSpectrum.yAxisMode == 2) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMin", &appState.averageSpectrum.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##AvgForcedYMax", &appState.averageSpectrum.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.averageSpectrum.forcedYMin >= appState.averageSpectrum.forcedYMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // SNR config panel
        ImGui::Begin("SNR");
        if (appState.dataLoaded) {
            if (!appState.snrSpectrum.calcInProgress) {
                int selCount = 0;
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    if (appState.filesSelectedForAveraging[i]) selCount++;
                ImGui::Text("Selected: %d files", selCount);
                if (ImGui::Button("Calculate SNR##SnrCalcBtn")) {
                    appState.snrSpectrum.startCalculation();
                    appState.needsRedraw = true;
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.75f, 0.25f, 0.15f, 1.0f));
                char pctBuf[48];
                float pct = appState.snrSpectrum.progressTotal > 0
                    ? (float)appState.snrSpectrum.progressCurrent /
                      (float)appState.snrSpectrum.progressTotal
                    : 0.0f;
                std::snprintf(pctBuf, sizeof(pctBuf), "Calculating SNR (%.0f%%)", pct * 100.0f);
                ImGui::ProgressBar(pct,
                    ImVec2(ImGui::GetContentRegionAvail().x, 0), pctBuf);
                ImGui::PopStyleColor();
            }

            const ImVec4 btnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            ImGui::Text("Cursor");
            ImGui::SameLine();
            const bool cursorOn = appState.spectrum.showTrackingCursor;

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("On##SnrCursorOn")) {
                if (!cursorOn) {
                    appState.spectrum.showTrackingCursor = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[!cursorOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("Off##SnrCursorOff")) {
                if (cursorOn) {
                    appState.spectrum.showTrackingCursor = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            ImGui::Text("Y scale");
            ImGui::SameLine();
            const bool snrLinSel = (appState.snrSpectrum.yScaleSelector == 0);
            const bool snrLogSel = (appState.snrSpectrum.yScaleSelector == 1);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrLinSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrLinSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("lin##SnrYScaleLin")) { appState.snrSpectrum.yScaleSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrLogSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrLogSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("log##SnrYScaleLog")) { appState.snrSpectrum.yScaleSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

            ImGui::Text("X unit");
            ImGui::SameLine();
            const bool snrCmSel  = (appState.snrSpectrum.xUnitSelector == 0);
            const bool snrUmSel  = (appState.snrSpectrum.xUnitSelector == 1);
            const bool snrThzSel = (appState.snrSpectrum.xUnitSelector == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrCmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrCmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("cm-1##SnrXUnitCm")) { appState.snrSpectrum.xUnitSelector = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrUmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrUmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##SnrXUnitUm")) { appState.snrSpectrum.xUnitSelector = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrThzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrThzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##SnrXUnitTHz")) { appState.snrSpectrum.xUnitSelector = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);

                // Match X to Spectrum View
                if (ImGui::Button("Match X to Spectrum View##SnrMatchX")) {
                    int newXUnit = appState.spectrum.xUnitSelector;
                    int oldUnit = appState.snrSpectrum.prevXUnitSelector;
                    double specMin = appState.spectrum.manualXMin;
                    double specMax = appState.spectrum.manualXMax;

                    if (specMin < specMax) {
                        appState.snrSpectrum.manualXMin = specMin;
                        appState.snrSpectrum.manualXMax = specMax;
                        appState.snrSpectrum.pendingNextXMin = specMin;
                        appState.snrSpectrum.pendingNextXMax = specMax;
                        appState.snrSpectrum.shouldAutoscale = false;
                    } else {
                        appState.snrSpectrum.shouldAutoscale = true;
                    }

                    if (appState.snrSpectrum.snrAvailable && !appState.snrSpectrum.cachedSnrX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (double& x : appState.snrSpectrum.cachedSnrX)
                            x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }

                    appState.snrSpectrum.xUnitSelector = newXUnit;
                    appState.snrSpectrum.prevXUnitSelector = newXUnit;
                    appState.needsRedraw = true;
                }

            ImGui::Text("Y Axis");
            ImGui::SameLine();
            const bool snrAllSel   = (appState.snrSpectrum.yAxisMode == 0);
            const bool snrTightSel = (appState.snrSpectrum.yAxisMode == 1);
            const bool snrForceSel = (appState.snrSpectrum.yAxisMode == 2);

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrAllSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrAllSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("all##SnrYAxisAll")) { appState.snrSpectrum.yAxisMode = 0; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrTightSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrTightSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("tight##SnrYAxisTight")) { appState.snrSpectrum.yAxisMode = 1; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrForceSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrForceSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("force##SnrYAxisForce")) { appState.snrSpectrum.yAxisMode = 2; appState.needsRedraw = true; }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("all: auto-fit Y to all data\n"
                                  "tight: auto-fit Y to visible data only\n"
                                  "force: lock Y to the given min/max");
            }

            if (appState.snrSpectrum.yAxisMode == 2) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMin", &appState.snrSpectrum.forcedYMin, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##SnrForcedYMax", &appState.snrSpectrum.forcedYMax, 0.0, 0.0, "%.6g"))
                    appState.needsRedraw = true;
                if (appState.snrSpectrum.forcedYMin >= appState.snrSpectrum.forcedYMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                }
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Allan config panel
        ImGui::Begin("Allan");
        if (appState.dataLoaded) {
            if (!appState.allanVariance.calcInProgress) {
                int selCount = 0;
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    if (appState.filesSelectedForAveraging[i]) selCount++;
                ImGui::Text("Selected: %d files", selCount);

                ImGui::Text("Decimation");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputInt("##AllanDecimation", &appState.allanVariance.wavelengthDecimation, 1, 1)) {
                    if (appState.allanVariance.wavelengthDecimation < 1)
                        appState.allanVariance.wavelengthDecimation = 1;
                    appState.needsRedraw = true;
                }

                const ImVec4 btnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };

                ImGui::Text("Calc base");
                ImGui::SameLine();
                const bool allanCalcBase100T = (appState.allanVariance.calcBaseSelector == 0);
                const bool allanCalcBaseSpectrum = (appState.allanVariance.calcBaseSelector == 1);
                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allanCalcBase100T ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allanCalcBase100T ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("100% T##AllanCalcBase100T")) { appState.allanVariance.calcBaseSelector = 0; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[allanCalcBaseSpectrum ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allanCalcBaseSpectrum ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
                if (ImGui::Button("Spectrum##AllanCalcBaseSpectrum")) { appState.allanVariance.calcBaseSelector = 1; appState.needsRedraw = true; }
                ImGui::PopStyleColor(3);

                if (ImGui::Button("Calculate Allan")) {
                    appState.allanVariance.startCalculation();
                    appState.needsRedraw = true;
            }

            ImGui::Separator();

                // Navigation block (X unit, Cursor, X range) - moved to bottom
                {
                    const ImVec4 navBtnColors[2] = {
                        ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                    };

                    // X unit
                    ImGui::Text("X unit");
                    ImGui::SameLine();
                    const bool allanCmSel  = (appState.allanVariance.xUnitSelector == 0);
                    const bool allanUmSel  = (appState.allanVariance.xUnitSelector == 1);
                    const bool allanThzSel = (appState.allanVariance.xUnitSelector == 2);

                    ImGui::PushStyleColor(ImGuiCol_Button,        navBtnColors[allanCmSel ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allanCmSel ? navBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   navBtnColors[1]);
                    if (ImGui::Button("cm-1##AllanXUnitCm")) { appState.allanVariance.xUnitSelector = 0; appState.needsRedraw = true; }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::PushStyleColor(ImGuiCol_Button,        navBtnColors[allanUmSel ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allanUmSel ? navBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   navBtnColors[1]);
                    if (ImGui::Button("\xC2\xB5""m##AllanXUnitUm")) { appState.allanVariance.xUnitSelector = 1; appState.needsRedraw = true; }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::PushStyleColor(ImGuiCol_Button,        navBtnColors[allanThzSel ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  allanThzSel ? navBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   navBtnColors[1]);
                    if (ImGui::Button("THz##AllanXUnitTHz")) { appState.allanVariance.xUnitSelector = 2; appState.needsRedraw = true; }
                    ImGui::PopStyleColor(3);

                    // Cursor On/Off
                    ImGui::Text("Cursor");
                    ImGui::SameLine();
                    const bool cursorOn = appState.spectrum.showTrackingCursor;
                    const ImVec4 cursorBtnColors[2] = {
                        ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                    };
                    ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[cursorOn ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                    if (ImGui::Button("On##AllanCursorOn")) {
                        if (!cursorOn) {
                            appState.spectrum.showTrackingCursor = true;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[!cursorOn ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                    if (ImGui::Button("Off##AllanCursorOff")) {
                        if (cursorOn) {
                            appState.spectrum.showTrackingCursor = false;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(3);

                    // X range min/max (stored in um, displayed in selected unit)
                    double displayMin = appState.allanVariance.xRangeMin;
                    double displayMax = appState.allanVariance.xRangeMax;
                    if (appState.allanVariance.xUnitSelector == 0) {
                        displayMin = SpectralToolbox::convertUmToCm(appState.allanVariance.xRangeMin);
                        displayMax = SpectralToolbox::convertUmToCm(appState.allanVariance.xRangeMax);
                    } else if (appState.allanVariance.xUnitSelector == 2) {
                        displayMin = SpectralToolbox::convertUmToTHz(appState.allanVariance.xRangeMin);
                        displayMax = SpectralToolbox::convertUmToTHz(appState.allanVariance.xRangeMax);
                    }
                    const char* unitStr = (appState.allanVariance.xUnitSelector == 0) ? "cm-1"
                                        : (appState.allanVariance.xUnitSelector == 1) ? "\xC2\xB5""m"
                                                                                       : "THz";
                    ImGui::Text("X range (%s)", unitStr);
                    ImGui::SameLine();
                    ImGui::Text("min:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::InputDouble("##AllanXRangeMin", &displayMin, 0.0, 0.0, "%.6g")) {
                        if (appState.allanVariance.xUnitSelector == 0)
                            appState.allanVariance.xRangeMin = SpectralToolbox::convertCmToUm(displayMin);
                        else if (appState.allanVariance.xUnitSelector == 2)
                            appState.allanVariance.xRangeMin = SpectralToolbox::convertTHzToUm(displayMin);
                        else
                            appState.allanVariance.xRangeMin = displayMin;
                        appState.needsRedraw = true;
                    }
                    ImGui::SameLine();
                    ImGui::Text("max:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::InputDouble("##AllanXRangeMax", &displayMax, 0.0, 0.0, "%.6g")) {
                        if (appState.allanVariance.xUnitSelector == 0)
                            appState.allanVariance.xRangeMax = SpectralToolbox::convertCmToUm(displayMax);
                        else if (appState.allanVariance.xUnitSelector == 2)
                            appState.allanVariance.xRangeMax = SpectralToolbox::convertTHzToUm(displayMax);
                        else
                            appState.allanVariance.xRangeMax = displayMax;
                        appState.needsRedraw = true;
                    }
                    if (appState.allanVariance.xRangeMin >= appState.allanVariance.xRangeMax) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                    }
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.5f, 1.0f));
                char pctBuf[48];
                float pct = appState.allanVariance.progressTotal > 0
                    ? (float)appState.allanVariance.progressCurrent /
                      (float)appState.allanVariance.progressTotal
                    : 0.0f;
                std::snprintf(pctBuf, sizeof(pctBuf), "Calculating Allan (%.0f%%)", pct * 100.0f);
                ImGui::ProgressBar(pct,
                    ImVec2(ImGui::GetContentRegionAvail().x, 0), pctBuf);
                ImGui::PopStyleColor();
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // 100% T config panel (docked)
        ImGui::Begin("100% T");
        if (appState.dataLoaded) {
            ImGui::Text("Reference source:");
            ImGui::SameLine();
            {
                int& refSrc = appState.t100.referenceSource;
                bool avgAvail = appState.averageSpectrum.averageAvailable;
                const ImVec4 cfgBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 0 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 0 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("File##T100RefSrcFile")) {
                    refSrc = 0;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 1 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 1 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("CSV##T100RefSrcCSV")) {
                    refSrc = 1;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                if (!avgAvail) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[refSrc == 2 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  refSrc == 2 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("Avg##T100RefSrcAvg")) {
                    refSrc = 2;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                if (!avgAvail) ImGui::EndDisabled();
            }

            ImGui::Separator();

            if (appState.t100.referenceSource == 0) {
                if (ImGui::Button("Set as reference##T100SetRef")) {
                    appState.t100.setReferenceFromCurrentSpectrum();
                    appState.needsRedraw = true;
                }
            } else if (appState.t100.referenceSource == 1) {
                ImGui::InputText("Path##T100CsvPath", appState.t100.csvPathBuffer,
                                 sizeof(appState.t100.csvPathBuffer));
                ImGui::SameLine();
                if (ImGui::Button("Browse...##T100Browse")) {
                    const char* filter = "*.csv";
                    const char* path = tinyfd_openFileDialog("Select Reference CSV", "", 1, &filter, "CSV Files", 0);
                    if (path) {
                        strncpy(appState.t100.csvPathBuffer, path,
                                     sizeof(appState.t100.csvPathBuffer) - 1);
                        appState.t100.csvPathBuffer[sizeof(appState.t100.csvPathBuffer) - 1] = '\0';
                        appState.needsRedraw = true;
                    }
                }
                if (appState.t100.csvPathBuffer[0] != '\0') {
                    if (ImGui::Button("Load##T100LoadCsv")) {
                        appState.t100.setReferenceFromCSV(appState.t100.csvPathBuffer);
                        appState.needsRedraw = true;
                    }
                }
            } else if (appState.t100.referenceSource == 2) {
                if (!appState.averageSpectrum.averageAvailable) ImGui::BeginDisabled();
                if (ImGui::Button("Use average##T100UseAvg")) {
                    appState.t100.setReferenceFromAverage();
                    appState.needsRedraw = true;
                }
                if (!appState.averageSpectrum.averageAvailable) ImGui::EndDisabled();
            }

            if (appState.t100.referenceAvailable) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Reference loaded");
                ImGui::TextWrapped("%s", appState.t100.refDescription.c_str());
                const char* unitName = (appState.t100.refXUnit == 0) ? "cm-1"
                                     : (appState.t100.refXUnit == 1) ? "um"
                                     : "THz";
                ImGui::TextDisabled("%zu points, unit: %s",
                    appState.t100.refX.size(), unitName);
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.5f, 0.1f, 1.0f), "No reference");
            }

            ImGui::Separator();

            // Energy Ratios
            {
                auto ratioInput = [](const char* label, char* buf, size_t bufSize) {
                    ImGui::SetNextItemWidth(70);
                    ImGui::InputText(label, buf, bufSize);
                };

                ImGui::Text("Energy Ratios");
                ImGui::SameLine();
                ImGui::SetWindowFontScale(0.7f);
                ImGui::Text("(cm-1)");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine();
                if (ImGui::Button("ASTM E1421##T100AstmE1421")) {
                    strncpy(appState.t100.energyRatioNumA, "4000", 31);
                    strncpy(appState.t100.energyRatioDenA, "2000", 31);
                    strncpy(appState.t100.energyRatioNumB, "2000", 31);
                    strncpy(appState.t100.energyRatioDenB, "1000", 31);
                    strncpy(appState.t100.energyRatioNumC, "150", 31);
                    strncpy(appState.t100.energyRatioDenC, "max", 31);
                    appState.needsRedraw = true;
                }

                ImGui::Text("A: "); ImGui::SameLine();
                ratioInput("##T100RatioNumA", appState.t100.energyRatioNumA,
                           sizeof(appState.t100.energyRatioNumA));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenA", appState.t100.energyRatioDenA,
                           sizeof(appState.t100.energyRatioDenA));

                ImGui::Text("B: "); ImGui::SameLine();
                ratioInput("##T100RatioNumB", appState.t100.energyRatioNumB,
                           sizeof(appState.t100.energyRatioNumB));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenB", appState.t100.energyRatioDenB,
                           sizeof(appState.t100.energyRatioDenB));

                ImGui::Text("C: "); ImGui::SameLine();
                ratioInput("##T100RatioNumC", appState.t100.energyRatioNumC,
                           sizeof(appState.t100.energyRatioNumC));
                ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                ratioInput("##T100RatioDenC", appState.t100.energyRatioDenC,
                           sizeof(appState.t100.energyRatioDenC));
            }

            ImGui::Separator();

            // Force Y min/max (shown when force mode)
            if (appState.t100.yAxisMode == 2) {
                ImGui::Text("Force Y");
                double vMin = appState.t100.forcedYMin;
                double vMax = appState.t100.forcedYMax;
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Min##T100ForceYMin", &vMin, 0.0, 0.0, "%.4f")) {
                    if (vMax > vMin) {
                        appState.t100.forcedYMin = vMin;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputDouble("Max##T100ForceYMax", &vMax, 0.0, 0.0, "%.4f")) {
                    if (vMax > vMin) {
                        appState.t100.forcedYMax = vMax;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::Separator();
            }

            ImGui::Text("Std Deviation");
            if (!appState.t100.calcStdInProgress) {
                if (ImGui::Button("Calculate std##T100CalcStd")) {
                    if (appState.t100.referenceAvailable) {
                        appState.t100.startStdCalculation();
                        appState.needsRedraw = true;
                    }
                }
            } else {
                float pct = appState.t100.stdProgressTotal > 0
                    ? (float)appState.t100.stdProgressCurrent / (float)appState.t100.stdProgressTotal
                    : 0.0f;
                ImGui::ProgressBar(pct, ImVec2(-1, 0), "");
                ImGui::Text("Processing %d/%d", appState.t100.stdProgressCurrent,
                            appState.t100.stdProgressTotal);
            }

            ImGui::Separator();

            // Navigation block (Cursor, X unit, Match X, Y Axis) - moved to bottom
            {
                const ImVec4 cfgBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };

                // Cursor On/Off
                ImGui::Text("Cursor");
                ImGui::SameLine();
                const bool cursorOn = appState.spectrum.showTrackingCursor;
                ImVec4 cursorBtnColors[2] = {
                    ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                };
                ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("On##T100CursorOn")) {
                    if (!cursorOn) {
                        appState.spectrum.showTrackingCursor = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cursorBtnColors[!cursorOn ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  !cursorOn ? cursorBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cursorBtnColors[1]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                if (ImGui::Button("Off##T100CursorOff")) {
                    if (cursorOn) {
                        appState.spectrum.showTrackingCursor = false;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);

                // X unit
                ImGui::Text("X unit");
                ImGui::SameLine();
                int& sel = appState.t100.xUnitSelector;
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[sel == 0 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  sel == 0 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("cm-1##T100XUnitCm")) {
                    sel = 0;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[sel == 1 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  sel == 1 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("\xC2\xB5" "m##T100XUnitUm")) {
                    sel = 1;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[sel == 2 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  sel == 2 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("THz##T100XUnitTHz")) {
                    sel = 2;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);

                // Match X to Spectrum View
                if (ImGui::Button("Match X to Spectrum View##T100MatchX")) {
                    int newXUnit = appState.spectrum.xUnitSelector;
                    int oldUnit = appState.t100.prevXUnitSelector;
                    double specMin = appState.spectrum.manualXMin;
                    double specMax = appState.spectrum.manualXMax;

                    if (specMin < specMax) {
                        appState.t100.manualXMin = specMin;
                        appState.t100.manualXMax = specMax;
                        appState.t100.pendingNextXMin = specMin;
                        appState.t100.pendingNextXMax = specMax;
                        appState.t100.shouldAutoscale = false;
                    } else {
                        appState.t100.shouldAutoscale = true;
                    }

                    if (appState.t100.transmittanceAvailable && !appState.t100.cachedTransX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (auto& [fid, vec] : appState.t100.cachedTransX)
                            for (double& x : vec)
                                x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }
                    if (appState.t100.stddevAvailable && !appState.t100.cachedStdX.empty()) {
                        auto oldU = static_cast<SpectralToolbox::SpectrumXUnit>(oldUnit);
                        auto newU = static_cast<SpectralToolbox::SpectrumXUnit>(newXUnit);
                        for (double& x : appState.t100.cachedStdX)
                            x = SpectralToolbox::convertXValue(x, oldU, newU);
                    }

                    appState.t100.xUnitSelector = newXUnit;
                    appState.t100.prevXUnitSelector = newXUnit;
                    appState.needsRedraw = true;
                }

                // Y Axis
                ImGui::Text("Y Axis");
                ImGui::SameLine();
                int& mode = appState.t100.yAxisMode;
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[mode == 0 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  mode == 0 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("all##T100YAxisAll")) {
                    mode = 0;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[mode == 1 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  mode == 1 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("tight##T100YAxisTight")) {
                    mode = 1;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[mode == 2 ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  mode == 2 ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("force##T100YAxisForce")) {
                    mode = 2;
                    appState.needsRedraw = true;
                }
                ImGui::PopStyleColor(3);
            }

        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Interferogram Config panel (docked)
        ImGui::Begin("Interferogram");
        if (!appState.datasetInfo.hasInterferograms && appState.dataLoaded) {
            ImGui::Text("Interferograms not available for this data type.");
        } else if (appState.dataLoaded) {
            const ImVec4 cfgBtnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // Row 1: X axis base (sample / OPD)
            ImGui::Text("X axis base");
            ImGui::SameLine();
            const bool xSample = (appState.xAxisBase == 0);
            const bool xOPD = (appState.xAxisBase == 1);
            const bool axisCorrected = appState.datasetInfo.axisIsCorrected;

            if (axisCorrected) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[(xSample && !axisCorrected) ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  (xSample && !axisCorrected) ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("sample##XBaseSample")) {
                if (!xSample && !axisCorrected) {
                    appState.xAxisBase = 0;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            if (axisCorrected) ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[xOPD ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  xOPD ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("OPD##XBaseOPD")) {
                if (!xOPD) {
                    appState.xAxisBase = 1;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 2: Max at zero (off / on)
            ImGui::Text("Max at zero");
            ImGui::SameLine();
            const bool alignOff = !appState.maxAtZero;
            const bool alignOn  =  appState.maxAtZero;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[alignOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  alignOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##AlignOff")) {
                if (appState.maxAtZero) {
                    appState.maxAtZero = false;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[alignOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  alignOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##AlignOn")) {
                if (!appState.maxAtZero) {
                    appState.maxAtZero = true;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 3: Auto-fit Y (off / on)
            ImGui::Text("Auto-fit Y");
            ImGui::SameLine();
            const bool afyOff = !appState.autoFitYAxis;
            const bool afyOn  =  appState.autoFitYAxis;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[afyOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  afyOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##AfyOff")) {
                if (appState.autoFitYAxis) {
                    appState.autoFitYAxis = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[afyOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  afyOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##AfyOn")) {
                if (!appState.autoFitYAxis) {
                    appState.autoFitYAxis = true;
                    if (appState.dataLoaded) {
                        if (!appState.loadedData[0].referenceDetector.empty()) {
                            auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                            appState.ref_y_min = *ref_min_max.first;
                            appState.ref_y_max = *ref_min_max.second;
                        }
                        auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                        appState.prim_y_min = *prim_min_max.first;
                        appState.prim_y_max = *prim_min_max.second;
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 5: Downsample (off / on)
            ImGui::Text("Downsample");
            ImGui::SameLine();
            const bool dsOff = !appState.enableDownsampling;
            const bool dsOn  =  appState.enableDownsampling;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[dsOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dsOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##DsOff")) {
                if (appState.enableDownsampling) {
                    appState.enableDownsampling = false;
                    appState.hilbertXCache.clear();
                    appState.peakPositionsCache.clear();
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = appState.currentAdapter->loadFile(filePath);
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                    std::vector<double> downsampledRef, downsampledPrim;
                                    for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                        downsampledRef.push_back(data.referenceDetector[j]);
                                        downsampledPrim.push_back(data.primaryDetector[j]);
                                    }
                                    data.referenceDetector = downsampledRef;
                                    data.primaryDetector = downsampledPrim;
                                }
                                reloadedData.push_back(data);
                            } catch (const std::exception& e) {
                                std::cerr << "Error reloading file: " << e.what() << std::endl;
                            }
                        }
                        if (!reloadedData.empty()) {
                            appState.loadedData = reloadedData;
                            appState.rawDataCache.clear();
                            size_t reloadedIdx = 0;
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = appState.currentAdapter->loadFile(file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    if (reloadedIdx < reloadedData.size())
                                        appState.rawDataCache.push_back(reloadedData[reloadedIdx]);
                                }
                                reloadedIdx++;
                            }
                            appState.zoomRange = {0, 0};
                            appState.shouldAutoscale = true;
                            appState.forceXAutofit = true;
                        }
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[dsOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dsOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##DsOn")) {
                if (!appState.enableDownsampling) {
                    appState.enableDownsampling = true;
                    appState.hilbertXCache.clear();
                    appState.peakPositionsCache.clear();
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = appState.currentAdapter->loadFile(filePath);
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                    std::vector<double> downsampledRef, downsampledPrim;
                                    for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                        downsampledRef.push_back(data.referenceDetector[j]);
                                        downsampledPrim.push_back(data.primaryDetector[j]);
                                    }
                                    data.referenceDetector = downsampledRef;
                                    data.primaryDetector = downsampledPrim;
                                }
                                reloadedData.push_back(data);
                            } catch (const std::exception& e) {
                                std::cerr << "Error reloading file: " << e.what() << std::endl;
                            }
                        }
                        if (!reloadedData.empty()) {
                            appState.loadedData = reloadedData;
                            appState.rawDataCache.clear();
                            size_t reloadedIdx = 0;
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = appState.currentAdapter->loadFile(file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    if (reloadedIdx < reloadedData.size())
                                        appState.rawDataCache.push_back(reloadedData[reloadedIdx]);
                                }
                                reloadedIdx++;
                            }
                            appState.zoomRange = {0, 0};
                            appState.shouldAutoscale = true;
                            appState.forceXAutofit = true;
                        }
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 6: X correction (Hilbert / Peaks) — only when feature is available
            const bool canPeakFind = !appState.datasetInfo.axisIsCorrected && appState.datasetInfo.hasReferenceChannel;
            if (canPeakFind) {
                ImGui::Text("X correction");
                ImGui::SameLine();
                const bool hilbSel = (appState.xCorrectionMethod == 0);
                const bool peakSel = (appState.xCorrectionMethod == 1);

                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[hilbSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  hilbSel ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("Hilbert##XCorrHilb")) {
                    if (appState.xCorrectionMethod != 0) {
                        appState.xCorrectionMethod = 0;
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.spectrum.cachedFrequencies.clear();
                        appState.spectrum.cachedSpectra.clear();
                        appState.spectrum.lastPrimaryDetectors.clear();
                        appState.spectrum.lastSpectrumParams.clear();
                        appState.spectrum.pendingSpectra_.clear();
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);

                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[peakSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  peakSel ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("Peaks##XCorrPeak")) {
                    if (appState.xCorrectionMethod != 1) {
                        appState.xCorrectionMethod = 1;
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.spectrum.cachedFrequencies.clear();
                        appState.spectrum.cachedSpectra.clear();
                        appState.spectrum.lastPrimaryDetectors.clear();
                        appState.spectrum.lastSpectrumParams.clear();
                        appState.spectrum.pendingSpectra_.clear();
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                // Row 7: Peak prominence slider — only visible when PeakFinding active
                if (appState.xCorrectionMethod == 1) {
                    ImGui::Text("Peak promin.");
                    ImGui::SameLine();
                    float prom = appState.peakProminenceThreshold;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::SliderFloat("##PeakProm", &prom, 0.0f, 0.5f, "%.3f")) {
                        if (std::abs(prom - appState.peakProminenceThreshold) > 1e-6f) {
                            appState.peakProminenceThreshold = prom;
                            appState.hilbertXCache.clear();
                            appState.peakPositionsCache.clear();
                            appState.spectrum.cachedFrequencies.clear();
                            appState.spectrum.cachedSpectra.clear();
                            appState.spectrum.lastPrimaryDetectors.clear();
                            appState.spectrum.lastSpectrumParams.clear();
                            appState.spectrum.pendingSpectra_.clear();
                            appState.needsRedraw = true;
                        }
                    }

                    // Row 8: Peak indicators (off / on) — only in sample mode
                    if (appState.xAxisBase != 0) ImGui::BeginDisabled();
                    ImGui::Text("Peak markers");
                    ImGui::SameLine();
                    const bool pmOff = !appState.showPeakIndicators;
                    const bool pmOn  =  appState.showPeakIndicators;

                    ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[pmOff ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  pmOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                    if (ImGui::Button("off##PmOff")) {
                        if (appState.showPeakIndicators) {
                            appState.showPeakIndicators = false;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[pmOn ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  pmOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                    if (ImGui::Button("on##PmOn")) {
                        if (!appState.showPeakIndicators) {
                            appState.showPeakIndicators = true;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if (appState.xAxisBase != 0) ImGui::EndDisabled();
                }
            }
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Export panel (docked)
        ImGui::Begin("Export");
        if (appState.dataLoaded) {
            appState.exportPanel.render();
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Metadata panel (right)
        ImGui::Begin("Metadata");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        if (appState.dataLoaded) {
            ImGui::Text("File: %s", appState.csvFiles.empty() ? "None" : appState.csvFiles[0].c_str());
            ImGui::Text("Samples: %zu", appState.loadedData.empty() ? 0 : appState.loadedData[0].dataSize());
            ImGui::Text("Adapter: %s", appState.datasetInfo.adapterName.c_str());
            
            // Display comments if comments.txt exists (WUST format)
            if (appState.datasetInfo.hasMetadataFile) {
                ImGui::Separator();
                ImGui::Text("Comments:");
                
                std::string commentsPath = appState.currentDirectory;
                size_t last_slash = commentsPath.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    commentsPath = commentsPath.substr(0, last_slash); // Go up to parent directory
                }
                commentsPath += "/comments.txt";
                
                std::ifstream commentsFile(commentsPath);
                if (commentsFile.is_open()) {
                    std::string line;
                    while (std::getline(commentsFile, line)) {
                        ImGui::TextWrapped("%s", line.c_str());
                    }
                    commentsFile.close();
                } else {
                    ImGui::Text("<Comments Empty>");
                }
            } else {
                ImGui::Separator();
                if (!appState.loadedData.empty() && !appState.loadedData[0].metadata.empty()) {
                    ImGui::TextWrapped("%s", appState.loadedData[0].metadata.c_str());
                } else {
                    ImGui::Text("-no data-");
                }
            }
        } else {
            ImGui::Text("No metadata available.");
        }
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();
        
        // Spectrum View panel (docked)
        ImGui::Begin("Spectrum View");
        if (appState.dataLoaded && !appState.loadedData.empty()) {
            // Pre-load precomputed spectra into spectrum cache (always refresh)
            if (appState.datasetInfo.hasPrecomputedSpectra) {
                auto targetUnit = static_cast<SpectralToolbox::SpectrumXUnit>(appState.spectrum.xUnitSelector);
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    const std::string& fid = appState.selectedFilenames[i];
                    if (appState.spectrum.cachedFrequencies.find(fid) == appState.spectrum.cachedFrequencies.end()) {
                        // File stores wavenumber in cm-1; convert to target unit
                        std::vector<double> freqs = appState.rawDataCache[i].referenceDetector;
                        for (double& f : freqs)
                            f = SpectralToolbox::convertXValue(f,
                                SpectralToolbox::SpectrumXUnit::CmInv, targetUnit);
                        appState.spectrum.cachedFrequencies[fid] = std::move(freqs);
                        appState.spectrum.cachedSpectra[fid] = appState.rawDataCache[i].primaryDetector;
                        appState.spectrum.lastPrimaryDetectors[fid] = appState.rawDataCache[i].primaryDetector;
                        double activeParam = 0.0;
                        if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Gauss))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.gaussSigma);
                        else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.rectWidth);
                        else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.nortonBeerFwhm);
                        else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.dolphChebyshevAt);
                        else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Hamming))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.hammingAlpha);
                        else if (appState.spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Kaiser))
                            activeParam = static_cast<double>(appState.spectrum.apodizationParams.kaiserBeta);
                        appState.spectrum.lastSpectrumParams[fid] = {
                            static_cast<double>(appState.spectrum.Kpadding),
                            static_cast<double>(appState.spectrum.refLaserTextbox),
                            static_cast<double>(appState.spectrum.apodizationSelector),
                            activeParam,
                            appState.spectrum.apodizationParams.rectAsymMode ? 1.0 : 0.0,
                            static_cast<double>(appState.xCorrectionMethod),
                            static_cast<double>(appState.peakProminenceThreshold),
                            0.0
                        };
                    }
                }
            }
            std::vector<std::pair<std::string, std::vector<double>>> primaryDetectors;
            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                primaryDetectors.emplace_back(appState.selectedFilenames[i], appState.loadedData[i].primaryDetector);
            }
            appState.spectrum.renderSpectrumContents(primaryDetectors, appState.rawDataCache);
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Average View panel (docked)
        ImGui::Begin("Average View");
        appState.averageSpectrum.renderAverageContents(appState.spectrum.showTrackingCursor);
        ImGui::End();

        // SNR View panel (docked)
        ImGui::Begin("SNR View");
        appState.snrSpectrum.renderSnrContents(appState.spectrum.showTrackingCursor);
        ImGui::End();

        // Allan View panel (docked)
        ImGui::Begin("Allan View");
        appState.allanVariance.renderAllanContents(appState.spectrum.showTrackingCursor);
        ImGui::End();

        // 100% T View panel (docked)
        ImGui::Begin("100% T View");
        if (appState.dataLoaded && !appState.selectedFilenames.empty()) {
            appState.t100.renderT100Contents(appState.spectrum.showTrackingCursor);
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

        // Close the panel condition (welcome screen)
        }
        
        // Close the docking condition
        }
        
        // Add FPS counter overlay before rendering
        if (appState.showFPS) {
            // Create a high-contrast FPS counter in top-right corner
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 120, 30), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.7f); // Semi-transparent background
            
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            
            ImGui::Begin("FPS Counter", nullptr, flags);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "FPS: %.1f", appState.fps); // White text for high contrast
            ImGui::End();
        }

        // Export progress overlay — drawn directly on foreground when export is pending
        if (appState.exportPanel.exportPending) {
            fprintf(stderr, "DEBUG: Export progress overlay rendering\n");
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 size = ImGui::GetIO().DisplaySize;
            dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 160));
            const char* msg = "Export in progress... Please wait.";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImVec2 pos((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f);
            dl->AddRectFilled(
                ImVec2(pos.x - 20, pos.y - 12),
                ImVec2(pos.x + ts.x + 20, pos.y + ts.y + 12),
                IM_COL32(30, 30, 50, 230), 8.0f);
            dl->AddText(pos, IM_COL32(255, 255, 255, 255), msg);
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        
        // Use a black background color to match the welcome screen
        // This creates a consistent dark theme throughout the application
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);

        // Execute deferred export after the frame is visible on screen
        if (appState.exportPanel.exportPending) {
            appState.exportPanel.executePendingExport();
            appState.needsRedraw = true;
        }
        
        // Force redraw every frame while welcome screen is active (pattern persistence)
        if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
            appState.needsRedraw = true;
        }
        
        // Reset keyboard navigation flag after rendering
        appState.keyboardNavigation = false;
    }
    
    // Cleanup
    destroyWelcomeBackground();
    cleanupApplication(window);
    
    // Save configuration before exiting
    config.maxAtZero = appState.maxAtZero;
    config.autoFitYAxis = appState.autoFitYAxis;
    config.enableDownsampling = appState.enableDownsampling;
    config.xAxisBase = appState.xAxisBase;
    config.lastWorkingDirectory = appState.currentDirectory;
    config.uiSize = appState.currentUiSize;

    // Update config with current FPS setting before saving
    config.showFPS = appState.showFPS;
    config.gridAlpha = appState.gridAlpha;
    config.xCorrectionMethod = appState.xCorrectionMethod;
    config.peakProminence = appState.peakProminenceThreshold;
    config.showPeakIndicators = appState.showPeakIndicators;
    config.accentColor = appState.currentAccentColor;
    
    // Save spectrum window settings
    config.spectrumYAxisMode           = appState.spectrum.yAxisMode;
    config.spectrumXUnitSelector       = appState.spectrum.xUnitSelector;
    config.spectrumYScaleSelector      = appState.spectrum.yScaleSelector;
    config.spectrumForcedYMin          = appState.spectrum.forcedYMin;
    config.spectrumForcedYMax   = appState.spectrum.forcedYMax;
    config.apodizationSelector  = appState.spectrum.apodizationSelector;
    config.apodGaussSigma       = appState.spectrum.apodizationParams.gaussSigma;
    config.apodRectWidth        = appState.spectrum.apodizationParams.rectWidth;
    config.apodNortonBeerFwhm  = appState.spectrum.apodizationParams.nortonBeerFwhm;
    config.apodDolphChebyshevAt = appState.spectrum.apodizationParams.dolphChebyshevAt;
    config.apodHammingAlpha     = appState.spectrum.apodizationParams.hammingAlpha;
    config.apodKaiserBeta       = appState.spectrum.apodizationParams.kaiserBeta;
    config.apodRectAsymMode     = appState.spectrum.apodizationParams.rectAsymMode;
    config.spectrumDetectorSensitivity = appState.spectrum.detectorSensitivity;
    config.spectrumRefLaser = appState.spectrum.refLaserTextbox;
    
    // Save average window settings
    config.avgYAxisMode           = appState.averageSpectrum.yAxisMode;
    config.avgXUnitSelector       = appState.averageSpectrum.xUnitSelector;
    config.avgYScaleSelector      = appState.averageSpectrum.yScaleSelector;
    config.avgForcedYMin          = appState.averageSpectrum.forcedYMin;
    config.avgForcedYMax          = appState.averageSpectrum.forcedYMax;

    // Save SNR window settings
    config.snrYAxisMode           = appState.snrSpectrum.yAxisMode;
    config.snrXUnitSelector       = appState.snrSpectrum.xUnitSelector;
    config.snrYScaleSelector      = appState.snrSpectrum.yScaleSelector;
    config.snrForcedYMin          = appState.snrSpectrum.forcedYMin;
    config.snrForcedYMax          = appState.snrSpectrum.forcedYMax;

    // Save Allan window settings
    config.allanXUnitSelector        = appState.allanVariance.xUnitSelector;
    config.allanWavelengthDecimation = appState.allanVariance.wavelengthDecimation;
    config.allanSliceIndex           = appState.allanVariance.selectedSliceIndex;
    config.allanXRangeMin            = appState.allanVariance.xRangeMin;
    config.allanXRangeMax            = appState.allanVariance.xRangeMax;
    config.allanCalcBaseSelector     = appState.allanVariance.calcBaseSelector;

    // Save 100% T window settings
    config.t100YAxisMode      = appState.t100.yAxisMode;
    config.t100XUnitSelector  = appState.t100.xUnitSelector;
    config.t100ForcedYMin     = appState.t100.forcedYMin;
    config.t100ForcedYMax     = appState.t100.forcedYMax;
    strncpy(config.t100EnergyRatioNumA, appState.t100.energyRatioNumA, 31);
    strncpy(config.t100EnergyRatioDenA, appState.t100.energyRatioDenA, 31);
    strncpy(config.t100EnergyRatioNumB, appState.t100.energyRatioNumB, 31);
    strncpy(config.t100EnergyRatioDenB, appState.t100.energyRatioDenB, 31);
    strncpy(config.t100EnergyRatioNumC, appState.t100.energyRatioNumC, 31);
    strncpy(config.t100EnergyRatioDenC, appState.t100.energyRatioDenC, 31);

    // Save accent color
    config.accentColor = appState.currentAccentColor;

    // Save config to file
    if (!config.saveToFile(configFilePath)) {
        std::cerr << "Failed to save configuration to " << configFilePath << std::endl;
    } else {
        std::cout << "Configuration saved to " << configFilePath << std::endl;
    }
    
    return 0;
}
