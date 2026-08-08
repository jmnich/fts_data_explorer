#pragma once

#include <string>

#include "config.h"
#include "converter.h"

struct AppState;

// State for the Conversion screen modal (phase5 §4). Owns the running
// converter job; the frame loop polls finished() and joins on the false edge.
struct ConversionScreenState {
    bool open = false;
    bool refreshPending = true;
    bool probed = false;        // tool probes run once per open/Test/edit

    // Setup fields (mirror AppConfig; edits persist immediately — never to
    // workspace.json, spec §8.2). Char buffers for InputText.
    char repoUrlBuf[1024] = "";
    char repoDirBuf[1024] = "";
    char pyPathBuf[256] = "";

    // Conversion inputs
    char inputPathBuf[1024] = "";
    // Output directory: the converter writes <outputDir>/<input stem>.h5
    // (directory inputs use the directory name as the stem).
    char outputDirBuf[1024] = "";
    int selectedIndex = -1;
    int lastSelectedIndex = -1;  // scroll-into-view latch: one-shot SetScrollHereY
    bool inputEdited = false;

    // Run state
    ConverterJob job;       // converter process
    ConverterJob syncJob;   // repo clone/pull
    bool jobStarted = false;
    bool syncStarted = false;
    bool showLog = false;
    std::string lastError;

    // Tool status (refreshed from probeTools when the interpreter changes)
    std::string pyVersion;
    std::string h5pyVersion;
    std::string gitVersion;
    bool gitOk = false;
    bool pyOk = false;
    bool h5pyOk = false;

    // "Python test success/failed" acknowledgment shown next to the Test
    // button for 2 s after clicking it.
    double testToastUntil = 0.0;   // ImGui::GetTime() deadline; 0 = not shown
    bool   testToastOk = false;
};

// Render the modal every frame (no-op when closed). Must be called after
// ImGui::NewFrame(); opens the popup when state.open is set.
void renderConversionScreen(AppState& s);
// Open the modal; prefillInput (e.g. a recent legacy directory) sets the
// converter input path.
void openConversionScreen(AppState& s, const std::string& prefillInput = "");
// Best-effort background repo pull at startup: only when a clone already
// exists and git is present (never a first clone at boot — decision 8).
void startupConverterRefresh(const AppConfig& config);
