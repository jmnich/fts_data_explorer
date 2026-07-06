#include "file_browser.h"
#include <filesystem>
#include "tinyfiledialogs.h"

std::vector<std::string> FileBrowser::getCSVFilesInDirectory(const std::string& directoryPath) {
    std::vector<std::string> csvFiles;

    if (directoryPath.empty()) {
        return csvFiles;
    }

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

std::string FileBrowser::showDirectorySelectionDialog() {
    const char* folder = tinyfd_selectFolderDialog("Select Dataset Directory", "");
    if (!folder) {
        return "";
    }
    return std::string(folder);
}
