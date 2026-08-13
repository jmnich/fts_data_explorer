#pragma once

#include <vector>
#include <string>

struct AppState;

class ExportPanel {
public:
    AppState* appState = nullptr;

    static constexpr const char* ARTIFACT_CORR_IFG   = "Corrected interferograms from selected files";
    static constexpr const char* ARTIFACT_UNCORR_IFG = "Uncorrected interferograms from selected files";
    static constexpr const char* ARTIFACT_AVG_SPECT  = "Average spectrum";
    static constexpr const char* ARTIFACT_SNR_SPECT  = "SNR spectrum";
    static constexpr const char* ARTIFACT_SPECTRA    = "Spectra from selected files";
    static constexpr const char* ARTIFACT_ALLAN_3D   = "Allan-Werle 3D";
    static constexpr const char* ARTIFACT_ALLAN_SLICE = "Allan-Werle slice";
    static constexpr const char* ARTIFACT_T100_TRANS    = "100% T transmission line";
    static constexpr const char* ARTIFACT_T100_ALL_TRANS = "100% T lines for all files";
    static constexpr const char* ARTIFACT_T100_STDDEV    = "100% T standard deviation";

    std::vector<std::string> artifactLabels;
    std::vector<int>         artifactChecked; // 0=false, 1=true

    // Deferred export state (between-frames processing)
    bool        exportPending = false;
    bool        exportJustCompleted = false;
    std::string exportDir;

    ExportPanel();

    void render();
    // Docked "Export" window (moved out of main.cpp, Phase-1 M1.2c).
    void renderPanel();
    void refreshArtifacts();
    bool isArtifactAvailable(const char* label) const;
    void performExport(const std::string& dir);

    // Called from main loop after SwapBuffers to run the deferred export
    void executePendingExport();

    // Export a single artifact by label (used by headless mode -p)
    bool exportArtifact(const std::string& label, const std::string& dir);

    // Park/resume mirror support (M2.1): heavy members (caches, futures) are
    // moved, scalars copied. Every per-workspace field must appear in BOTH
    // directions.

private:
    void writeCorrectedIFGCsv(const std::string& dir);
    void writeUncorrectedIFGCsv(const std::string& dir);
    void writeAvgSpectrumCsv(const std::string& dir);
    void writeSnrSpectrumCsv(const std::string& dir);
    void writeSpectraCsv(const std::string& dir);
    void writeAllan3DCsv(const std::string& dir);
    void writeAllanSliceCsv(const std::string& dir);
    void writeT100TransCsv(const std::string& dir);
    void writeT100AllTransCsv(const std::string& dir);
    void writeT100StdDevCsv(const std::string& dir);
};
