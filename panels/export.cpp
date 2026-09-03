#include "export.h"
#include "app_state.h"
#include "theme.h"
#include "popup_utils.h"
#include "spectrum.h"
#include "average_spectrum.h"
#include "allan_variance.h"
#include "t100.h"
#include "spectral_toolbox.h"
#include "interferogram_data.h"
#include "workspace_reader.h"

#include "imgui.h"
#include "file_browser.h"
#include <GLFW/glfw3.h>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cstring>

// Basename without the last extension, used for export file/column names.
// Handles both path separators (Windows member ids can carry '\').
static std::string exportBaseName(const std::string& p) {
    std::string name = p;
    size_t ls = name.find_last_of("/\\");
    if (ls != std::string::npos) name = name.substr(ls + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

ExportPanel::ExportPanel()
{
    artifactLabels = {
        ARTIFACT_CORR_IFG,
        ARTIFACT_UNCORR_IFG,
        ARTIFACT_AVG_SPECT,
        ARTIFACT_SNR_SPECT,
        ARTIFACT_SPECTRA,
        ARTIFACT_ALLAN_3D,
        ARTIFACT_ALLAN_SLICE,
        ARTIFACT_T100_TRANS,
        ARTIFACT_T100_ALL_TRANS,
        ARTIFACT_T100_STDDEV,
        ARTIFACT_ABSORBANCE,
        ARTIFACT_TRANSMITTANCE
    };
    artifactChecked.assign(artifactLabels.size(), 0);
}

bool ExportPanel::isArtifactAvailable(const char* label) const
{
    if (!appState) return false;
    std::string lbl(label);
    if (lbl == ARTIFACT_CORR_IFG || lbl == ARTIFACT_UNCORR_IFG)
        return appState->active->dataLoaded && !appState->active->selectedFiles.empty()
               && appState->active->datasetInfo.hasInterferograms;
    if (lbl == ARTIFACT_SPECTRA) {
        if (!appState->active->dataLoaded) return false;
        for (bool v : appState->active->filesSelectedForAveraging)
            if (v) return true;
        return false;
    }
    if (lbl == ARTIFACT_AVG_SPECT)
        return appState->active->averageSpectrum.averageAvailable;
    if (lbl == ARTIFACT_SNR_SPECT)
        return appState->active->snrSpectrum.snrAvailable;
    if (lbl == ARTIFACT_ALLAN_3D || lbl == ARTIFACT_ALLAN_SLICE)
        return appState->active->allanVariance.allanAvailable;
    if (lbl == ARTIFACT_T100_TRANS || lbl == ARTIFACT_T100_ALL_TRANS)
        return appState->active->t100.transmittanceAvailable;
    if (lbl == ARTIFACT_T100_STDDEV)
        return appState->active->t100.stddevAvailable;
    if (lbl == ARTIFACT_ABSORBANCE || lbl == ARTIFACT_TRANSMITTANCE)
        return appState->active->t100.transmittanceAvailable;
    return false;
}

void ExportPanel::refreshArtifacts()
{
    for (size_t i = 0; i < artifactLabels.size(); i++) {
        if (!isArtifactAvailable(artifactLabels[i].c_str())) {
            artifactChecked[i] = 0;
        }
    }
}

static std::string sanitizeFilename(const std::string& s)
{
    std::string out = s;
    for (auto& ch : out) {
        if (ch == ' ' || ch == '/' || ch == '\\' || ch == ':')
            ch = '_';
    }
    return out;
}

void ExportPanel::render()
{
    // Check for deferred export completion (set after SwapBuffers in main loop)
    if (exportJustCompleted) {
        exportJustCompleted = false;
        ImGui::OpenPopup("Export Complete");
    }

    // Skip clearing checkboxes while any calculation is in progress —
    // artifacts become temporarily unavailable during recomputation.
    bool anyCalcInProgress = false;
    if (appState) {
        anyCalcInProgress = appState->active->averageSpectrum.calcInProgress
                         || appState->active->snrSpectrum.calcInProgress
                         || appState->active->allanVariance.calcInProgress
                         || appState->active->t100.calcStdInProgress;
    }
    if (!anyCalcInProgress)
        refreshArtifacts();

    if (!appState || !appState->active->dataLoaded) {
        ImGui::Text("No data loaded.");
        return;
    }

    ImGui::Text("Format: .csv");

    ImGui::Separator();

    if (ImGui::BeginChild("ExportArtifacts", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 8), true)) {
        for (size_t i = 0; i < artifactLabels.size(); i++) {
            bool avail = isArtifactAvailable(artifactLabels[i].c_str());
            if (!avail) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 0.7f));
                bool dummy = false;
                ImGui::Checkbox(artifactLabels[i].c_str(), &dummy);
                ImGui::PopStyleColor();
            } else {
                bool checked = (artifactChecked[i] != 0);
                if (ImGui::Checkbox(artifactLabels[i].c_str(), &checked)) {
                    artifactChecked[i] = checked ? 1 : 0;
                    appState->needsRedraw = true;
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginDisabled(anyCalcInProgress);
    if (ImGui::Button("Export", ImVec2(-1, 0))) {
        bool anySelected = false;
        for (size_t i = 0; i < artifactChecked.size(); i++) {
            if (artifactChecked[i] != 0 && isArtifactAvailable(artifactLabels[i].c_str())) {
                anySelected = true;
                break;
            }
        }
        if (!anySelected) {
            ImGui::OpenPopup("Export Warning");
        } else {
            std::string folder = FileBrowser::pickFolder(glfwGetCurrentContext(), "Select Export Directory");
            if (!folder.empty()) {
                exportDir = folder;
                exportPending = true;
            }
        }
    }
    ImGui::EndDisabled();

    ImVec4 accent = GetAccentBase(StringToAccentColor(appState->currentAccentColor));

    beginModal(440.0f, accent);
    if (ImGui::BeginPopupModal("Export Warning", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        // NoTitleBar: the title moves into the body so removing the header
        // loses no information.
        ImGui::Text("Export Warning");
        ImGui::Spacing();
        ImGui::TextWrapped("No artifacts selected or available for export.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        static int warnFocus = 0;
        static bool warnWasOpen = false;
        if (modalButtonRow({"OK"}, warnFocus, warnWasOpen, accent) == 0 ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        warnWasOpen = true;
        drawModalAccentFrame(accent);
        ImGui::EndPopup();
    }
    endModal();

    beginModal(440.0f, accent);
    if (ImGui::BeginPopupModal("Export Complete", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        // NoTitleBar: the body restates the title ("Export complete.").
        ImGui::TextWrapped("Export complete.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        static int doneFocus = 0;
        static bool doneWasOpen = false;
        if (modalButtonRow({"OK"}, doneFocus, doneWasOpen, accent) == 0 ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        doneWasOpen = true;
        drawModalAccentFrame(accent);
        ImGui::EndPopup();
    }
    endModal();


}

void ExportPanel::performExport(const std::string& dir)
{
    if (artifactChecked[0]) writeCorrectedIFGCsv(dir);
    if (artifactChecked[1]) writeUncorrectedIFGCsv(dir);
    if (artifactChecked[2]) writeAvgSpectrumCsv(dir);
    if (artifactChecked[3]) writeSnrSpectrumCsv(dir);
    if (artifactChecked[4]) writeSpectraCsv(dir);
    if (artifactChecked[5]) writeAllan3DCsv(dir);
    if (artifactChecked[6]) writeAllanSliceCsv(dir);
    if (artifactChecked[7]) writeT100TransCsv(dir);
    if (artifactChecked[8]) writeT100AllTransCsv(dir);
    if (artifactChecked[9]) writeT100StdDevCsv(dir);
}

void ExportPanel::executePendingExport()
{
    if (!exportPending) return;
    performExport(exportDir);
    exportPending = false;
    exportJustCompleted = true;
}

bool ExportPanel::exportArtifact(const std::string& label, const std::string& dir)
{
    if (label == ARTIFACT_CORR_IFG)      { writeCorrectedIFGCsv(dir); return true; }
    if (label == ARTIFACT_UNCORR_IFG)    { writeUncorrectedIFGCsv(dir); return true; }
    if (label == ARTIFACT_AVG_SPECT)     { writeAvgSpectrumCsv(dir); return true; }
    if (label == ARTIFACT_SNR_SPECT)     { writeSnrSpectrumCsv(dir); return true; }
    if (label == ARTIFACT_SPECTRA)       { writeSpectraCsv(dir); return true; }
    if (label == ARTIFACT_ALLAN_3D)      { writeAllan3DCsv(dir); return true; }
    if (label == ARTIFACT_ALLAN_SLICE)   { writeAllanSliceCsv(dir); return true; }
    if (label == ARTIFACT_T100_TRANS)    { writeT100TransCsv(dir); return true; }
    if (label == ARTIFACT_T100_ALL_TRANS){ writeT100AllTransCsv(dir); return true; }
    if (label == ARTIFACT_T100_STDDEV)   { writeT100StdDevCsv(dir); return true; }
    if (label == ARTIFACT_ABSORBANCE)    { writeAbsorbanceCsv(dir); return true; }
    if (label == ARTIFACT_TRANSMITTANCE) { writeTransmittanceCsv(dir); return true; }
    return false;
}

// ---------------------------------------------------------------------------
//  CSV exports
// ---------------------------------------------------------------------------

void ExportPanel::writeCorrectedIFGCsv(const std::string& dir)
{
    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    for (size_t i = 0; i < appState->active->selectedFiles.size(); i++) {
        if (i >= appState->active->rawDataCache.size()) continue;
        const auto& raw = appState->active->rawDataCache[i];

        std::vector<double> opdX;
        if (appState->active->datasetInfo.axisIsCorrected) {
            // Axis already corrected: opdAxis is OPD; convert to um (m -> um).
            if (raw.opdAxis.empty() || raw.primaryDetector.empty()) continue;
            opdX.resize(raw.opdAxis.size());
            for (size_t j = 0; j < raw.opdAxis.size(); j++)
                opdX[j] = raw.opdAxis[j] * 1e6;
        } else {
            if (raw.referenceDetector.empty() || raw.primaryDetector.empty()) continue;
            // Reference-based axis is mirror displacement; double to round-trip OPD
            // (matches spectral_toolbox.cpp processSpectrum: OPD = 2.0 * maxOPD).
            if (appState->active->xCorrectionMethod == 1) {
                SpectralToolbox::xAxisFromPeaks(
                    raw.referenceDetector, appState->active->spectrum.refLaserTextbox,
                    appState->active->peakProminenceThreshold, opdX);
            } else {
                SpectralToolbox::xAxisFromHilbert(raw.referenceDetector,
                                                  appState->active->spectrum.refLaserTextbox,
                                                  opdX);
            }
            if (opdX.empty()) continue;
            for (double& v : opdX) v *= 2.0;
        }

        std::string fname = exportBaseName(appState->active->selectedFilenames[i]);
        fname = sanitizeFilename(fname);

        std::string path = dir + "/" + dsName + "_corrected_ifg_" + fname + ".csv";
        std::ofstream ofs(path);
        if (!ofs.is_open()) continue;
        ofs << std::setprecision(15);
        ofs << "OPD [um],Primary Detector [V]\n";
        size_t n = std::min(opdX.size(), raw.primaryDetector.size());
        for (size_t j = 0; j < n; j++) {
            ofs << opdX[j] << "," << raw.primaryDetector[j] << "\n";
        }
    ofs.close();
    }
}
void ExportPanel::writeUncorrectedIFGCsv(const std::string& dir)
{
    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_uncorrected_ifgs.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    size_t nFiles = std::min(appState->active->selectedFiles.size(), appState->active->rawDataCache.size());
    size_t maxLen = 0;
    for (size_t i = 0; i < nFiles; i++) {
        maxLen = std::max(maxLen, appState->active->rawDataCache[i].referenceDetector.size());
        maxLen = std::max(maxLen, appState->active->rawDataCache[i].primaryDetector.size());
    }

    ofs << "Index";
    for (size_t i = 0; i < nFiles; i++) {
        ofs << ",Reference_" << i << ",Primary_" << i;
    }
    ofs << "\n";

    for (size_t row = 0; row < maxLen; row++) {
        ofs << row;
        for (size_t i = 0; i < nFiles; i++) {
            const auto& raw = appState->active->rawDataCache[i];
            if (row < raw.referenceDetector.size())
                ofs << "," << raw.referenceDetector[row];
            else
                ofs << ",";
            if (row < raw.primaryDetector.size())
                ofs << "," << raw.primaryDetector[row];
            else
                ofs << ",";
        }
        ofs << "\n";
    }
    ofs.close();
}

void ExportPanel::writeAvgSpectrumCsv(const std::string& dir)
{
    const auto& avg = appState->active->averageSpectrum;
    if (!avg.averageAvailable || avg.cachedAverageX.empty() || avg.cachedAverageY.empty())
        return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_average_spectrum.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (avg.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (avg.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel << ",Magnitude [V]\n";
    size_t n = std::min(avg.cachedAverageX.size(), avg.cachedAverageY.size());
    for (size_t j = 0; j < n; j++) {
        ofs << avg.cachedAverageX[j] << "," << avg.cachedAverageY[j] << "\n";
    }
    ofs.close();
}

void ExportPanel::writeSnrSpectrumCsv(const std::string& dir)
{
    const auto& snr = appState->active->snrSpectrum;
    if (!snr.snrAvailable || snr.cachedSnrX.empty() || snr.cachedSnrY.empty())
        return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_snr_spectrum.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (snr.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (snr.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel << ",SNR\n";
    size_t n = std::min(snr.cachedSnrX.size(), snr.cachedSnrY.size());
    for (size_t j = 0; j < n; j++) {
        ofs << snr.cachedSnrX[j] << "," << snr.cachedSnrY[j] << "\n";
    }
    ofs.close();
}

void ExportPanel::writeSpectraCsv(const std::string& dir)
{
    // Build list of checked files
    std::vector<std::string> checkedFull;
    std::vector<std::string> checkedShort;
    for (size_t i = 0; i < appState->active->sortedFiles.size() && i < appState->active->filesSelectedForAveraging.size(); i++) {
        if (appState->active->filesSelectedForAveraging[i]) {
            checkedFull.push_back(appState->active->sortedFiles[i]);
            std::string fn = appState->active->sortedFiles[i];
            size_t ls = fn.find_last_of("/\\");
            if (ls != std::string::npos) fn = fn.substr(ls + 1);
            checkedShort.push_back(fn);
        }
    }
    if (checkedFull.empty()) return;

    // Ensure each checked file's spectrum is cached; track success
    size_t nFiles = checkedFull.size();
    std::vector<bool> fileOk(nFiles, false);
    for (size_t i = 0; i < nFiles; i++) {
        const std::string& fid = checkedShort[i];
        if (appState->active->spectrum.cachedFrequencies.find(fid) == appState->active->spectrum.cachedFrequencies.end() ||
            appState->active->spectrum.cachedSpectra.find(fid) == appState->active->spectrum.cachedSpectra.end()) {
            if (!appState->active->spectrum.computeAndCacheSpectrum(checkedFull[i], fid)) {
                std::cerr << "Warning: Could not compute spectrum for " << checkedFull[i] << std::endl;
                continue;
            }
        }
        if (appState->active->spectrum.cachedFrequencies.count(fid) &&
            appState->active->spectrum.cachedSpectra.count(fid))
            fileOk[i] = true;
    }

    // Find first successful file for master X axis
    int masterIdx = -1;
    for (size_t i = 0; i < nFiles; i++) {
        if (fileOk[i]) { masterIdx = static_cast<int>(i); break; }
    }
    if (masterIdx < 0) return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_spectra.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (appState->active->spectrum.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (appState->active->spectrum.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel;
    for (size_t i = 0; i < nFiles; i++) {
        std::string fn = exportBaseName(checkedShort[i]);
        ofs << ",Magnitude_" << i << " [" << fn << "]";
    }
    ofs << "\n";

    const auto& masterFreq = appState->active->spectrum.cachedFrequencies.at(checkedShort[masterIdx]);
    size_t nRows = masterFreq.size();
    for (size_t r = 0; r < nRows; r++) {
        ofs << masterFreq[r];
        for (size_t i = 0; i < nFiles; i++) {
            if (!fileOk[i]) { ofs << ","; continue; }
            const auto& fid = checkedShort[i];
            const auto& freq = appState->active->spectrum.cachedFrequencies.at(fid);
            const auto& spec = appState->active->spectrum.cachedSpectra.at(fid);
            if (freq.empty() || spec.empty()) {
                ofs << ",";
                continue;
            }
            double val;
            if (freq.front() <= freq.back()) {
                if (masterFreq[r] <= freq.front()) val = spec.front();
                else if (masterFreq[r] >= freq.back()) val = spec.back();
                else {
                    auto it = std::lower_bound(freq.begin(), freq.end(), masterFreq[r]);
                    size_t hi = it - freq.begin();
                    size_t lo = hi - 1;
                    double frac = (masterFreq[r] - freq[lo]) / (freq[hi] - freq[lo]);
                    val = spec[lo] * (1.0 - frac) + spec[hi] * frac;
                }
            } else {
                if (masterFreq[r] >= freq.front()) val = spec.front();
                else if (masterFreq[r] <= freq.back()) val = spec.back();
                else {
                    auto it = std::lower_bound(freq.begin(), freq.end(), masterFreq[r], std::greater<double>());
                    size_t hi = it - freq.begin();
                    size_t lo = hi - 1;
                    double frac = (masterFreq[r] - freq[lo]) / (freq[hi] - freq[lo]);
                    val = spec[lo] * (1.0 - frac) + spec[hi] * frac;
                }
            }
            ofs << "," << val;
        }
        ofs << "\n";
    }
    ofs.close();
}

void ExportPanel::writeAllan3DCsv(const std::string& dir)
{
    const auto& al = appState->active->allanVariance;
    if (!al.allanAvailable || al.cachedSurfaceWavelengths.empty() ||
        al.cachedSurfaceTaus.empty() || al.cachedSurfaceAllanVar.empty())
        return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_allan_3d.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* wlLabel = "Wavenumber [cm-1]";
    if (al.xUnitSelector == 1) wlLabel = "Wavelength [um]";
    else if (al.xUnitSelector == 2) wlLabel = "Frequency [THz]";

    ofs << wlLabel << ",Tau [measurements],Allan Variance\n";

    int M = al.numSurfaceWavelengths;
    int N = al.numSurfaceTaus;

    auto convertWl = [&](double um) -> double {
        using ST = SpectralToolbox;
        if (al.xUnitSelector == 0) return ST::convertUmToCm(um);
        if (al.xUnitSelector == 2) return ST::convertUmToTHz(um);
        return um;
    };

    for (int i = 0; i < M; ++i) {
        double wl = convertWl(al.cachedSurfaceWavelengths[i]);
        for (int j = 0; j < N; ++j) {
            ofs << wl << "," << al.cachedSurfaceTaus[j] << "," << al.cachedSurfaceAllanVar[i * N + j] << "\n";
        }
    }
    ofs.close();
}

void ExportPanel::writeAllanSliceCsv(const std::string& dir)
{
    const auto& al = appState->active->allanVariance;
    if (!al.allanAvailable || al.cachedSurfaceWavelengths.empty() ||
        al.cachedSurfaceTaus.empty() || al.cachedSurfaceAllanVar.empty())
        return;

    int idx = al.selectedSliceIndex;
    if (idx < 0 || idx >= al.numSurfaceWavelengths) return;

    double wlUm = al.cachedSurfaceWavelengths[idx];
    double wlDisplay = wlUm;
    const char* wlUnit = "um";
    using ST = SpectralToolbox;
    if (al.xUnitSelector == 0) { wlDisplay = ST::convertUmToCm(wlUm); wlUnit = "cm-1"; }
    else if (al.xUnitSelector == 2) { wlDisplay = ST::convertUmToTHz(wlUm); wlUnit = "THz"; }

    char wlStr[64];
    std::snprintf(wlStr, sizeof(wlStr), "%.4f_%s", wlDisplay, wlUnit);

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_allan_slice_" + wlStr + ".csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    ofs << "Tau [measurements],Allan Variance\n";

    int N = al.numSurfaceTaus;
    for (int j = 0; j < N; ++j) {
        ofs << al.cachedSurfaceTaus[j] << "," << al.cachedSurfaceAllanVar[idx * N + j] << "\n";
    }
    ofs.close();
}

void ExportPanel::writeT100TransCsv(const std::string& dir)
{
    const auto& t100 = appState->active->t100;
    if (!t100.transmittanceAvailable || t100.lastKnownSelection.empty())
        return;
    const std::string& fileId = t100.lastKnownSelection[0];
    auto xIt = t100.cachedTransX.find(fileId);
    auto yIt = t100.cachedTransY.find(fileId);
    if (xIt == t100.cachedTransX.end() || yIt == t100.cachedTransY.end())
        return;
    const auto& xv = xIt->second;
    const auto& yv = yIt->second;
    if (xv.empty() || yv.empty()) return;

    std::string srcName = exportBaseName(fileId);

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_t100_transmission_" + srcName + ".csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (t100.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (t100.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel << ",T(%)\n";
    size_t n = std::min(xv.size(), yv.size());
    for (size_t j = 0; j < n; j++)
        ofs << xv[j] << "," << yv[j] << "\n";
    ofs.close();
}

void ExportPanel::writeT100AllTransCsv(const std::string& dir)
{
    const auto& t100 = appState->active->t100;
    if (!t100.transmittanceAvailable || !t100.referenceAvailable)
        return;

    std::vector<std::string> checkedFiles;
    for (size_t i = 0; i < appState->active->sortedFiles.size() && i < appState->active->filesSelectedForAveraging.size(); i++) {
        if (appState->active->filesSelectedForAveraging[i])
            checkedFiles.push_back(appState->active->sortedFiles[i]);
    }
    if (checkedFiles.empty()) return;

    std::vector<std::vector<double>> allTransX(checkedFiles.size());
    std::vector<std::vector<double>> allTransY(checkedFiles.size());
    bool anyValid = false;

    for (size_t i = 0; i < checkedFiles.size(); i++) {
        auto raw = workspaceRead(appState->active->workspace, checkedFiles[i]);
        SpectralToolbox::ProcessedSpectrum ps;
        if (appState->active->datasetInfo.hasPrecomputedSpectra) {
            ps.spectrumX = raw.referenceDetector;
            for (double& f : ps.spectrumX)
                f = SpectralToolbox::convertXValue(f,
                    SpectralToolbox::SpectrumXUnit::CmInv,
                    static_cast<SpectralToolbox::SpectrumXUnit>(t100.plot.xUnitSelector));
            ps.spectrumY = std::move(raw.primaryDetector);
        } else if (appState->active->datasetInfo.axisIsCorrected) {
            for (auto& v : raw.opdAxis) v *= 1e6;
            ps = SpectralToolbox::processSpectrumFromCorrectedAxis(
                raw.primaryDetector, raw.opdAxis,
                appState->active->spectrum.Kpadding,
                static_cast<SpectralToolbox::SpectrumXUnit>(t100.plot.xUnitSelector),
                static_cast<ApodizationWindow>(appState->active->spectrum.apodizationSelector),
                appState->active->spectrum.apodizationParams);
        } else {
            ps = SpectralToolbox::processSpectrum(
                raw.primaryDetector, raw.referenceDetector,
                appState->active->spectrum.refLaserTextbox,
                appState->active->spectrum.Kpadding,
                static_cast<SpectralToolbox::SpectrumXUnit>(t100.plot.xUnitSelector),
                static_cast<ApodizationWindow>(appState->active->spectrum.apodizationSelector),
                appState->active->spectrum.apodizationParams,
                static_cast<SpectralToolbox::XCorrectionMethod>(appState->active->xCorrectionMethod),
                appState->active->peakProminenceThreshold);
        }
        if (ps.spectrumX.empty() || ps.spectrumY.empty()) continue;
        std::vector<double> tx, ty;
        if (appState->active->t100.computeTransmittanceFromVectors(
                ps.spectrumX, ps.spectrumY, t100.plot.xUnitSelector, tx, ty)) {
            allTransX[i] = std::move(tx);
            allTransY[i] = std::move(ty);
            anyValid = true;
        }
    }
    if (!anyValid) return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_t100_all_transmissions.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (t100.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (t100.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel;
    for (size_t i = 0; i < checkedFiles.size(); i++) {
        if (!allTransX[i].empty()) {
            std::string fn = exportBaseName(checkedFiles[i]);
            ofs << ",T%_" << i << " [" << fn << "]";
        }
    }
    ofs << "\n";

    const std::vector<double>* masterXPtr = nullptr;
    for (size_t i = 0; i < allTransX.size(); i++) {
        if (!allTransX[i].empty()) { masterXPtr = &allTransX[i]; break; }
    }
    if (!masterXPtr) { ofs.close(); return; }
    const auto& masterX = *masterXPtr;
    size_t nRows = masterX.size();
    for (size_t r = 0; r < nRows; r++) {
        ofs << masterX[r];
        for (size_t i = 0; i < checkedFiles.size(); i++) {
            if (allTransX[i].empty()) { ofs << ","; continue; }
            if (r >= allTransY[i].size()) { ofs << ","; continue; }
            ofs << "," << allTransY[i][r];
        }
        ofs << "\n";
    }
    ofs.close();
}

void ExportPanel::writeT100StdDevCsv(const std::string& dir)
{
    const auto& t100 = appState->active->t100;
    if (!t100.stddevAvailable || t100.cachedStdX.empty() || t100.cachedStdY.empty())
        return;

    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    std::string path = dir + "/" + dsName + "_t100_stddev.csv";
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << std::setprecision(15);

    const char* xLabel = "Wavenumber [cm-1]";
    if (t100.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (t100.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    ofs << xLabel << ",Standard Deviation T(%)\n";
    size_t n = std::min(t100.cachedStdX.size(), t100.cachedStdY.size());
    for (size_t j = 0; j < n; j++)
        ofs << t100.cachedStdX[j] << "," << t100.cachedStdY[j] << "\n";
    ofs.close();
}

void ExportPanel::writeAbsorbanceCsv(const std::string& dir)
{
    const auto& t100 = appState->active->t100;
    if (!t100.transmittanceAvailable || t100.lastKnownSelection.empty())
        return;
    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    const char* xLabel = "Wavenumber [cm-1]";
    if (t100.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (t100.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    for (const auto& fileId : t100.lastKnownSelection) {
        auto xIt = t100.cachedTransX.find(fileId);
        auto yIt = t100.cachedTransY.find(fileId);
        if (xIt == t100.cachedTransX.end() || yIt == t100.cachedTransY.end()) continue;
        const auto& xv = xIt->second;
        const auto& yv = yIt->second;
        if (xv.empty() || yv.empty()) continue;
        std::string srcName = exportBaseName(fileId);
        srcName = sanitizeFilename(srcName);
        std::string path = dir + "/" + dsName + "_absorbance_" + srcName + ".csv";
        std::ofstream ofs(path);
        if (!ofs.is_open()) continue;
        ofs << std::setprecision(15);
        ofs << xLabel << ",Absorbance\n";
        size_t n = std::min(xv.size(), yv.size());
        for (size_t j = 0; j < n; j++) {
            double tFraction = yv[j] / 100.0;  // T% → fraction
            double a = (tFraction > 1e-15) ? -std::log10(tFraction) : 0.0;
            ofs << xv[j] << "," << a << "\n";
        }
        ofs.close();
    }
}

void ExportPanel::writeTransmittanceCsv(const std::string& dir)
{
    const auto& t100 = appState->active->t100;
    if (!t100.transmittanceAvailable || t100.lastKnownSelection.empty())
        return;
    std::string dsName = sanitizeFilename(appState->active->currentDatasetName);
    const char* xLabel = "Wavenumber [cm-1]";
    if (t100.plot.xUnitSelector == 1) xLabel = "Wavelength [um]";
    else if (t100.plot.xUnitSelector == 2) xLabel = "Frequency [THz]";

    for (const auto& fileId : t100.lastKnownSelection) {
        auto xIt = t100.cachedTransX.find(fileId);
        auto yIt = t100.cachedTransY.find(fileId);
        if (xIt == t100.cachedTransX.end() || yIt == t100.cachedTransY.end()) continue;
        const auto& xv = xIt->second;
        const auto& yv = yIt->second;
        if (xv.empty() || yv.empty()) continue;
        std::string srcName = exportBaseName(fileId);
        srcName = sanitizeFilename(srcName);
        std::string path = dir + "/" + dsName + "_transmittance_" + srcName + ".csv";
        std::ofstream ofs(path);
        if (!ofs.is_open()) continue;
        ofs << std::setprecision(15);
        ofs << xLabel << ",Transmittance\n";
        size_t n = std::min(xv.size(), yv.size());
        for (size_t j = 0; j < n; j++)
            ofs << xv[j] << "," << (yv[j] / 100.0) << "\n";  // T% → fraction
        ofs.close();
    }
}
void ExportPanel::renderPanel() {
        ImGui::Begin("Export");
        if (appState->active->dataLoaded) {
            render();
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

}

// ── Park/resume mirror support (M2.1) ───────────────────────────────────────




