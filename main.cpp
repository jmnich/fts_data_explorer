#include <iostream>
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

// Data structure to hold interferogram data
struct InterferogramData {
    std::vector<float> referenceDetector;
    std::vector<float> primaryDetector;
    std::string metadata;
};

// Dummy variables for function defaults
static std::vector<std::string> dummySelectedFiles;
static std::vector<std::string> dummySelectedFilenames;
static std::vector<InterferogramData> dummyLoadedData;
static bool dummyDataLoaded = false;
static std::vector<std::string> dummySortedFiles;

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

    return true;
}

/**
 * Configure ImGui style and colors
 * @param io ImGuiIO reference for DPI scaling
 * @param config Application configuration for UI settings
 * @param uiScale UI scale factor
 */
void setupUIStyle(ImGuiIO& io, const AppConfig& config, float& uiScale, const std::string& currentUiSize) {
    // Initialize ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
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
    ImVec4 background_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f); // Black background

    // Set plot colors
    style.Colors[ImGuiCol_PlotLines] = yellow_color;
    style.Colors[ImGuiCol_PlotLinesHovered] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogram] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogramHovered] = yellow_color;

    // Initialize ImPlot context
    ImPlot::CreateContext();

    // Set up for high DPI displays AFTER backend initialization
    // Apply scaling based on UI size setting
    float dpi_scale = io.DisplayFramebufferScale.x;

    // Update scale based on UI size
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

    // Apply the scaling (font only initially to avoid UI issues)
    io.FontGlobalScale = dpi_scale * uiScale;
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
                             bool shiftSelectMode = false, 
                             std::vector<std::string>& selectedFiles = dummySelectedFiles, 
                             std::vector<std::string>& selectedFilenames = dummySelectedFilenames, 
                             std::vector<InterferogramData>& loadedData = dummyLoadedData, 
                             bool& dataLoaded = dummyDataLoaded, 
                             const std::vector<std::string>& sortedFiles = dummySortedFiles, 
                             bool enableDownsampling = true, 
                             size_t maxPointsBeforeDownsampling = 50000, 
                             size_t maxSelectableFiles = 5) {
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
                        
                        // Apply downsampling
                        if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                            size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
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
                        }
                        
                        loadedData.push_back(data);
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
    std::string currentUiSize = config.uiSize;
    float uiScale = 1.0f; // Default scale
    bool uiSizeChanged = false; // Flag to track UI size changes
    
    // Initialize application
    GLFWwindow* window = nullptr;
    if (!initializeApplication(config, window)) {
        return -1;
    }
    
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
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
    
    // Initialize ImPlot context
    ImPlot::CreateContext();
    
    // Set up for high DPI displays AFTER backend initialization
    // Apply scaling based on UI size setting
    float dpi_scale = io.DisplayFramebufferScale.x;
    
    // Apply UI scaling based on selected size
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
    
    // Apply the scaling (font only initially to avoid UI issues)
    io.FontGlobalScale = dpi_scale * uiScale;
    // Note: We don't call ScaleAllSizes here to avoid UI element issues
    // The font scaling will handle most of the UI size adjustment
    
    // Main application state
    std::string currentDirectory;
    
    // Use config settings if available, otherwise use empty path
    if (!config.lastWorkingDirectory.empty() && std::filesystem::exists(config.lastWorkingDirectory)) {
        currentDirectory = config.lastWorkingDirectory;
    } else {
        currentDirectory = "";
    }
    
    std::vector<std::string> csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
    std::vector<InterferogramData> loadedData; // Store multiple loaded datasets
    std::vector<std::string> selectedFiles; // Store selected file paths
    std::vector<std::string> selectedFilenames; // Store selected filenames for legend
    bool dataLoaded = false;
    std::string currentDatasetName = "No dataset selected"; // Track current dataset name
    size_t currentSortedFileIndex = 0; // Track currently selected file index in sorted list
    bool filesChanged = true; // Flag to indicate files list changed
    bool keyboardNavigation = false; // Track if selection changed via keyboard
    bool multiSelectMode = false; // Track if Ctrl is held for multi-select
    bool shiftSelectMode = false; // Track if Shift is held for range selection
    const size_t MAX_SELECTABLE_FILES = 5; // Limit to 5 files for simultaneous display
    size_t lastSelectedIndex = 0; // Track last selected index for Shift+Click range selection
    bool alignPeaks = config.alignPeaks; // Use config setting for peak alignment
    bool autoRestoreScale = config.autoRestoreScale; // Use config setting for scale restoration
    
    // Keyboard shortcut state tracking
    bool yKeyPressedLastFrame = false;
    bool aKeyPressedLastFrame = false;
    bool dKeyPressedLastFrame = false;
    
    // Performance optimization: downsampling for large datasets
    bool enableDownsampling = true;
    const size_t maxPointsBeforeDownsampling = 50000; // Downsample if dataset exceeds this
    
    // Zoom state
    bool isZooming = false;
    ImVec2 zoomStart;
    ImVec2 zoomEnd;
    std::pair<size_t, size_t> zoomRange = {0, 0};
    bool isZoomed = false;
    bool shouldAutoscale = false; // Flag to trigger autoscale on new data load

    // X-range selection state
    bool isSelectingXRange = false;
    bool applyXRangeSelection = false;
    double selectionStartX = 0.0;
    double selectionEndX = 0.0;
    bool isMouseOverPlot = false;
    
    // Y-axis limits for plots
    float ref_y_min = 0.0f, ref_y_max = 1.0f;
    float prim_y_min = 0.0f, prim_y_max = 1.0f;
    bool autoFitYAxis = true; // Always default to enabled, don't use config
    
    // Track if we should update recent datasets (only after successful load)
    bool shouldUpdateRecentDatasets = false;
    
    // Track if this is the first data load after launch or directory switch
    bool isFirstDataLoad = true;
    
    // Sorted files list for display
    std::vector<std::string> sortedFiles;
    
    // Welcome screen state
    bool showWelcomeScreen = true;
    bool welcomeScreenInitialized = false;
    
    // No initialization needed for simple file dialog
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        multiSelectMode = ImGui::GetIO().KeyCtrl;
        shiftSelectMode = ImGui::GetIO().KeyShift;
        
        // Handle keyboard shortcuts - only trigger once per key press
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            bool yKeyPressed = glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool aKeyPressed = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool dKeyPressed = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            
            // 'Ctrl+Y' - Toggle auto-fit Y-axis (only on initial press)
            if (yKeyPressed && !yKeyPressedLastFrame) {
                autoFitYAxis = !autoFitYAxis;
                if (autoFitYAxis && dataLoaded) {
                    auto ref_min_max = std::minmax_element(loadedData[0].referenceDetector.begin(), loadedData[0].referenceDetector.end());
                    auto prim_min_max = std::minmax_element(loadedData[0].primaryDetector.begin(), loadedData[0].primaryDetector.end());
                    ref_y_min = *ref_min_max.first;
                    ref_y_max = *ref_min_max.second;
                    prim_y_min = *prim_min_max.first;
                    prim_y_max = *prim_min_max.second;
                }
            }
            
            // 'Ctrl+A' - Toggle align peaks (only on initial press)
            if (aKeyPressed && !aKeyPressedLastFrame) {
                alignPeaks = !alignPeaks;
            }
            
            // 'Ctrl+D' - Toggle downsampling (only on initial press)
            if (dKeyPressed && !dKeyPressedLastFrame) {
                enableDownsampling = !enableDownsampling;
                if (dataLoaded) {
                    // Reload all selected files with new downsampling setting while preserving selection
                    std::vector<InterferogramData> reloadedData;
                    for (const auto& filePath : selectedFiles) {
                        try {
                            InterferogramData data = CSVAdapter::loadFromCSV(filePath);
                            
                            // Apply downsampling if enabled and dataset is large
                            if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
                                
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
                        loadedData = reloadedData;
                        shouldAutoscale = autoRestoreScale; // Trigger plot update only if autorestore enabled
                        std::cout << "Reloaded " << loadedData.size() << " datasets with " 
                                  << (enableDownsampling ? "enabled" : "disabled") << " downsampling" << std::endl;
                    }
                }
            }
            
            // Update key state tracking for next frame
            yKeyPressedLastFrame = yKeyPressed;
            aKeyPressedLastFrame = aKeyPressed;
            dKeyPressedLastFrame = dKeyPressed;
        } else {
            // Reset key states when keyboard is captured (e.g., typing in text field)
            yKeyPressedLastFrame = false;
            aKeyPressedLastFrame = false;
        }
        
        // Reapply UI scaling if size changed
        handleUIScaling(io, uiScale, currentUiSize, uiSizeChanged);
        
        // Track window state changes
        handleWindowEvents(window, config);
        
        // Update sorted files list for keyboard navigation
        sortedFiles = csvFiles;
        std::sort(sortedFiles.begin(), sortedFiles.end(), [](const std::string& a, const std::string& b) {
            std::string nameA = a;
            std::string nameB = b;
            size_t last_slash_a = nameA.find_last_of("/\\");
            size_t last_slash_b = nameB.find_last_of("/\\");
            if (last_slash_a != std::string::npos) nameA = nameA.substr(last_slash_a + 1);
            if (last_slash_b != std::string::npos) nameB = nameB.substr(last_slash_b + 1);
            return naturalSortCompare(nameA, nameB);
        });
        
        // Handle keyboard navigation for file selection
        handleKeyboardNavigation(csvFiles, currentSortedFileIndex, filesChanged, keyboardNavigation, 
                                shiftSelectMode, selectedFiles, selectedFilenames, loadedData, dataLoaded, 
                                sortedFiles, enableDownsampling, maxPointsBeforeDownsampling, MAX_SELECTABLE_FILES);
        
        // Handle ESC key to reset zoom (works independently of file navigation)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && dataLoaded) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                // Reset X-axis zoom when ESC is pressed
                isZoomed = false;
                zoomRange = {0, 0};
                shouldAutoscale = true; // Always force redraw with full range when ESC is pressed
            }
        }
        

        
        // Handle Ctrl+H to return to welcome screen
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && ImGui::IsKeyPressed(ImGuiKey_H) && ImGui::GetIO().KeyCtrl) {
            // Reset to welcome screen state
            showWelcomeScreen = true;
            welcomeScreenInitialized = false;
            dataLoaded = false;
            filesChanged = false;
        }
        
        // Load file if navigation changed
        if (filesChanged && !csvFiles.empty()) {
            try {
                // Load the currently selected file
                InterferogramData data = CSVAdapter::loadFromCSV(sortedFiles[currentSortedFileIndex]);
                

                
                // Apply downsampling if enabled and dataset is large
                if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                    size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
                    
                    // Downsample both reference and primary detectors
                    std::vector<float> downsampledRef, downsampledPrim;
                    for (size_t i = 0; i < data.referenceDetector.size(); i += localDownsampleFactor) {
                        downsampledRef.push_back(data.referenceDetector[i]);
                        downsampledPrim.push_back(data.primaryDetector[i]);
                    }
                    data.referenceDetector = downsampledRef;
                    data.primaryDetector = downsampledPrim;
                    std::cout << "Downsampled dataset from " << (downsampledRef.size() * localDownsampleFactor) 
                              << " to " << downsampledRef.size() << " points (factor: " << localDownsampleFactor << ")" << std::endl;
                }
                
                // For single selection (no Ctrl), replace current selection
                loadedData.clear();
                selectedFiles.clear();
                selectedFilenames.clear();
                
                // Always load the current file
                loadedData.push_back(data);

                selectedFiles.push_back(sortedFiles[currentSortedFileIndex]);
                
                // Extract filename for legend
                std::string filename = sortedFiles[currentSortedFileIndex];
                size_t last_slash = filename.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    filename = filename.substr(last_slash + 1);
                }
                selectedFilenames.push_back(filename);
                
                // Update current dataset name (extract from current directory path)
                std::string dirPath = currentDirectory;
                size_t dir_last_slash = dirPath.find_last_of("/\\");
                if (dir_last_slash != std::string::npos) {
                    currentDatasetName = dirPath.substr(dir_last_slash + 1);
                    // If this is "raw_data", get the parent directory name
                    if (currentDatasetName == "raw_data" && dir_last_slash > 0) {
                        size_t parent_slash = dirPath.substr(0, dir_last_slash).find_last_of("/\\");
                        if (parent_slash != std::string::npos) {
                            currentDatasetName = dirPath.substr(parent_slash + 1, dir_last_slash - parent_slash - 1);
                        }
                    }
                }
                
                dataLoaded = true;
                
                // Handle autoscale behavior based on AGENTS.md requirements:
                // "when the application loads a file for display for the first time after launch or work directory switch, axes zoom to fit all data."
                if (isFirstDataLoad || autoRestoreScale) {
                    isZoomed = false;
                    zoomRange = {0, 0};
                    shouldAutoscale = true; // Trigger autoscale
                    
                    // Recalculate Y-axis limits from the actual data for autoscale
                    auto ref_min_max = std::minmax_element(data.referenceDetector.begin(), data.referenceDetector.end());
                    auto prim_min_max = std::minmax_element(data.primaryDetector.begin(), data.primaryDetector.end());
                    ref_y_min = *ref_min_max.first;
                    ref_y_max = *ref_min_max.second;
                    prim_y_min = *prim_min_max.first;
                    prim_y_max = *prim_min_max.second;
                    
                    // Reset first load flag after handling
                    isFirstDataLoad = false;
                }
                filesChanged = false;
                
                // Mark that we should update recent datasets after this successful load
                shouldUpdateRecentDatasets = true;
                
            } catch (const std::exception& e) {
                std::cerr << "Error loading file: " << e.what() << std::endl;
                dataLoaded = false;
                filesChanged = false;
                // Don't update recent datasets on failure
                shouldUpdateRecentDatasets = false;
            }
        }
        
        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Show welcome screen if no data is loaded and we haven't initialized yet
        if (showWelcomeScreen && !welcomeScreenInitialized) {
            // Safety check to prevent multiple welcome screen instances
            static bool welcomeScreenActive = false;
            if (welcomeScreenActive) {
                std::cerr << "Warning: Attempted to show welcome screen while already active!" << std::endl;
                showWelcomeScreen = false; // Prevent re-entry by disabling welcome screen
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
                                        currentDirectory = rawDataPath; // Use the raw_data subdirectory
                                    } else {
                                        currentDirectory = datasetPath; // Fallback to the dataset directory itself
                                    }
                                    
                                    // Update current dataset name
                                    currentDatasetName = datasetPath.substr(datasetPath.find_last_of("/\\") + 1);
                                    
                                    csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
                                    dataLoaded = false;
                                    filesChanged = true;
                                    currentSortedFileIndex = 0;
                                    showWelcomeScreen = false;
                                    welcomeScreenInitialized = true;
                                    isFirstDataLoad = true; // Reset first load flag for new directory
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
                if (ImGui::BeginCombo("##UISizeCombo", currentUiSize.c_str())) {
                    if (ImGui::Selectable("tiny", currentUiSize == "tiny")) {
                        currentUiSize = "tiny";
                        uiSizeChanged = true;
                    }
                    if (ImGui::Selectable("small", currentUiSize == "small")) {
                        currentUiSize = "small";
                        uiSizeChanged = true;
                    }
                    if (ImGui::Selectable("normal", currentUiSize == "normal")) {
                        currentUiSize = "normal";
                        uiSizeChanged = true;
                    }
                    if (ImGui::Selectable("large", currentUiSize == "large")) {
                        currentUiSize = "large";
                        uiSizeChanged = true;
                    }
                    if (ImGui::Selectable("huge", currentUiSize == "huge")) {
                        currentUiSize = "huge";
                        uiSizeChanged = true;
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
                    std::string selectedDirectory = FileBrowser::showDirectorySelectionDialog();
                    if (!selectedDirectory.empty()) {
                        // Check if the selected directory has a raw_data subdirectory
                        std::string rawDataPath = selectedDirectory + "/raw_data";
                        if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                            currentDirectory = rawDataPath;
                            // Update dataset name from parent directory
                            currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                        } else {
                            currentDirectory = selectedDirectory;
                            // Update dataset name from selected directory
                            currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                        }
                        csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
                        dataLoaded = false;
                        filesChanged = true;
                        currentSortedFileIndex = 0;
                        showWelcomeScreen = false;
                        welcomeScreenInitialized = true;
                        std::cout << "Working directory set to: " << currentDirectory << std::endl;
                        ImGui::CloseCurrentPopup(); // Close the modal
                    }
                }
                // Clean up welcome screen styles
                ImGui::PopStyleColor(); // PopupBg only (removed other styles to simplify)

                ImGui::EndPopup();
                
                // If welcome screen is closed manually, initialize the app
                if (!showWelcomeScreen) {
                    welcomeScreenInitialized = true;
                }
                welcomeScreenActive = false; // Reset the safety flag when welcome screen closes
            } else {
                // Popup wasn't opened, no styles to clean up (they're pushed inside BeginPopupModal)
                welcomeScreenActive = false; // Reset the safety flag if popup wasn't opened
            }
        }
        
        // Only render main docking interface if welcome screen is not active
        if (welcomeScreenInitialized) {
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
                                currentDirectory = rawDataPath; // Use the raw_data subdirectory
                                // Update dataset name from parent directory
                                currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                            } else {
                                currentDirectory = selectedDirectory; // Fallback to selected directory
                                // Update dataset name from selected directory
                                currentDatasetName = selectedDirectory.substr(selectedDirectory.find_last_of("/\\") + 1);
                            }
                            csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
                            dataLoaded = false;
                            shouldUpdateRecentDatasets = true; // Mark to update recent datasets
                            isFirstDataLoad = true; // Reset first load flag for new directory
                            std::cout << "Working directory set to: " << currentDirectory << std::endl;
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
                                            currentDirectory = rawDataPath; // Use the raw_data subdirectory
                                        } else {
                                            currentDirectory = datasetPath; // Fallback to the dataset directory itself
                                        }
                                        
                                        // Update current dataset name
                                        currentDatasetName = datasetPath.substr(datasetPath.find_last_of("/\\") + 1);
                                        
                                        csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
                                        dataLoaded = false;
                                        filesChanged = true;
                                        currentSortedFileIndex = 0;
                                        isFirstDataLoad = true; // Reset first load flag for new directory
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
                    ImGui::MenuItem("Align peaks", NULL, &alignPeaks);
                    if (ImGui::MenuItem("Autorestore scale", NULL, &autoRestoreScale)) {
                        // When enabling autorestore scale, trigger autoscale to fit all data
                        if (autoRestoreScale && dataLoaded) {
                            shouldAutoscale = true;
                        }
                    }
                    
                    // Auto-fit Y-axis toggle
                    if (ImGui::MenuItem("Auto-fit Y-axis", NULL, &autoFitYAxis)) {
                        // When toggling auto-fit, recalculate limits if enabling auto-fit
                        if (autoFitYAxis && dataLoaded) {
                            auto ref_min_max = std::minmax_element(loadedData[0].referenceDetector.begin(), loadedData[0].referenceDetector.end());
                            auto prim_min_max = std::minmax_element(loadedData[0].primaryDetector.begin(), loadedData[0].primaryDetector.end());
                            ref_y_min = *ref_min_max.first;
                            ref_y_max = *ref_min_max.second;
                            prim_y_min = *prim_min_max.first;
                            prim_y_max = *prim_min_max.second;
                        }
                    }
                    
                    // Downsampling toggle
                    if (ImGui::MenuItem("Enable downsampling", NULL, &enableDownsampling)) {
                        if (dataLoaded) {
                            // Reload all selected files with new downsampling setting while preserving selection
                            std::vector<InterferogramData> reloadedData;
                            for (const auto& filePath : selectedFiles) {
                                try {
                                    InterferogramData data = CSVAdapter::loadFromCSV(filePath);
                                    
                                    // Apply downsampling if enabled and dataset is large
                                    if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                                        size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
                                        
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
                                loadedData = reloadedData;
                                shouldAutoscale = autoRestoreScale; // Trigger plot update only if autorestore enabled
                                std::cout << "Reloaded " << loadedData.size() << " datasets with " 
                                          << (enableDownsampling ? "enabled" : "disabled") << " downsampling" << std::endl;
                            }
                        }
                    }
                    
                    // UI Size selection dropdown
                    if (ImGui::BeginMenu("UI Size"))
                    {
                        if (ImGui::MenuItem("tiny", NULL, currentUiSize == "tiny")) {
                            currentUiSize = "tiny";
                            uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("small", NULL, currentUiSize == "small")) {
                            currentUiSize = "small";
                            uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("normal", NULL, currentUiSize == "normal")) {
                            currentUiSize = "normal";
                            uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("large", NULL, currentUiSize == "large")) {
                            currentUiSize = "large";
                            uiSizeChanged = true;
                        }
                        if (ImGui::MenuItem("huge", NULL, currentUiSize == "huge")) {
                            currentUiSize = "huge";
                            uiSizeChanged = true;
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
            if (showWelcomeScreen && !welcomeScreenInitialized) {
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
            
            if (showWelcomeScreen && !welcomeScreenInitialized) {
                ImGui::PopStyleColor(); // Restore window background color
            }
            
            ImGui::End();
        
        // Files panel (left)
        ImGui::Begin("Files");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        ImGui::Text("Current Dataset: %s", currentDatasetName.c_str());
        
        // Use the pre-sorted files list
        size_t currentSortedIndex = currentSortedFileIndex;
        
        // Only calculate scroll position when using keyboard navigation
        if (keyboardNavigation) {
            if (currentSortedIndex > 0 && ImGui::GetScrollY() + ImGui::GetWindowHeight() < (currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing()) {
                ImGui::SetScrollY((currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight());
            } else if (currentSortedIndex == 0) {
                ImGui::SetScrollY(0);
            }
        }
        
        for (size_t i = 0; i < sortedFiles.size(); i++) {
            const auto& file = sortedFiles[i];
            // Extract just the filename without path
            std::string filename = file;
            size_t last_slash = filename.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                filename = filename.substr(last_slash + 1);
            }
            
            // Enhanced highlighting for the currently selected file
            int stylesPushed = 1; // Default: push 1 style
            bool isFileSelected = (std::find(selectedFiles.begin(), selectedFiles.end(), file) != selectedFiles.end());
            
            if (isFileSelected) {
                // Find the index of this file in the selectedFiles vector to determine its color
                auto it = std::find(selectedFiles.begin(), selectedFiles.end(), file);
                size_t fileIndex = std::distance(selectedFiles.begin(), it);
                
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
                if (multiSelectMode) {
                    // Toggle selection for this file
                    std::string fullPath = sortedFiles[i];
                    auto it = std::find(selectedFiles.begin(), selectedFiles.end(), fullPath);
                    if (it != selectedFiles.end()) {
                        // File already selected, remove it
                        size_t index = std::distance(selectedFiles.begin(), it);
                        selectedFiles.erase(selectedFiles.begin() + index);
                        selectedFilenames.erase(selectedFilenames.begin() + index);
                        loadedData.erase(loadedData.begin() + index);
                    } else {
                        // Check if we would exceed the limit
                        if (selectedFiles.size() < MAX_SELECTABLE_FILES) {
                            try {
                                InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
        
                                
                                // Apply downsampling to multi-selected files too
                                if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
                                    
                                    // Downsample both reference and primary detectors
                                    std::vector<float> downsampledRef, downsampledPrim;
                                    for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                        downsampledRef.push_back(data.referenceDetector[j]);
                                        downsampledPrim.push_back(data.primaryDetector[j]);
                                    }
                                    data.referenceDetector = downsampledRef;
                                    data.primaryDetector = downsampledPrim;
                                }
                                
                                loadedData.push_back(data);
                                selectedFiles.push_back(fullPath);
                                selectedFilenames.push_back(filename);
                            } catch (const std::exception& e) {
                                std::cerr << "Error loading file: " << e.what() << std::endl;
                            }
                        } else {
                            ImGui::OpenPopup("Selection Limit");
                        }
                    }
                } else if (shiftSelectMode) {
                    // Handle Shift+Click for range selection
                    size_t startIndex = std::min(lastSelectedIndex, i);
                    size_t endIndex = std::max(lastSelectedIndex, i);
                    
                    // Clear current selection
                    selectedFiles.clear();
                    selectedFilenames.clear();
                    loadedData.clear();
                    
                    // Add all files in the range, respecting the 5-file limit
                    size_t filesToAdd = 0;
                    for (size_t j = startIndex; j <= endIndex; j++) {
                        if (filesToAdd >= MAX_SELECTABLE_FILES) break;
                        
                        try {
                            std::string fullPath = sortedFiles[j];
                            InterferogramData data = CSVAdapter::loadFromCSV(fullPath);
                            
                            // Apply downsampling
                            if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / maxPointsBeforeDownsampling + 1;
                                std::vector<float> downsampledRef, downsampledPrim;
                                for (size_t k = 0; k < data.referenceDetector.size(); k += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[k]);
                                    downsampledPrim.push_back(data.primaryDetector[k]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            
                            loadedData.push_back(data);
                            selectedFiles.push_back(fullPath);
                            
                            // Extract filename for legend
                            std::string filename = sortedFiles[j];
                            size_t last_slash = filename.find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                filename = filename.substr(last_slash + 1);
                            }
                            selectedFilenames.push_back(filename);
                            filesToAdd++;
                        } catch (const std::exception& e) {
                            std::cerr << "Error loading file: " << e.what() << std::endl;
                        }
                    }
                    
                    // Update last selected index
                    lastSelectedIndex = i;
                    dataLoaded = !selectedFiles.empty();
                } else {
                    // Single selection - replace current selection
                    currentSortedFileIndex = i;
                    filesChanged = true;
                    // Update last selected index for future Shift+Click
                    lastSelectedIndex = i;
                }
            }
            
            // Pop the correct number of styles
            ImGui::PopStyleColor(stylesPushed);
            
            // Auto-scroll to keep selected item visible only when selection changes via keyboard
            if (i == currentSortedFileIndex && keyboardNavigation) {
                ImGui::SetScrollHereY(0.5f); // Scroll to center the selected item
            }
        }
        // Show selection limit popup if needed
        if (ImGui::BeginPopupModal("Selection Limit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Maximum of %zu files can be selected at once.", MAX_SELECTABLE_FILES);
            ImGui::Text("Please deselect some files first.");
            
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }
        
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();
        
        // Graphing panel (main) - now using ImPlot
        ImGui::Begin("Graphing Panel");
        if (dataLoaded) {
            // Y-axis limits are now handled by the auto-fit toggle
            // When autoFitYAxis is true, ImPlot will auto-calculate Y-axis limits
            // When autoFitYAxis is false, we use the manually calculated limits
            
            // Handle mouse scroll zoom
            float scroll = ImGui::GetIO().MouseWheel;
            if (scroll != 0.0f && ImGui::IsWindowHovered()) {
                // Get mouse position relative to plot area
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 plotMin = ImGui::GetItemRectMin();
                ImVec2 plotMax = ImGui::GetItemRectMax();
                
                if (mousePos.x >= plotMin.x && mousePos.x <= plotMax.x && 
                    mousePos.y >= plotMin.y && mousePos.y <= plotMax.y) {
                    
                    // Calculate zoom factor based on scroll direction
                    float zoomFactor = scroll > 0 ? 0.8f : 1.25f; // Scroll up = zoom in, scroll down = zoom out
                    
                    // Get current visible range
                    size_t current_start = isZoomed ? zoomRange.first : 0;
                    size_t current_end = isZoomed ? zoomRange.second : loadedData[0].referenceDetector.size();
                    size_t current_width = current_end - current_start;
                    
                    // Calculate new zoom range centered on mouse position
                    size_t new_width = static_cast<size_t>(current_width * zoomFactor);
                    size_t mouse_data_pos = static_cast<size_t>(((mousePos.x - plotMin.x) / (plotMax.x - plotMin.x)) * loadedData[0].referenceDetector.size());
                    
                    // Apply zoom with constraints
                    size_t new_start = mouse_data_pos - new_width / 2;
                    size_t new_end = mouse_data_pos + new_width / 2;
                    
                    // Clamp to valid range
                    size_t data_size = loadedData[0].referenceDetector.size();
                    new_start = std::max(size_t(0), new_start);
                    new_end = std::min(data_size, new_end);
                    new_end = std::max(new_start + 1, new_end);
                    
                    // Apply the zoom
                    zoomRange.first = new_start;
                    zoomRange.second = new_end;
                    isZoomed = true;
                    shouldAutoscale = false;
                    
                    std::cout << "Scroll zoom: " << zoomRange.first << "-" << zoomRange.second 
                              << " (center: " << mouse_data_pos << ", width: " << new_width << ")" << std::endl;
                }
            }
            

            
            // Determine zoom range
            size_t ref_start = isZoomed ? zoomRange.first : 0;
            size_t ref_end = isZoomed ? zoomRange.second : loadedData[0].referenceDetector.size();
            size_t prim_start = isZoomed ? zoomRange.first : 0;
            size_t prim_end = isZoomed ? zoomRange.second : loadedData[0].primaryDetector.size();
            // Apply peak alignment if enabled
            std::vector<InterferogramData> alignedData = loadedData;
            if (alignPeaks && loadedData.size() > 1) {
                // Find the peak position in the first dataset's primary detector (reference)
                size_t referencePeakPos = 0;
                float referencePeakValue = loadedData[0].primaryDetector[0];
                for (size_t i = 1; i < loadedData[0].primaryDetector.size(); i++) {
                    if (loadedData[0].primaryDetector[i] > referencePeakValue) {
                        referencePeakValue = loadedData[0].primaryDetector[i];
                        referencePeakPos = i;
                    }
                }
                
                // Align all other datasets to this peak position
                for (size_t datasetIdx = 1; datasetIdx < loadedData.size(); datasetIdx++) {
                    // Find peak in current dataset's primary detector
                    size_t currentPeakPos = 0;
                    float currentPeakValue = loadedData[datasetIdx].primaryDetector[0];
                    for (size_t i = 1; i < loadedData[datasetIdx].primaryDetector.size(); i++) {
                        if (loadedData[datasetIdx].primaryDetector[i] > currentPeakValue) {
                            currentPeakValue = loadedData[datasetIdx].primaryDetector[i];
                            currentPeakPos = i;
                        }
                    }
                    
                    // Calculate shift needed to align peaks
                    int shift = referencePeakPos - currentPeakPos;
                    
                    // Apply shift to both reference and primary detectors
                    std::vector<float> shiftedRef(loadedData[datasetIdx].referenceDetector.size(), 0.0f);
                    std::vector<float> shiftedPrim(loadedData[datasetIdx].primaryDetector.size(), 0.0f);
                    
                    if (shift > 0) {
                        // Shift right - pad beginning with zeros
                        for (size_t i = 0; i < loadedData[datasetIdx].referenceDetector.size() - shift; i++) {
                            shiftedRef[i + shift] = loadedData[datasetIdx].referenceDetector[i];
                            shiftedPrim[i + shift] = loadedData[datasetIdx].primaryDetector[i];
                        }
                    } else if (shift < 0) {
                        // Shift left - pad end with zeros
                        shift = -shift; // Make positive
                        for (size_t i = 0; i < loadedData[datasetIdx].referenceDetector.size() - shift; i++) {
                            shiftedRef[i] = loadedData[datasetIdx].referenceDetector[i + shift];
                            shiftedPrim[i] = loadedData[datasetIdx].primaryDetector[i + shift];
                        }
                    } else {
                        // No shift needed
                        shiftedRef = loadedData[datasetIdx].referenceDetector;
                        shiftedPrim = loadedData[datasetIdx].primaryDetector;
                    }
                    
                    // Update aligned data
                    alignedData[datasetIdx].referenceDetector = shiftedRef;
                    alignedData[datasetIdx].primaryDetector = shiftedPrim;
                }
            }
            
            if (loadedData.size() > 1) {
                ImGui::Text("Datasets:");
                ImGui::BeginGroup(); // Start horizontal group
                for (size_t i = 0; i < loadedData.size(); i++) {
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
                    ImGui::Text("%s", selectedFilenames[i].c_str());
                    if (i < loadedData.size() - 1) {
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
            if (dataLoaded) {
                plotSpecs.resize(loadedData.size());
                for (size_t i = 0; i < loadedData.size(); i++) {
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
            isMouseOverPlot = isOverPlot;

            // Handle X-range selection with Shift key - state management only
            bool shiftPressed = ImGui::GetIO().KeyShift;
            if (isOverPlot && shiftPressed && !isSelectingXRange) {
                // Start selection when Shift is pressed over plot
                isSelectingXRange = true;
                // Reset selection positions
                selectionStartX = 0.0;
                selectionEndX = 0.0;
                std::cout << "DEBUG: Started X-range selection" << std::endl;
            } else if (!shiftPressed && isSelectingXRange) {
                // End selection when Shift is released
                isSelectingXRange = false;
                
                // Only finalize if we have valid selection
                if(selectionStartX != selectionEndX) {
                    applyXRangeSelection = true;
                    
                    if(selectionStartX > selectionEndX)
                    {
                        // make sure start is always smaller
                        double dum = selectionStartX;
                        selectionStartX = selectionEndX;
                        selectionEndX = dum;
                    }
                    
                    std::cout << "DEBUG: Finalizing X-range selection: Start=" << selectionStartX << ", End=" << selectionEndX << std::endl;
                } else {
                    std::cout << "DEBUG: X-range selection cancelled (no valid range)" << std::endl;
                }
            }

            
            if (ImPlot::BeginSubplots("Detector Plots", 2, 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend, row_ratios)) {
                
                // Reference detector plot (top)
                if (ImPlot::BeginPlot("Reference", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_Crosshairs)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }
                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels, y_flags);
                    // Set lighter gray grid color
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));

                    if (isZoomed) {
                        // Only zoom X-axis (Y-axis is handled by auto-fit flag)
                        if (!autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(ref_start, ref_end, ref_y_min, ref_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit Y-axis: only set X-axis limits for zoom
                            ImPlot::SetupAxisLimits(ImAxis_X1, ref_start, ref_end, ImPlotCond_Always);
                        }
                    } else if (shouldAutoscale) {
                        // Set initial view to show all data when new data is loaded
                        if (!autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(0, loadedData[0].referenceDetector.size(), ref_y_min, ref_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit both axes: set X-axis to full range, let Y-axis auto-fit
                            ImPlot::SetupAxisLimits(ImAxis_X1, 0, loadedData[0].referenceDetector.size(), ImPlotCond_Always);
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (applyXRangeSelection && selectionStartX != selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, selectionStartX, selectionEndX, ImPlotCond_Always);
                        applyXRangeSelection = false; // Reset flag after applying
                    }
                    // Plot all selected datasets with pre-allocated specs
                    if (dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < loadedData[0].referenceDetector.size()) {
                            for (size_t i = 0; i < loadedData.size(); i++) {
                                const auto& dataToPlot = alignPeaks ? alignedData[i] : loadedData[i];
                                if (ref_start < dataToPlot.referenceDetector.size()) {
                                    size_t actual_count = std::min(data_count, dataToPlot.referenceDetector.size() - ref_start);
                                    ImPlot::PlotLine("", 
                                                   &dataToPlot.referenceDetector[ref_start], 
                                                   actual_count, 1.0, 0.0, plotSpecs[i]);
                                }
                            }
                        } else {
                            std::cout << "DEBUG: Invalid data range for plotting: start=" << ref_start << ", end=" << ref_end << ", size=" << loadedData[0].referenceDetector.size() << std::endl;
                        }
                    }
                    
                    // Handle X-range selection within plot context
                    if (isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (selectionStartX == 0.0 && selectionEndX == 0.0) {
                            selectionStartX = mousePos.x;
                        }
                        selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(selectionStartX, selectionEndX);
                        double selection_right = std::max(selectionStartX, selectionEndX);
                        
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
                        double start_x[2] = {selectionStartX, selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {selectionEndX, selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    ImPlot::EndPlot();
                    ImPlot::PopStyleColor(); // Pop grid color
                }
                
                // Primary detector plot (bottom)
                if (ImPlot::BeginPlot("Primary", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_Crosshairs)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }
                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks, y_flags);
                    // Set lighter gray grid color
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));

                    if (isZoomed) {
                        // Only zoom X-axis (Y-axis is handled by auto-fit flag)
                        if (!autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(ref_start, ref_end, prim_y_min, prim_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit Y-axis: only set X-axis limits for zoom
                            ImPlot::SetupAxisLimits(ImAxis_X1, ref_start, ref_end, ImPlotCond_Always);
                        }
                    } else if (shouldAutoscale) {
                        // Set initial view to show all data when new data is loaded
                        if (!autoFitYAxis) {
                            // Manual Y-axis: set both X and Y limits
                            ImPlot::SetupAxesLimits(0, loadedData[0].primaryDetector.size(), prim_y_min, prim_y_max, ImPlotCond_Always);
                        } else {
                            // Auto-fit both axes: set X-axis to full range, let Y-axis auto-fit
                            ImPlot::SetupAxisLimits(ImAxis_X1, 0, loadedData[0].primaryDetector.size(), ImPlotCond_Always);
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (applyXRangeSelection && selectionStartX != selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, selectionStartX, selectionEndX, ImPlotCond_Always);
                        applyXRangeSelection = false; // Reset flag after applying
                    }
                    // Reuse the same plot specs as reference plot (already pre-allocated)
                    // Plot all selected datasets with same colors as reference
                    if (dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < loadedData[0].primaryDetector.size()) {
                            for (size_t i = 0; i < loadedData.size(); i++) {
                                const auto& dataToPlot = alignPeaks ? alignedData[i] : loadedData[i];
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
                    if (isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (selectionStartX == 0.0 && selectionEndX == 0.0) {
                            selectionStartX = mousePos.x;
                        }
                        selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(selectionStartX, selectionEndX);
                        double selection_right = std::max(selectionStartX, selectionEndX);
                        
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
                        double start_x[2] = {selectionStartX, selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {selectionEndX, selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    ImPlot::EndPlot();
                    ImPlot::PopStyleColor(); // Pop grid color
                }
                
                // Reset autoscale flag after use
                if (shouldAutoscale) {
                    shouldAutoscale = false;
                }
                
                ImPlot::EndSubplots();
            }
            
            
        } else {
            ImGui::Text("No data loaded. Select a CSV file from the Files panel.");
        }
        ImGui::End();
        
        // Metadata panel (right)
        ImGui::Begin("Metadata");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        if (dataLoaded) {
            ImGui::Text("File: %s", csvFiles.empty() ? "None" : csvFiles[0].c_str());
            ImGui::Text("Samples: %zu", loadedData[0].referenceDetector.size());
            
            // Display comments if comments.txt exists
            ImGui::Separator();
            ImGui::Text("Comments:");
            
            std::string commentsPath = currentDirectory;
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
        keyboardNavigation = false;
    }
    
    // Cleanup
    cleanupApplication(window);
    
    // Save configuration before exiting
    config.alignPeaks = alignPeaks;
    config.autoRestoreScale = autoRestoreScale;
    config.lastWorkingDirectory = currentDirectory;
    config.uiSize = currentUiSize;
    
    // Update recent datasets if we had a successful load
    if (shouldUpdateRecentDatasets && dataLoaded && !selectedFiles.empty()) {
        // Add the parent directory of the current dataset to recent datasets
        std::string datasetPath = selectedFiles[0];
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
    
    // Save config to file
    if (!config.saveToFile(configFilePath)) {
        std::cerr << "Failed to save configuration to " << configFilePath << std::endl;
    } else {
        std::cout << "Configuration saved to " << configFilePath << std::endl;
    }
    
    return 0;
}
