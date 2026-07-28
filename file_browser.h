#pragma once

#include <string>
#include <vector>

struct GLFWwindow;

class FileBrowser {
public:
    static std::vector<std::string> getCSVFilesInDirectory(const std::string& directoryPath);
    static std::string showDirectorySelectionDialog(GLFWwindow* window = nullptr);
    static std::string pickFolder(GLFWwindow* window, const std::string& title);
};
