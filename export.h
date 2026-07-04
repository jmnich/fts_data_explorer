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
    static constexpr const char* ARTIFACT_SPECTRA    = "Spectra from selected files";

    std::vector<std::string> artifactLabels;
    std::vector<int>         artifactChecked; // 0=false, 1=true

    ExportPanel();

    void render();
    void refreshArtifacts();
    bool isArtifactAvailable(const char* label) const;
    void performExport(const std::string& dir);

private:
    void writeCorrectedIFGCsv(const std::string& dir);
    void writeUncorrectedIFGCsv(const std::string& dir);
    void writeAvgSpectrumCsv(const std::string& dir);
    void writeSpectraCsv(const std::string& dir);
};
