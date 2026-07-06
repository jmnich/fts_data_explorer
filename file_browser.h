#pragma once

#include <string>
#include <vector>

class FileBrowser {
public:
    static std::vector<std::string> getCSVFilesInDirectory(const std::string& directoryPath);
    static std::string showDirectorySelectionDialog();
};
