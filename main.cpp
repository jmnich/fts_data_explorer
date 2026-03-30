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

// Include config header
#include "config.h"
#include "app_state.h"
#include "spectrum.h"

// Include imgui and other dependencies
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>

// Simple file dialog implementation (replaces NFD)
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>





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

// CSV Adapter class
class CSVAdapter {
public:
    static InterferogramData loadFromCSV(const std::string& filePath) {
        InterferogramData data;
        std::ifstream file(filePath);
        
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file: " + filePath);
        }
        
        std::string line;
        bool isFirstLine = true;
        
        while (std::getline(file, line)) {
            if (isFirstLine) {
                isFirstLine = false;
                continue; // Skip header
            }
            
            std::istringstream iss(line);
            std::string refValue, primaryValue;
            
            if (std::getline(iss, refValue, ',') && std::getline(iss, primaryValue, ',')) {
                try {
                    data.referenceDetector.push_back(std::stof(refValue));
                    data.primaryDetector.push_back(std::stof(primaryValue));
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing line: " << line << " - " << e.what() << std::endl;
                }
            }
        }
        
        return data;
    }
};

// Simple file browser
class FileBrowser {
public:
    static std::vector<std::string> getCSVFilesInDirectory(const std::string& directoryPath) {
        std::vector<std::string> csvFiles;
        
        // Return empty vector if directory path is empty
        if (directoryPath.empty()) {
            return csvFiles;
        }
        
        // Check if directory exists
        if (!std::filesystem::exists(directoryPath) || !std::filesystem::is_directory(directoryPath)) {
            return csvFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                csvFiles.push_back(entry.path().string());
            }
        }
        
        return csvFiles;
    }
    
    // Simple cross-platform directory selection dialog
    static std::string showDirectorySelectionDialog() {
        // For Linux/Unix systems, we'll use a simple approach
        // In a production app, you might use Zenity, KDialog, or a proper GUI library
        
        std::string result;
        
        // Try using zenity if available (common on Ubuntu/GNOME)
        FILE* pipe = popen("zenity --file-selection --directory --title='Select Dataset Directory' 2>/dev/null", "r");
        if (pipe) {
            char buffer[1024] = {0};
            if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                // Remove trailing newline
                buffer[strcspn(buffer, "\n")] = 0;
                // Only use the result if it's not empty and not just whitespace
                if (strlen(buffer) > 0 && buffer[0] != '\0') {
                    result = buffer;
                }
            }
            pclose(pipe);
            
            // If zenity returned a valid path, use it
            if (!result.empty()) {
                return result;
            }
        }
        
        // Fallback: use a simple ImGui-based directory selector
        // Note: This is a basic implementation. For production use, consider:
        // 1. Using a proper native file dialog library
        // 2. Implementing a non-modal directory browser
        // 3. Using a separate window for directory selection
        
        // For now, we'll use a simple approach that works within the modal context
        static char tempDirectoryBuffer[1024] = "/home"; // Default starting directory
        static bool directorySelectorActive = false;
        
        // Start directory selector if not already active
        if (!directorySelectorActive) {
            directorySelectorActive = true;
            ImGui::OpenPopup("Select Directory");
        }
        
        // Simple directory selector popup
        if (ImGui::BeginPopupModal("Select Directory", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter directory path:");
            ImGui::InputText("##DirectoryPath", tempDirectoryBuffer, sizeof(tempDirectoryBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            
            if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                // Check if directory exists
                if (std::filesystem::exists(tempDirectoryBuffer) && std::filesystem::is_directory(tempDirectoryBuffer)) {
                    result = tempDirectoryBuffer;
                    ImGui::CloseCurrentPopup();
                    directorySelectorActive = false;
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Directory does not exist!");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
                directorySelectorActive = false;
            }
            
            ImGui::EndPopup();
        }
        
        return result;
    }
};

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
                            std::vector<float> downsampledRef, downsampledPrim;
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
    
    ImGui::StyleColorsDark();
    
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
    appState.alignPeaks = config.alignPeaks; // Use config setting for peak alignment
    appState.autoRestoreScale = config.autoRestoreScale; // Use config setting for scale restoration
    appState.showFPS = config.showFPS; // Load from config
    
    // Load spectrum window settings from config
    appState.spectrum.spectrumWindowPosX = config.spectrumWindowPosX;
    appState.spectrum.spectrumWindowPosY = config.spectrumWindowPosY;
    appState.spectrum.spectrumWindowSizeX = config.spectrumWindowSizeX;
    appState.spectrum.spectrumWindowSizeY = config.spectrumWindowSizeY;
    
    // Set the appState pointer in the spectrum object for raw data access
    appState.spectrum.appState = &appState;
    
    // No initialization needed for simple file dialog
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        appState.multiSelectMode = ImGui::GetIO().KeyCtrl;
        appState.shiftSelectMode = ImGui::GetIO().KeyShift;
        
        // Handle keyboard shortcuts - only trigger once per key press
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            bool yKeyPressed = glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool aKeyPressed = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool dKeyPressed = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            
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
            
            // 'Ctrl+A' - Toggle align peaks (only on initial press)
            if (aKeyPressed && !appState.aKeyPressedLastFrame) {
                appState.alignPeaks = !appState.alignPeaks;
            }
            
            // 'Ctrl+D' - Toggle downsampling (only on initial press)
            if (dKeyPressed && !appState.dKeyPressedLastFrame) {
                appState.enableDownsampling = !appState.enableDownsampling;
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
                                std::vector<float> downsampledRef, downsampledPrim;
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
            
            // Update key state tracking for next frame
            appState.yKeyPressedLastFrame = yKeyPressed;
            appState.aKeyPressedLastFrame = aKeyPressed;
            appState.dKeyPressedLastFrame = dKeyPressed;
        } else {
            // Reset key states when keyboard is captured (e.g., typing in text field)
            appState.yKeyPressedLastFrame = false;
            appState.aKeyPressedLastFrame = false;
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
            appState.spectrum.showSpectrumWindow = false; // Close spectrum window when returning to welcome screen
            appState.spectrum.spectrumWindowInitialized = false;
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
                    std::vector<float> downsampledRef, downsampledPrim;
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
                
                // Handle autoscale behavior based on AGENTS.md requirements:
                // "when the application loads a file for display for the first time after launch or work directory switch, axes zoom to fit all data."
                if (appState.isFirstDataLoad || appState.autoRestoreScale) {
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
                
                // Mark that we should update recent datasets after this successful load
                appState.shouldUpdateRecentDatasets = true;
                
            } catch (const std::exception& e) {
                std::cerr << "Error loading file: " << e.what() << std::endl;
                appState.dataLoaded = false;
                appState.filesChanged = false;
                // Don't update recent datasets on failure
                appState.shouldUpdateRecentDatasets = false;
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
            // Safety check to prevent multiple welcome screen instances
            static bool welcomeScreenActive = false;
            if (welcomeScreenActive) {
                std::cerr << "Warning: Attempted to show welcome screen while already active!" << std::endl;
                appState.showWelcomeScreen = false; // Prevent re-entry by disabling welcome screen
            }
            welcomeScreenActive = true;
            // Center the welcome screen
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(1200, 800));
            
            // Create a modal popup that blocks all interaction
            ImGui::OpenPopup("Welcome to FTS Data Explorer");
            
            if (ImGui::BeginPopupModal("Welcome to FTS Data Explorer", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
                // Set up welcome screen with minimal styling to avoid style stack issues
                ImVec4 darkBackground(0.1f, 0.1f, 0.1f, 1.0f); // Same as main window clear color
                ImGui::PushStyleColor(ImGuiCol_PopupBg, darkBackground);
                
                // Welcome message
                // ImGui::TextColored(ImVec4(0.6f, 0.5f, 0.1f, 1.0f), "Welcome to FTS Data Explorer");
                // ImGui::Text("A tool for exploring Fourier spectrometer data");
                // ImGui::Spacing();
                // ImGui::Separator();
                ImGui::Spacing();
                
                // Recent datasets section
                ImGui::Text("Recent Datasets:");
                ImGui::Spacing();
                
                if (config.recentDatasets.empty()) {
                    ImGui::Text("No recent datasets found.");
                    ImGui::Text("Use the button below to select a dataset directory.");
                } else {
                    // Create a child window for scrollable recent datasets list
                    if (ImGui::BeginChild("RecentDatasetsChild", ImVec2(0, 500), true)) {
                        for (const auto& datasetPath : config.recentDatasets) {
                            // Extract the parent directory name for display (skip "raw_data" if present)
                            std::string displayName = datasetPath;
                            size_t last_slash = displayName.find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                displayName = displayName.substr(last_slash + 1);
                                // If this is "raw_data", get the parent directory name
                                if (displayName == "raw_data" && last_slash > 0) {
                                    size_t parent_slash = datasetPath.substr(0, last_slash).find_last_of("/\\");
                                    if (parent_slash != std::string::npos) {
                                        displayName = datasetPath.substr(parent_slash + 1, last_slash - parent_slash - 1);
                                    }
                                }
                            }
                            
                            // Create a button for each recent dataset
                            if (ImGui::Button(displayName.c_str(), ImVec2(-FLT_MIN, 0))) {
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
                                    appState.dataLoaded = false;
                                    appState.filesChanged = true;
                                    appState.currentSortedFileIndex = 0;
                                    appState.showWelcomeScreen = false;
                                    appState.welcomeScreenInitialized = true;
                                    appState.isFirstDataLoad = true; // Reset first load flag for new directory
                                    std::cout << "Opened recent dataset: " << datasetPath << std::endl;
                                    ImGui::CloseCurrentPopup(); // Close the modal
                                } else {
                                    std::cerr << "Recent dataset path no longer exists: " << datasetPath << std::endl;
                                }
                            }
                            
                            // Add tooltip with full path
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", datasetPath.c_str());
                            }
                        }
                        ImGui::EndChild();
                    }
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
                
                // Directory selection button with dark green background
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.3f, 0.1f, 0.8f)); // Dark green
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.35f, 0.15f, 0.9f)); // Lighter green on hover
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.05f, 0.25f, 0.05f, 1.0f)); // Even darker when active
                
                // Calculate available space for the button
                float buttonHeight = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y * 2;
                bool buttonClicked = ImGui::Button("Select Dataset Directory", ImVec2(-FLT_MIN, buttonHeight));
                ImGui::PopStyleColor(3); // Always pop styles after button
                
                if (buttonClicked) {
                    // For welcome screen, use zenity directly since we're in a modal context
                    std::string selectedDirectory;
                    FILE* pipe = popen("zenity --file-selection --directory --title='Select Dataset Directory' 2>/dev/null", "r");
                    if (pipe) {
                        char buffer[1024] = {0};
                        if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                            // Remove trailing newline
                            buffer[strcspn(buffer, "\n")] = 0;
                            // Only use the result if it's not empty and not just whitespace
                            if (strlen(buffer) > 0 && buffer[0] != '\0') {
                                selectedDirectory = buffer;
                            }
                        }
                        pclose(pipe);
                    }
                    
                    if (!selectedDirectory.empty()) {
                        // Check if the selected directory has a raw_data subdirectory
                        std::string rawDataPath = selectedDirectory + "/raw_data";
                        if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                            appState.currentDirectory = rawDataPath;
                            // Update dataset name from parent directory
                            appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                        } else {
                            appState.currentDirectory = selectedDirectory;
                            // Update dataset name from selected directory
                            appState.currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                        }
                        appState.csvFiles = FileBrowser::getCSVFilesInDirectory(appState.currentDirectory);
                        appState.dataLoaded = false;
                        appState.filesChanged = true;
                        appState.currentSortedFileIndex = 0;
                        appState.showWelcomeScreen = false;
                        appState.welcomeScreenInitialized = true;
                        std::cout << "Working directory set to: " << appState.currentDirectory << std::endl;
                        ImGui::CloseCurrentPopup(); // Close the modal
                    }
                }
                // Clean up welcome screen styles
                ImGui::PopStyleColor(); // PopupBg only (removed other styles to simplify)

                ImGui::EndPopup();
                
                // If welcome screen is closed manually, initialize the app
                if (!appState.showWelcomeScreen) {
                    appState.welcomeScreenInitialized = true;
                }
                welcomeScreenActive = false; // Reset the safety flag when welcome screen closes
            } else {
                // Popup wasn't opened, no styles to clean up (they're pushed inside BeginPopupModal)
                welcomeScreenActive = false; // Reset the safety flag if popup wasn't opened
            }
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
                            appState.dataLoaded = false;
                            appState.shouldUpdateRecentDatasets = true; // Mark to update recent datasets
                            appState.isFirstDataLoad = true; // Reset first load flag for new directory
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
                    ImGui::MenuItem("Align peaks", NULL, &appState.alignPeaks);
                    if (ImGui::MenuItem("Autorestore scale", NULL, &appState.autoRestoreScale)) {
                        // When enabling autorestore scale, trigger autoscale to fit all data
                        if (appState.autoRestoreScale && appState.dataLoaded) {
                            appState.shouldAutoscale = true;
                        }
                    }
                    
                    // Auto-fit Y-axis toggle
                    if (ImGui::MenuItem("Auto-fit Y-axis", NULL, &appState.autoFitYAxis)) {
                        // When toggling auto-fit, recalculate limits if enabling auto-fit
                        if (appState.autoFitYAxis && appState.dataLoaded) {
                            auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                            auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                            appState.ref_y_min = *ref_min_max.first;
                            appState.ref_y_max = *ref_min_max.second;
                            appState.prim_y_min = *prim_min_max.first;
                            appState.prim_y_max = *prim_min_max.second;
                        }
                    }
                    
                    // Downsampling toggle
                    if (ImGui::MenuItem("Enable downsampling", NULL, &appState.enableDownsampling)) {
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
                                        std::vector<float> downsampledRef, downsampledPrim;
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
                                // Force X-axis to show all data when downsampling is toggled
                                appState.zoomRange = {0, 0};
                                // Set flag to force autofit on next render
                                appState.shouldAutoscale = true;
                                appState.forceXAutofit = true; // Set global flag to force X-axis autofit
                                // Note: We don't change autoRestoreScale setting, so other behaviors are preserved
                                std::cout << "Reloaded " << appState.loadedData.size() << " datasets with " 
                                          << (appState.enableDownsampling ? "enabled" : "disabled") << " downsampling" << std::endl;
                            }
                        }
                    }
                    
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
                    ImGui::Text("Ctrl+A: Toggle align peaks");
                    ImGui::Text("Ctrl+D: Toggle downsampling");
                    ImGui::Text("Ctrl+H: Go back to home");
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
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
            
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
            
            if (ImGui::Button(filename.c_str())) {
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
                                    std::vector<float> downsampledRef, downsampledPrim;
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
                                std::vector<float> downsampledRef, downsampledPrim;
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
        
        // Graphing panel (main) - now using ImPlot
        bool isMainWindowFocused = false;
        ImGui::Begin("Graphing Panel");
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
            // Apply peak alignment if enabled
            std::vector<InterferogramData> alignedData = appState.loadedData;
            if (appState.alignPeaks && appState.loadedData.size() > 1) {
                // Find the peak position in the first dataset's primary detector (reference)
                size_t referencePeakPos = 0;
                float referencePeakValue = appState.loadedData[0].primaryDetector[0];
                for (size_t i = 1; i < appState.loadedData[0].primaryDetector.size(); i++) {
                    if (appState.loadedData[0].primaryDetector[i] > referencePeakValue) {
                        referencePeakValue = appState.loadedData[0].primaryDetector[i];
                        referencePeakPos = i;
                    }
                }
                
                // Align all other datasets to this peak position
                for (size_t datasetIdx = 1; datasetIdx < appState.loadedData.size(); datasetIdx++) {
                    // Find peak in current dataset's primary detector
                    size_t currentPeakPos = 0;
                    float currentPeakValue = appState.loadedData[datasetIdx].primaryDetector[0];
                    for (size_t i = 1; i < appState.loadedData[datasetIdx].primaryDetector.size(); i++) {
                        if (appState.loadedData[datasetIdx].primaryDetector[i] > currentPeakValue) {
                            currentPeakValue = appState.loadedData[datasetIdx].primaryDetector[i];
                            currentPeakPos = i;
                        }
                    }
                    
                    // Calculate shift needed to align peaks
                    int shift = referencePeakPos - currentPeakPos;
                    
                    // Apply shift to both reference and primary detectors
                    std::vector<float> shiftedRef(appState.loadedData[datasetIdx].referenceDetector.size(), 0.0f);
                    std::vector<float> shiftedPrim(appState.loadedData[datasetIdx].primaryDetector.size(), 0.0f);
                    
                    if (shift > 0) {
                        // Shift right - pad beginning with zeros
                        for (size_t i = 0; i < appState.loadedData[datasetIdx].referenceDetector.size() - shift; i++) {
                            shiftedRef[i + shift] = appState.loadedData[datasetIdx].referenceDetector[i];
                            shiftedPrim[i + shift] = appState.loadedData[datasetIdx].primaryDetector[i];
                        }
                    } else if (shift < 0) {
                        // Shift left - pad end with zeros
                        shift = -shift; // Make positive
                        for (size_t i = 0; i < appState.loadedData[datasetIdx].referenceDetector.size() - shift; i++) {
                            shiftedRef[i] = appState.loadedData[datasetIdx].referenceDetector[i + shift];
                            shiftedPrim[i] = appState.loadedData[datasetIdx].primaryDetector[i + shift];
                        }
                    } else {
                        // No shift needed
                        shiftedRef = appState.loadedData[datasetIdx].referenceDetector;
                        shiftedPrim = appState.loadedData[datasetIdx].primaryDetector;
                    }
                    
                    // Update aligned data
                    alignedData[datasetIdx].referenceDetector = shiftedRef;
                    alignedData[datasetIdx].primaryDetector = shiftedPrim;
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
                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels, y_flags);
                    // Conditionally optimize grid rendering for large datasets
                    if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    if (appState.shouldAutoscale || appState.forceXAutofit) {
                        // Set initial view to show all data when new data is loaded or when downsampling is toggled
                        if (!appState.autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(0, appState.loadedData[0].referenceDetector.size(), appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit both axes: set X-axis to full range, let Y-axis auto-fit
                            ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].referenceDetector.size(), ImPlotCond_Always);
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
                    // Plot all selected datasets with pre-allocated specs
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].referenceDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& dataToPlot = appState.alignPeaks ? alignedData[i] : appState.loadedData[i];
                                if (ref_start < dataToPlot.referenceDetector.size()) {
                                    size_t actual_count = std::min(data_count, dataToPlot.referenceDetector.size() - ref_start);
                                    ImPlot::PlotLine("", 
                                                   &dataToPlot.referenceDetector[ref_start], 
                                                   actual_count, 1.0, 0.0, plotSpecs[i]);
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


                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks, y_flags);

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
                        if (!appState.autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(0, appState.loadedData[0].primaryDetector.size(), appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit both axes: set X-axis to full range, let Y-axis auto-fit
                            ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].primaryDetector.size(), ImPlotCond_Always);
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (appState.applyXRangeSelection && appState.selectionStartX != appState.selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, appState.selectionStartX, appState.selectionEndX, ImPlotCond_Always);
                        appState.applyXRangeSelection = false; // Reset flag after applying
                    }
                    // Reuse the same plot specs as reference plot (already pre-allocated)
                    // Plot all selected datasets with same colors as reference
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].primaryDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& dataToPlot = appState.alignPeaks ? alignedData[i] : appState.loadedData[i];
                                if (ref_start < dataToPlot.primaryDetector.size()) {
                                    size_t actual_count = std::min(data_count, dataToPlot.primaryDetector.size() - ref_start);
                                    ImPlot::PlotLine("", 
                                                   &dataToPlot.primaryDetector[ref_start], 
                                                   actual_count, 1.0, 0.0, plotSpecs[i]);
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
            ImGui::Text("No data loaded. Select a CSV file from the Files panel.");
        }
        ImGui::End();
        
        // Spectrum panel (bottom)
        ImGui::Begin("Spectrum");
        if (appState.dataLoaded) {
            if (ImGui::Button("Spectrum")) {
                appState.spectrum.showSpectrumWindow = true;
            }
            
            // Add new controls to Spectrum panel
            ImGui::Separator();
            ImGui::Text("Controls:");
            
            // X unit selector
            const char* xUnitItems[] = { "cm-1", "um", "THz" };
            ImGui::Text("X unit:");
            ImGui::SameLine();
            ImGui::Combo("##XUnitSelector", &appState.spectrum.xUnitSelector, xUnitItems, IM_ARRAYSIZE(xUnitItems));
            
            // Reference laser textbox
            ImGui::Text("Ref laser [um]:");
            ImGui::SameLine();
            ImGui::InputText("##RefLaserTextbox", appState.spectrum.refLaserTextbox, sizeof(appState.spectrum.refLaserTextbox));
            
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

        // Spectrum view window
        if (appState.spectrum.showSpectrumWindow && appState.dataLoaded && !appState.loadedData.empty()) {
            // Prepare data for all selected files
            std::vector<std::pair<std::string, std::vector<float>>> primaryDetectors;
            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                primaryDetectors.emplace_back(appState.selectedFilenames[i], appState.loadedData[i].primaryDetector);
            }
            appState.spectrum.renderSpectrumWindow(primaryDetectors, appState.rawDataCache, appState.autoFitYAxis);
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
    cleanupApplication(window);
    
    // Save configuration before exiting
    config.alignPeaks = appState.alignPeaks;
    config.autoRestoreScale = appState.autoRestoreScale;
    config.lastWorkingDirectory = appState.currentDirectory;
    config.uiSize = appState.currentUiSize;
    
    // Update recent datasets if we had a successful load
    if (appState.shouldUpdateRecentDatasets && appState.dataLoaded && !appState.selectedFiles.empty()) {
        // Add the parent directory of the current dataset to recent datasets
        std::string datasetPath = appState.selectedFiles[0];
        size_t last_slash = datasetPath.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            std::string parentDir = datasetPath.substr(0, last_slash);
            // Check if this is a raw_data directory, if so, go up one more level
            size_t raw_data_pos = parentDir.find_last_of("/\\");
            if (raw_data_pos != std::string::npos) {
                std::string dirName = parentDir.substr(raw_data_pos + 1);
                if (dirName == "raw_data") {
                    parentDir = parentDir.substr(0, raw_data_pos);
                }
            }
            config.addRecentDataset(parentDir);
        }
    }
    
    // Update config with current FPS setting before saving
    config.showFPS = appState.showFPS;
    
    // Save spectrum window settings
    config.spectrumWindowPosX = appState.spectrum.spectrumWindowPosX;
    config.spectrumWindowPosY = appState.spectrum.spectrumWindowPosY;
    config.spectrumWindowSizeX = appState.spectrum.spectrumWindowSizeX;
    config.spectrumWindowSizeY = appState.spectrum.spectrumWindowSizeY;
    
    // Save config to file
    if (!config.saveToFile(configFilePath)) {
        std::cerr << "Failed to save configuration to " << configFilePath << std::endl;
    } else {
        std::cout << "Configuration saved to " << configFilePath << std::endl;
    }
    
    return 0;
}
