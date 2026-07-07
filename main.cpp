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
#include "spectral_toolbox.h"
#include "adapters/csv_adapter.h"
#include "tinyfiledialogs.h"
#include "file_browser.h"
#include "welcome.h"

// Include imgui and other dependencies
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
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



/**
 * Initialize GLFW, ImGui, and application state
 * @param config Application configuration
 * @param window Reference to GLFW window pointer
 * @return true if initialization successful, false otherwise
 */
bool initializeApplication(AppConfig& config, GLFWwindow*& window) {
    std::cout << "FTS Data Explorer - Starting application..." << std::endl;

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
    window = glfwCreateWindow(config.windowWidth, config.windowHeight, "FTS Data Explorer", nullptr, nullptr);
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

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // Initialize ImPlot context
    ImPlot::CreateContext();
    
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
                        InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
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
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
}

int main() {
    // Set environment variables to prefer dedicated GPU on NVIDIA systems
    #ifdef _WIN32
    _putenv("D3D12_ENABLE_LAYERED_DRIVER_QUERY=1");
    _putenv("D3D12_ENABLE_EXPERIMENTAL_FEATURES=1");
    #else
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
    #endif
    
    std::cout << "FTS Data Explorer - Starting application..." << std::endl;
    
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

    // UI size settings
    appState.currentUiSize = config.uiSize;

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

    // Load welcome screen background texture
    initWelcomeBackground();
    
    // Customize colors to use black background
    ImGuiStyle& style = ImGui::GetStyle();
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
    ImPlotStyle& plotStyle = ImPlot::GetStyle();
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
    
    appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
    appState.maxAtZero = config.maxAtZero; // Use config setting for peak alignment
    appState.autoFitYAxis = config.autoFitYAxis; // Load from config
    appState.enableDownsampling = config.enableDownsampling; // Load from config
    appState.xAxisBase = config.xAxisBase; // Load from config
    appState.showFPS = config.showFPS; // Load from config
    
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
    appState.spectrum.apodizationParams.rectAsymMode = config.apodRectAsymMode;
    appState.spectrum.detectorSensitivity = config.spectrumDetectorSensitivity;
    
    // Set the appState pointer in the spectrum object for raw data access
    appState.spectrum.appState = &appState;
    appState.averageSpectrum.appState = &appState;
    appState.snrSpectrum.appState = &appState;
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
    
    // No initialization needed for simple file dialog
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Tick average spectrum calculation (multi-frame, one file per frame)
        if (appState.averageSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.averageSpectrum.tickCalculation();
        }

        // Tick SNR spectrum calculation (multi-frame, one file per frame)
        if (appState.snrSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.snrSpectrum.tickCalculation();
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
        appState.needsRedraw = false;

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
                    auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                    auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                    appState.ref_y_min = *ref_min_max.first;
                    appState.ref_y_max = *ref_min_max.second;
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
                            InterferogramData data = CSVAdapter::loadFromCSV(filePath);
                            
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
                        for (const auto& file : appState.selectedFiles) {
                            try {
                                // Load the original raw data from file
                                InterferogramData rawData = CSVAdapter::loadFromCSV(file);
                                appState.rawDataCache.push_back(rawData);
                            } catch (const std::exception& e) {
                                std::cerr << "Error reloading raw data for spectrum: " << e.what() << std::endl;
                                // If we can't reload raw data, use processed data as fallback
                                // This ensures spectrum can still be computed
                                appState.rawDataCache.push_back(reloadedData[&file - &appState.selectedFiles[0]]);
                            }
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
            appState.clearAverageSpectrum();
            appState.clearSnrSpectrum();
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
        if (appState.filesChanged && !appState.csvFiles.empty()) {
            try {
                // Load the currently selected file
                InterferogramData data = CSVAdapter::loadFromCSV(appState.sortedFiles[appState.currentSortedFileIndex]);
                

                
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
                    auto ref_min_max = std::minmax_element(data.referenceDetector.begin(), data.referenceDetector.end());
                    auto prim_min_max = std::minmax_element(data.primaryDetector.begin(), data.primaryDetector.end());
                    appState.ref_y_min = *ref_min_max.first;
                    appState.ref_y_max = *ref_min_max.second;
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
                        addToRecentDatasets(config, configFilePath, parentDir);
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
        
        // Conditionally disable anti-aliasing for large datasets (>50k points)
        if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
            ImGui::GetStyle().AntiAliasedLines = false;
        }
        
        // Show welcome screen if no data is loaded and we haven't initialized yet
        if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
            renderWelcomeScreen(appState, config, configFilePath);
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
                        std::string selectedDirectory = FileBrowser::showDirectorySelectionDialog();
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
                            appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                            appState.clearAverageSpectrum();
            appState.clearSnrSpectrum();
                            appState.dataLoaded = false;
                            appState.isFirstDataLoad = true;
                            appState.needsRedraw = true;
                            addToRecentDatasets(config, configFilePath, selectedDirectory);
                            std::cout << "Working directory set to: " << appState.currentDirectory << std::endl;
                        }
                    }
                    
                    // Recent datasets menu
                    if (!config.recentDatasets.empty()) {
                        if (ImGui::BeginMenu("Recent Datasets")) {
                            for (const auto& datasetPath : config.recentDatasets) {
                                if (ImGui::MenuItem(datasetPath.c_str())) {
                                    // Extract just the directory name for display
                                    size_t last_slash = datasetPath.find_last_of("/\\");
                                    std::string displayName = (last_slash != std::string::npos) 
                                        ? datasetPath.substr(last_slash + 1) 
                                        : datasetPath;
                                    
                                    if (std::filesystem::exists(datasetPath) && std::filesystem::is_directory(datasetPath)) {
                                        // Check if there's a raw_data subdirectory
                                        std::string rawDataPath = datasetPath + "/raw_data";
                                        if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                                            appState.currentDirectory = rawDataPath; // Use the raw_data subdirectory
                                        } else {
                                            appState.currentDirectory = datasetPath; // Fallback to the dataset directory itself
                                        }
                                        
                                        // Update current dataset name
                                        appState.currentDatasetName = datasetPath.substr(datasetPath.find_last_of("/\\") + 1);
                                        
                                        appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                                        appState.clearAverageSpectrum();
            appState.clearSnrSpectrum();
                                        appState.dataLoaded = false;
                                        appState.filesChanged = true;
                                        appState.currentSortedFileIndex = 0;
                                        appState.isFirstDataLoad = true; // Reset first load flag for new directory
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
                    
                    ImGui::EndMenu();
                }
                
                // Help menu
                if (ImGui::BeginMenu("Help"))
                {
                    ImGui::Text("Keyboard Shortcuts:");
                    ImGui::Separator();
                    ImGui::Text("Up/Down Arrows: Navigate files");
                    ImGui::Text("Shift + mouse / Right click: X-axis range selection");
                    ImGui::Text("ESC: Reset zoom");
                    ImGui::Text("Mouse Scroll: Zoom in/out");
                    ImGui::Text("Ctrl+Y: Toggle auto-fit Y-axis");
                    ImGui::Text("Ctrl+A: Toggle max at zero");
                    ImGui::Text("Ctrl+D: Toggle downsampling");
                    ImGui::Text("Ctrl+H: Go back to home");
                    ImGui::Text("Ctrl+Q: Toggle tracking cursor");
                    ImGui::EndMenu();
                }
                
                ImGui::EndMainMenuBar();
            }

            // Push style variables for full viewport docking
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            
            // Make the dockspace background match the welcome screen when welcome screen is active
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            }
            
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            
            // Adjust window position to account for menu bar
            ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + ImGui::GetFrameHeight()));
            ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - ImGui::GetFrameHeight()));
            
            // Create main dockspace window
            ImGui::Begin("DockSpace", nullptr, window_flags);
            ImGui::PopStyleVar(2);
            
            // Create docking space
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v2");
            
            // Apply default layout only on first launch (no saved imgui.ini)
            if (!appState.defaultLayoutApplied) {
                appState.defaultLayoutApplied = true;
                
                const char* iniPath = io.IniFilename ? io.IniFilename : "imgui.ini";
                bool iniExists = std::filesystem::exists(iniPath) && std::filesystem::file_size(iniPath) > 0;
                
                if (!iniExists) {
                    ImGui::DockBuilderRemoveNode(dockspace_id);
                    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                    ImGui::DockBuilderSetNodeSize(dockspace_id,
                        ImVec2(viewport->Size.x, viewport->Size.y - ImGui::GetFrameHeight()));
                    
                    // Split dockspace: left (16%) / right (84%)
                    ImGuiID dock_left, dock_right;
                    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.16f, &dock_left, &dock_right);
                    
                    // Split left: Files (40%) / bottom (60%)
                    ImGuiID dock_left_top, dock_left_bottom;
                    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.40f, &dock_left_top, &dock_left_bottom);
                    
                    // Split left-bottom: Metadata/Export/SNR (50%) / Spectrum/Interferogram/Average (50%)
                    ImGuiID dock_left_bottom_top, dock_left_bottom_bottom;
                    ImGui::DockBuilderSplitNode(dock_left_bottom, ImGuiDir_Up, 0.50f, &dock_left_bottom_top, &dock_left_bottom_bottom);
                    
                    // Split right: Interferogram View (48%) / right panel (52%)
                    ImGuiID dock_center, dock_right_panel;
                    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Left, 0.48f, &dock_center, &dock_right_panel);
                    
                    // Split right panel: view tabs (50%) / Spectrum View (50%)
                    ImGuiID dock_right_top, dock_right_bottom;
                    ImGui::DockBuilderSplitNode(dock_right_panel, ImGuiDir_Up, 0.50f, &dock_right_top, &dock_right_bottom);
                    
                    // Dock all windows to their zones
                    ImGui::DockBuilderDockWindow("Files",              dock_left_top);
                    ImGui::DockBuilderDockWindow("Metadata",           dock_left_bottom_top);
                    ImGui::DockBuilderDockWindow("Export",             dock_left_bottom_top);
                    ImGui::DockBuilderDockWindow("SNR",                dock_left_bottom_top);
                    ImGui::DockBuilderDockWindow("Spectrum",           dock_left_bottom_bottom);
                    ImGui::DockBuilderDockWindow("Interferogram",      dock_left_bottom_bottom);
                    ImGui::DockBuilderDockWindow("Average",            dock_left_bottom_bottom);
                    ImGui::DockBuilderDockWindow("Interferogram View", dock_center);
                    ImGui::DockBuilderDockWindow("SNR View",           dock_right_top);
                    ImGui::DockBuilderDockWindow("Average View",       dock_right_top);
                    ImGui::DockBuilderDockWindow("Spectrum View",      dock_right_bottom);
                    
                    ImGui::DockBuilderFinish(dockspace_id);
                }
            }
            
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), 0);
            
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PopStyleColor(); // Restore window background color
            }
            
            ImGui::End();
        
        // Files panel (left)
        ImGui::Begin("Files");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        ImGui::Text("Current Dataset: %s", appState.currentDatasetName.c_str());
        
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
        
        for (size_t i = 0; i < appState.sortedFiles.size(); i++) {
            const auto& file = appState.sortedFiles[i];
            // Extract just the filename without path
            std::string filename = file;
            size_t last_slash = filename.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                filename = filename.substr(last_slash + 1);
            }
            
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
            
            // Compute widths: button on left, checkbox on right
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
                                InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
        
                                
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
                            InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
                            
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
        }
        // Show selection limit popup if needed
        if (ImGui::BeginPopupModal("Selection Limit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Maximum of %zu files can be selected at once.", appState.MAX_SELECTABLE_FILES);
            ImGui::Text("Please deselect some files first.");
            
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
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
        
        if (appState.dataLoaded) {
            // Y-axis limits are now handled by the auto-fit toggle
            // When autoFitYAxis is true, ImPlot will auto-calculate Y-axis limits
            // When autoFitYAxis is false, we use the manually calculated limits
            
            // Determine zoom range
            size_t ref_start =  0;
            size_t ref_end =  appState.loadedData[0].referenceDetector.size();
            size_t prim_start =  0;
            size_t prim_end =  appState.loadedData[0].primaryDetector.size();
            // Compute peak positions for X-axis alignment
            std::vector<size_t> peakPositions;
            if (appState.maxAtZero) {
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    auto peakIt = std::max_element(appState.loadedData[i].primaryDetector.begin(),
                                                   appState.loadedData[i].primaryDetector.end());
                    peakPositions.push_back(static_cast<size_t>(std::distance(appState.loadedData[i].primaryDetector.begin(), peakIt)));
                }
            }
            
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
            float row_ratios[2] = {1.0f, 2.0f}; // Reference:Primary height ratio
            
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

            
            if (ImPlot::BeginSubplots("Detector Plots", 2, 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend, row_ratios)) {

                // Reference detector plot (top)
                ImPlotFlags ref_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
                if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                    ref_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }
                // Never show crosshairs
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
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    // Ensure Hilbert X cache is populated for OPD mode
                    if (appState.xAxisBase == 1 && appState.dataLoaded) {
                        if (appState.hilbertCacheLaserWavelength != appState.spectrum.refLaserTextbox) {
                            appState.hilbertXCache.clear();
                            appState.hilbertCacheLaserWavelength = appState.spectrum.refLaserTextbox;
                        }
                        for (size_t i = 0; i < appState.loadedData.size(); i++) {
                            const std::string& fileId = appState.selectedFilenames[i];
                            if (appState.hilbertXCache.find(fileId) == appState.hilbertXCache.end()) {
                                std::vector<double> hilbX;
                                const auto& refDet = appState.loadedData[i].referenceDetector;
                                SpectralToolbox::xAxisFromHilbert(refDet, appState.hilbertCacheLaserWavelength, hilbX);
                                appState.hilbertXCache[fileId] = hilbX;
                            }
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
                                    double off = static_cast<double>(peakPositions[i]);
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
                                double off = static_cast<double>(peakPositions[0]);
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
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = hilbX[j] - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                ImPlot::PlotLine("", hilbX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(peakPositions[i]);
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

                
                
                // Primary detector plot (bottom)
                ImPlotFlags prim_flags = ImPlotFlags_NoTitle;
                if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                    prim_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }

                // Never show crosshairs
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
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
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
                                    double off = static_cast<double>(peakPositions[i]);
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
                                double off = static_cast<double>(peakPositions[0]);
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
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = hilbX[j] - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                ImPlot::PlotLine("", hilbX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(peakPositions[i]);
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
                                        if (appState.maxAtZero && !peakPositions.empty()) {
                                            std::vector<double> shiftedHilbX(hilbX.size());
                                            double peakHilbX = hilbX[peakPositions[0]];
                                            for (size_t j = 0; j < hilbX.size(); j++)
                                                shiftedHilbX[j] = hilbX[j] - peakHilbX;
                                            ImPlot::PlotLine("##ApodWindow", shiftedHilbX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                        } else {
                                            ImPlot::PlotLine("##ApodWindow", hilbX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                        }
                                    }
                                } else if (appState.maxAtZero && !peakPositions.empty()) {
                                    std::vector<double> shiftedX(window.size());
                                    int peak = static_cast<int>(peakPositions[0]);
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
                
                // Reset autoscale flag after use
                if (appState.shouldAutoscale) {
                    appState.shouldAutoscale = false;
                }
                
                ImPlot::EndSubplots();
            }
            
            
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
                appState.needsRedraw = true;
            };

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
                    invalidateSpectrumCaches();
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
                    invalidateSpectrumCaches();
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
                    invalidateSpectrumCaches();
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

            if (forceSelected) {
                ImGui::Text("min:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##ForcedYMin", &appState.spectrum.forcedYMin, 0.0, 0.0, "%.6g")) {
                    appState.needsRedraw = true;
                }
                ImGui::SameLine();
                ImGui::Text("max:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputDouble("##ForcedYMax", &appState.spectrum.forcedYMax, 0.0, 0.0, "%.6g")) {
                    appState.needsRedraw = true;
                }
                if (appState.spectrum.forcedYMin >= appState.spectrum.forcedYMax) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(min<max!)");
                }
            }

            ImGui::Separator();

            // Detector sensitivity textbox
            ImGui::Text("Detector sensitivity [kV/W]:");
            ImGui::SameLine();

            float remWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remWidth);
            ImGui::InputFloat("##DetectorSensitivity", &appState.spectrum.detectorSensitivity, 0.0f, 0.0f, "%.4f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                invalidateSpectrumCaches();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Detector sensitivity in kV/W. Converts spectrum from V to W and enables dBm units.\nLeave at 0 to skip conversion.");
            }

            // Reference laser textbox
            ImGui::Text("Ref laser [\xC2\xB5""m]:");
            ImGui::SameLine();

            float remainingWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(remainingWidth);
            ImGui::InputFloat("##RefLaserTextbox", &(appState.spectrum.refLaserTextbox), 0.001, 0.01);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                invalidateSpectrumCaches();
            }

            // Zero-pad factor K
            ImGui::Text("Zero-pad K:");
            ImGui::SameLine();

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
                    ImGui::SetTooltip("Rectangular window width fraction (0.05-1.0).\n1.0 = full signal, 0.05 = 5%% of signal.");
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

            // Select All / Select None
            ImGui::Text("Select");
            ImGui::SameLine();
            if (ImGui::Button("All")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    appState.filesSelectedForAveraging[i] = true;
                appState.needsRedraw = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("None")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    appState.filesSelectedForAveraging[i] = false;
                appState.needsRedraw = true;
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
            if (ImGui::Button("cm-1##AvgXUnitCm")) { appState.averageSpectrum.xUnitSelector = 0; if (appState.averageSpectrum.averageAvailable && !appState.averageSpectrum.calcInProgress) appState.averageSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[umSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  umSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##AvgXUnitUm")) { appState.averageSpectrum.xUnitSelector = 1; if (appState.averageSpectrum.averageAvailable && !appState.averageSpectrum.calcInProgress) appState.averageSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[thzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  thzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##AvgXUnitTHz")) { appState.averageSpectrum.xUnitSelector = 2; if (appState.averageSpectrum.averageAvailable && !appState.averageSpectrum.calcInProgress) appState.averageSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);

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

            ImGui::Separator();

            ImGui::Text("Select");
            ImGui::SameLine();
            if (ImGui::Button("All##SnrAll")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    appState.filesSelectedForAveraging[i] = true;
                appState.needsRedraw = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("None##SnrNone")) {
                for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                    appState.filesSelectedForAveraging[i] = false;
                appState.needsRedraw = true;
            }

            ImGui::Separator();

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
            if (ImGui::Button("cm-1##SnrXUnitCm")) { appState.snrSpectrum.xUnitSelector = 0; if (appState.snrSpectrum.snrAvailable && !appState.snrSpectrum.calcInProgress) appState.snrSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrUmSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrUmSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("\xC2\xB5""m##SnrXUnitUm")) { appState.snrSpectrum.xUnitSelector = 1; if (appState.snrSpectrum.snrAvailable && !appState.snrSpectrum.calcInProgress) appState.snrSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        btnColors[snrThzSel ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  snrThzSel ? btnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   btnColors[1]);
            if (ImGui::Button("THz##SnrXUnitTHz")) { appState.snrSpectrum.xUnitSelector = 2; if (appState.snrSpectrum.snrAvailable && !appState.snrSpectrum.calcInProgress) appState.snrSpectrum.startCalculation(); }
            ImGui::PopStyleColor(3);

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

        // Interferogram Config panel (docked)
        ImGui::Begin("Interferogram");
        if (appState.dataLoaded) {
            const ImVec4 cfgBtnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // Row 1: X axis base (sample / OPD)
            ImGui::Text("X axis base");
            ImGui::SameLine();
            const bool xSample = (appState.xAxisBase == 0);
            const bool xOPD = (appState.xAxisBase == 1);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[xSample ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  xSample ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("sample##XBaseSample")) {
                if (!xSample) {
                    appState.xAxisBase = 0;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
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
                        auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                        auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                        appState.ref_y_min = *ref_min_max.first;
                        appState.ref_y_max = *ref_min_max.second;
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
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = CSVAdapter::loadFromCSV(filePath);
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
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = CSVAdapter::loadFromCSV(file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    appState.rawDataCache.push_back(reloadedData[&file - &appState.selectedFiles[0]]);
                                }
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
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = CSVAdapter::loadFromCSV(filePath);
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
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = CSVAdapter::loadFromCSV(file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    appState.rawDataCache.push_back(reloadedData[&file - &appState.selectedFiles[0]]);
                                }
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
            ImGui::Text("Samples: %zu", appState.loadedData[0].referenceDetector.size());
            
            // Display comments if comments.txt exists
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
            ImGui::Text("No metadata available.");
        }
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();
        
        // Spectrum View panel (docked)
        ImGui::Begin("Spectrum View");
        if (appState.dataLoaded && !appState.loadedData.empty()) {
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
    config.apodRectAsymMode     = appState.spectrum.apodizationParams.rectAsymMode;
    config.spectrumDetectorSensitivity = appState.spectrum.detectorSensitivity;
    
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
    
    // Save config to file
    if (!config.saveToFile(configFilePath)) {
        std::cerr << "Failed to save configuration to " << configFilePath << std::endl;
    } else {
        std::cout << "Configuration saved to " << configFilePath << std::endl;
    }
    
    return 0;
}
