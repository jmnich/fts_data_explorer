#pragma once

#include <string>
#include <vector>

struct GLFWwindow;

class FileBrowser {
public:
    static std::vector<std::string> getCSVFilesInDirectory(const std::string& directoryPath);
    static std::string showDirectorySelectionDialog(GLFWwindow* window = nullptr);
    static std::string pickFolder(GLFWwindow* window, const std::string& title);
    // Open a file for reading. displayName is shown in the filter dropdown
    // (e.g. "HDF5 files"); globPattern filters the listing (e.g. "*.h5").
    static std::string showFileOpenDialog(const std::string& title,
                                          const std::string& displayName,
                                          const std::string& globPattern,
                                          GLFWwindow* window = nullptr,
                                          const std::string& defaultFolder = "");
};
