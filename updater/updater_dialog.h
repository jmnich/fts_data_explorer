#pragma once

#include <chrono>
#include <string>

#include "updater_config.h"
#include "updater_service.h"

namespace updater {

// All UI state of the updater modal. Lives in one struct so the dialog has no
// hidden globals beyond state(); owned by updater_dialog.cpp.
struct DialogState {
    UpdateService service;
    Config config;
    bool configLoaded = false;
    bool autoStarted = false;    // the one-shot startup check fired
    bool manual = false;         // started from the menu; ignores cooldown/skip
    bool open = false;           // a modal should be/shown
    bool skipRequested = false;  // "Skip this version" pressed
    bool notesExpanded = false;  // full release notes vs the first lines
    bool closeRequested = false; // "Restart Now" applied: ask the loop to close
    int lastPhase = 0;           // UpdatePhase int, for transition detection
    std::string dismissedVersion;  // "Later" pressed for exactly this release
    bool readyDeferred = false;    // "Later" pressed on the Ready-to-apply body
    bool cancelling = false;       // Cancel pressed, worker not settled yet
    bool userInitiated = false;    // Download/Restart clicked this cycle
    std::chrono::steady_clock::time_point upToDateUntil{};  // toast deadline
    std::string exePath;         // captured in startup(), before any swap
};

DialogState& state();
void startCheck(bool manualCheck);

} // namespace updater
