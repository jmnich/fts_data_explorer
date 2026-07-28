#include "file_browser.h"
#include <filesystem>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <GLFW/glfw3.h>

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

std::string FileBrowser::pickFolder(GLFWwindow* window, const std::string& title) {
    if (window && glfwWindowShouldClose(window))
        return "";

    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        const char* argv_zenity[]  = {"zenity", "--file-selection", "--directory",
                                       "--title", title.c_str(), nullptr};
        const char* argv_kdialog[] = {"kdialog", "--getexistingdirectory",
                                       "--title", title.c_str(), nullptr};

        execvp("zenity", const_cast<char* const*>(argv_zenity));
        execvp("kdialog", const_cast<char* const*>(argv_kdialog));
        _exit(1);
    }

    close(pipefd[1]);
    char buf[4096] = {};
    size_t total = 0;

    while (total < sizeof(buf) - 1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(pipefd[0], &fds);
        struct timeval tv = {0, 100000};

        int ret = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret > 0) {
            ssize_t n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
        }

        if (window && glfwWindowShouldClose(window)) {
            kill(pid, SIGTERM);
            break;
        }

        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            while (total < sizeof(buf) - 1) {
                ssize_t n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total);
                if (n <= 0) break;
                total += n;
            }
            break;
        }
    }
    close(pipefd[0]);

    std::string result;
    if (total > 0) {
        buf[total] = '\0';
        // Take the last line (skip trailing newlines)
        char* end = buf + total;
        while (end > buf && (*(end - 1) == '\n' || *(end - 1) == '\r'))
            *(--end) = '\0';
        char* last = buf;
        for (char* p = buf; p < end; p++) {
            if (*p == '\n') last = p + 1;
        }
        result = last;
    }

    int status;
    waitpid(pid, &status, 0);

    if (window && glfwWindowShouldClose(window))
        return "";

    return result;
}

std::string FileBrowser::showDirectorySelectionDialog(GLFWwindow* window) {
    return pickFolder(window, "Select Dataset Directory");
}
