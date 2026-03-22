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
        FILE* pipe = popen("zenity --file-selection --directory --title='Select Dataset Directory'", "r");
        if (pipe) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                // Remove trailing newline
                buffer[strcspn(buffer, "\n")] = 0;
                result = buffer;
            }
            pclose(pipe);
            
            // If zenity returned a valid path, use it
            if (!result.empty() && result != "") {
                return result;
            }
        }
        
        // Fallback: use a simple ImGui-based directory selector
        // This is a basic implementation - a production app would use a proper file dialog library
        static char tempDirectoryBuffer[1024] = "/home"; // Default starting directory
        static bool showDialog = true;
        
        if (showDialog) {
            ImGui::OpenPopup("Select Directory");
            showDialog = false;
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
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Directory does not exist!");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }
        
        return result;
    }
};

int main() {
    std::cout << "FTS Data Explorer - Starting application..." << std::endl;
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }
    
    // Create window
    GLFWwindow* window = glfwCreateWindow(1280, 720, "FTS Data Explorer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
    ImGui::StyleColorsDark();
    
    // Customize plot colors
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 yellow_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Bright yellow
    ImVec4 background_color = style.Colors[ImGuiCol_WindowBg]; // Match window background
    
    // Set plot colors
    style.Colors[ImGuiCol_PlotLines] = yellow_color;
    style.Colors[ImGuiCol_PlotLinesHovered] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogram] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogramHovered] = yellow_color;
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    
    // Initialize ImPlot context
    ImPlot::CreateContext();
    
    // Set up for high DPI displays AFTER backend initialization
    // Apply 1.5x scaling to all UI elements including fonts
    float dpi_scale = io.DisplayFramebufferScale.x;
    io.FontGlobalScale = dpi_scale * 1.5f;
    ImGui::GetStyle().ScaleAllSizes(dpi_scale * 1.5f);
    
    // Main application state
    std::string currentDirectory = "/home/guowa/Documents/Repos/fts_data_explorer/example_datasets/2025-06-12_15-17-10_reference_3mm_2.0mms_30avg/raw_data";
    std::vector<std::string> csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
    std::vector<InterferogramData> loadedData; // Store multiple loaded datasets
    std::vector<std::string> selectedFiles; // Store selected file paths
    std::vector<std::string> selectedFilenames; // Store selected filenames for legend
    bool dataLoaded = false;
    std::string currentDatasetName = "No dataset selected"; // Track current dataset name
    size_t currentSortedFileIndex = 0; // Track currently selected file index in sorted list
    bool filesChanged = true; // Flag to indicate files list changed
    bool multiSelectMode = false; // Track if Ctrl is held for multi-select
    const size_t MAX_SELECTABLE_FILES = 5; // Limit to 5 files for simultaneous display
    bool alignPeaks = false; // Flag to enable peak alignment
    
    // Zoom state
    bool isZooming = false;
    ImVec2 zoomStart;
    ImVec2 zoomEnd;
    std::pair<size_t, size_t> zoomRange = {0, 0};
    bool isZoomed = false;
    bool shouldAutoscale = false; // Flag to trigger autoscale on new data load
    
    // No initialization needed for simple file dialog
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        multiSelectMode = ImGui::GetIO().KeyCtrl;
        
        // Handle keyboard navigation for file selection
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && !csvFiles.empty() && dataLoaded) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                // Navigate up in file list (with wrapping)
                if (currentSortedFileIndex > 0) {
                    currentSortedFileIndex--;
                } else {
                    currentSortedFileIndex = csvFiles.size() - 1; // Wrap to bottom
                }
                filesChanged = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                // Navigate down in file list (with wrapping)
                if (currentSortedFileIndex < csvFiles.size() - 1) {
                    currentSortedFileIndex++;
                } else {
                    currentSortedFileIndex = 0; // Wrap to top
                }
                filesChanged = true;
            }
        }
        
        // Handle ESC key to reset zoom (works independently of file navigation)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && dataLoaded) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                // Reset X-axis zoom when ESC is pressed
                isZoomed = false;
                zoomRange = {0, 0};
                shouldAutoscale = true; // Force redraw with full range
            }
        }
        
        // Load file if navigation changed
        if (filesChanged && !csvFiles.empty()) {
            try {
                // Sort files to get the correct file for the current index
                std::vector<std::string> sortedFiles = csvFiles;
                std::sort(sortedFiles.begin(), sortedFiles.end(), [](const std::string& a, const std::string& b) {
                    std::string nameA = a;
                    std::string nameB = b;
                    size_t last_slash_a = nameA.find_last_of("/\\");
                    size_t last_slash_b = nameB.find_last_of("/\\");
                    if (last_slash_a != std::string::npos) nameA = nameA.substr(last_slash_a + 1);
                    if (last_slash_b != std::string::npos) nameB = nameB.substr(last_slash_b + 1);
                    return naturalSortCompare(nameA, nameB);
                });
                
                // Load the currently selected file
                InterferogramData data = CSVAdapter::loadFromCSV(sortedFiles[currentSortedFileIndex]);
                
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
                
                dataLoaded = true;
                // Reset zoom when loading new file
                isZoomed = false;
                zoomRange = {0, 0};
                shouldAutoscale = true; // Trigger autoscale for new data
                filesChanged = false;
            } catch (const std::exception& e) {
                std::cerr << "Error loading file: " << e.what() << std::endl;
                dataLoaded = false;
                filesChanged = false;
            }
        }
        
        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Set up docking
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        
        // Push style variables for full viewport docking
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        
        // Create main dockspace window
        ImGui::Begin("DockSpace", nullptr, window_flags);
        ImGui::PopStyleVar(2);
        
        // Create docking space
        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
        
        // Files panel (left)
        ImGui::Begin("Files");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        ImGui::Text("Current Dataset: %s", currentDatasetName.c_str());
        
        // Sort files naturally (handles numbers properly)
        std::vector<std::string> sortedFiles = csvFiles;
        std::sort(sortedFiles.begin(), sortedFiles.end(), [](const std::string& a, const std::string& b) {
            // Extract just the filename without path for comparison
            std::string nameA = a;
            std::string nameB = b;
            size_t last_slash_a = nameA.find_last_of("/\\");
            size_t last_slash_b = nameB.find_last_of("/\\");
            if (last_slash_a != std::string::npos) nameA = nameA.substr(last_slash_a + 1);
            if (last_slash_b != std::string::npos) nameB = nameB.substr(last_slash_b + 1);
            
            return naturalSortCompare(nameA, nameB);
        });
        
        // Use the directly tracked sorted index
        size_t currentSortedIndex = currentSortedFileIndex;
        
        // Calculate scroll position to keep selected file visible
        if (currentSortedIndex > 0 && ImGui::GetScrollY() + ImGui::GetWindowHeight() < (currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing()) {
            ImGui::SetScrollY((currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight());
        } else if (currentSortedIndex == 0) {
            ImGui::SetScrollY(0);
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
                } else {
                    // Single selection - replace current selection
                    currentSortedFileIndex = i;
                    filesChanged = true;
                }
            }
            
            // Pop the correct number of styles
            ImGui::PopStyleColor(stylesPushed);
            
            // Auto-scroll to keep selected item visible
            if (i == currentSortedFileIndex) {
                ImGui::SetScrollHereY(0.5f); // Scroll to center the selected item
            }
        }
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();
        
        // Graphing panel (main) - now using ImPlot
        ImGui::Begin("Graphing Panel");
        if (dataLoaded) {
            // Show info for multi-select mode
            if (selectedFiles.size() > 1) {
                ImGui::Text("Displaying %zu datasets", selectedFiles.size());
            } else {
                ImGui::Text("Reference Detector: %zu samples", loadedData[0].referenceDetector.size());
                ImGui::Text("Primary Detector: %zu samples", loadedData[0].primaryDetector.size());
            }
            
            // Add zoom controls
            if (isZoomed) {
                if (ImGui::Button("Reset Zoom")) {
                    isZoomed = false;
                    zoomRange = {0, 0};
                }
                ImGui::SameLine();
                ImGui::Text("Zoomed: %zu-%zu", zoomRange.first, zoomRange.second);
            }
            
            // Calculate min/max values for Y-axis for each dataset
            auto ref_min_max = std::minmax_element(loadedData[0].referenceDetector.begin(), loadedData[0].referenceDetector.end());
            auto prim_min_max = std::minmax_element(loadedData[0].primaryDetector.begin(), loadedData[0].primaryDetector.end());
            float ref_y_min = *ref_min_max.first;
            float ref_y_max = *ref_min_max.second;
            float prim_y_min = *prim_min_max.first;
            float prim_y_max = *prim_min_max.second;
            
            // Add 2% margin to Y axis limits symmetrically
            float ref_y_range = ref_y_max - ref_y_min;
            float prim_y_range = prim_y_max - prim_y_min;
            float ref_margin = ref_y_range * 0.02f;
            float prim_margin = prim_y_range * 0.02f;
            ref_y_min -= ref_margin;
            ref_y_max += ref_margin;
            prim_y_min -= prim_margin;
            prim_y_max += prim_margin;
            
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
            
            // Create ImPlot subplots - two vertically stacked plots
            if (ImPlot::BeginSubplots("Detector Plots", 2, 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend)) {
                
                // Reference detector plot (top)
                if (ImPlot::BeginPlot("Reference", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_Crosshairs)) {
                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
                    // Set lighter gray grid color
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                    if (isZoomed) {
                        ImPlot::SetupAxesLimits(ref_start, ref_end, ref_y_min, ref_y_max, ImPlotCond_Always);
                    } else if (shouldAutoscale) {
                        // Set initial view to show all data when new data is loaded
                        ImPlot::SetupAxesLimits(0, loadedData[0].referenceDetector.size(), ref_y_min, ref_y_max, ImPlotCond_Always);
                    }
                    ImPlotSpec spec;
                    spec.LineColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow color
                    // Plot all selected datasets with specific colors and legend
                    for (size_t i = 0; i < loadedData.size(); i++) {
                        ImPlotSpec spec;
                        spec.LineWeight = 2.0f;
                        
                        // Assign specific colors based on the requested scheme (yellow first)
                        if (i == 0) {
                            spec.LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                        } else if (i == 1) {
                            spec.LineColor = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                        } else if (i == 2) {
                            spec.LineColor = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                        } else if (i == 3) {
                            spec.LineColor = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                        } else if (i == 4) {
                            spec.LineColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                        }
                        
                        const auto& dataToPlot = alignPeaks ? alignedData[i] : loadedData[i];
                        ImPlot::PlotLine("", 
                                       &dataToPlot.referenceDetector[ref_start], 
                                       ref_end - ref_start, 1.0, 0.0, spec);
                    }
                    ImPlot::EndPlot();
                    ImPlot::PopStyleColor(); // Pop grid color
                }
                
                // Primary detector plot (bottom)
                if (ImPlot::BeginPlot("Primary", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_Crosshairs)) {
                    ImPlot::SetupAxes("Sample", "Voltage [V]", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
                    // Set lighter gray grid color
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
                    if (isZoomed) {
                        ImPlot::SetupAxesLimits(ref_start, ref_end, prim_y_min, prim_y_max, ImPlotCond_Always);
                    } else if (shouldAutoscale) {
                        // Set initial view to show all data when new data is loaded
                        ImPlot::SetupAxesLimits(0, loadedData[0].primaryDetector.size(), prim_y_min, prim_y_max, ImPlotCond_Always);
                    }
                    ImPlotSpec spec2;
                    spec2.LineColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow color
                    // Plot all selected datasets with same colors as reference
                    for (size_t i = 0; i < loadedData.size(); i++) {
                        ImPlotSpec spec;
                        spec.LineWeight = 2.0f;
                        
                        // Use same specific colors as reference plot
                        if (i == 0) {
                            spec.LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                        } else if (i == 1) {
                            spec.LineColor = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                        } else if (i == 2) {
                            spec.LineColor = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                        } else if (i == 3) {
                            spec.LineColor = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                        } else if (i == 4) {
                            spec.LineColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                        }
                        
                        const auto& dataToPlot = alignPeaks ? alignedData[i] : loadedData[i];
                        ImPlot::PlotLine("", 
                                       &dataToPlot.primaryDetector[ref_start], 
                                       ref_end - ref_start, 1.0, 0.0, spec);
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
            
            // Handle box selection zoom manually using ImGui mouse input
            // This completely bypasses ImPlot's input system
            ImVec2 mousePos = ImGui::GetMousePos();
            bool isOverPlot = ImGui::IsWindowHovered();
            
            if (isOverPlot && ImGui::IsMouseDown(0) && !isZooming) {
                isZooming = true;
                zoomStart = mousePos;
            }
            if (isZooming && ImGui::IsMouseReleased(0)) {
                zoomEnd = mousePos;
                isZooming = false;
                
                // Calculate zoom based on pixel coordinates
                // This is a simplified approach - in a real app you'd map pixels to data coordinates
                if (fabs(zoomEnd.x - zoomStart.x) > 20 && fabs(zoomEnd.y - zoomStart.y) > 20) {
                    // For now, use a fixed zoom factor
                    // In a production app, you'd calculate the actual data range
                    size_t zoom_center = (ref_start + ref_end) / 2;
                    size_t zoom_width = (ref_end - ref_start) / 4; // Zoom in by 4x
                    
                    ref_start = std::max(size_t(0), zoom_center - zoom_width/2);
                    ref_end = std::min(loadedData[0].referenceDetector.size(), zoom_center + zoom_width/2);
                    prim_start = ref_start;
                    prim_end = ref_end;
                    isZoomed = true;
                }
            }
            
            // Draw custom zoom selection rectangle
            if (isZooming) {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRect(zoomStart, zoomEnd, IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);
                drawList->AddRectFilled(zoomStart, zoomEnd, IM_COL32(255, 255, 0, 80));
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
        
        // Buttons panel (bottom)
        ImGui::Begin("Controls");
        if (ImGui::Button("Set Working Directory")) {
            // Implement proper directory selection dialog
            std::string selectedDirectory = FileBrowser::showDirectorySelectionDialog();
            if (!selectedDirectory.empty()) {
                // Check if the selected directory has a raw_data subdirectory
                std::string rawDataPath = selectedDirectory + "/raw_data";
                if (std::filesystem::exists(rawDataPath) && std::filesystem::is_directory(rawDataPath)) {
                    currentDirectory = rawDataPath; // Use the raw_data subdirectory
                } else {
                    currentDirectory = selectedDirectory; // Fallback to selected directory
                }
                csvFiles = FileBrowser::getCSVFilesInDirectory(currentDirectory);
                dataLoaded = false;
                std::cout << "Working directory set to: " << currentDirectory << std::endl;
            }
        }
        
        ImGui::Checkbox("Align peaks", &alignPeaks);
        
        ImGui::End();
        
        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
    }
    
    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    return 0;
}