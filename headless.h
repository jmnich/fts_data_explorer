#pragma once

#include <string>

struct HeadlessConfig {
    enum class Command {
        None,
        Help,
        Version,
        List,
        OpenGUI,
        Process,
        Reset,
        Template
    };

    Command command = Command::None;
    std::string listType;
    std::string path;
    std::string adapter;
    std::string configPath;
    std::string outputType;
    std::string outputDir;
};

// Parse command-line arguments. Returns false on success, true on error.
bool parseHeadlessArgs(int argc, char* argv[], HeadlessConfig& cfg);

// Execute a headless command. Returns true if the command was fully handled
// (caller should exit). Returns false for None and OpenGUI (continue to GUI).
bool runHeadlessCommand(const HeadlessConfig& cfg);
