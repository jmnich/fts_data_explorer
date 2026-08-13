#include "headless.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include "nlohmann/json.hpp"
#include "config.h"
#include "app_state.h"
#include "version.h"
#include "export.h"
#include "apodization.h"
#include "spectral_toolbox.h"
#include "converter.h"
#include "app_dirs.h"

#include "hdf/h5_store.h"
#include "workspace_reader.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string getImguiIniPath() {
    return "imgui.ini";
}

// ---------------------------------------------------------------------------
// Help (-help)
// ---------------------------------------------------------------------------
static void handleHelp() {
    std::cout << "FTS Data Explorer " << APP_VERSION << " - Headless Mode\n"
              << "\n"
              << "Usage: fts_data_explorer [flag] [arguments]\n"
              << "\n"
              << "Flags:\n"
              << "  No flag        Launch the GUI normally with welcome screen.\n"
              << "  -help          Print this help message and exit.\n"
              << "  -v             Print version (" << APP_VERSION << ") and exit.\n"
              << "  -l [type]      List available options. Types: converter, output, recent.\n"
              << "  -w <workspace.h5> <output type> <output dir> [<config.json>]\n"
              << "                 Open workspace, compute <output type> into it, save in\n"
              << "                 place and export. Config optional (saved view state\n"
              << "                 otherwise); processing.workerThreads from config only,\n"
              << "                 pool defaults to hardware_concurrency otherwise.\n"
              << "  -c <converter> <input> <output.h5>\n"
              << "                 Run a converter (<id> from -l converter, or a direct\n"
              << "                 .py path) on <input>, validate the result, exit 0/1.\n"
              << "                 Uses the local clone as-is (no implicit network).\n"
              << "  -sync-converters\n"
              << "                 Clone (first run) or pull the standard converter repo.\n"
              << "  -t             Generate template.json with all config settings.\n"
              << "  -r             Reset config by deleting config file and imgui.ini.\n"
              << std::endl;
}

// ---------------------------------------------------------------------------
// Version (-v)
// ---------------------------------------------------------------------------
static void handleVersion() {
    std::cout << APP_VERSION << std::endl;
}

// ---------------------------------------------------------------------------
// List (-l)
// ---------------------------------------------------------------------------
static void handleList(const std::string& type) {
    if (type.empty()) {
        std::cout << "Available list types: converter, output, recent" << std::endl;
        return;
    }

    if (type == "converter") {
        AppConfig config;
        std::string configFilePath = getConfigFilePath();
        if (std::filesystem::exists(configFilePath)) {
            config.loadFromFile(configFilePath);
        }
        std::string repoDir = config.converterRepoDir.empty()
            ? appDataDir() + "/converter-repo" : config.converterRepoDir;
        ConverterRegistry::instance().refresh(appDataDir() + "/converters",
                                              config.converterPaths, repoDir);
        for (const auto& c : ConverterRegistry::instance().all()) {
            std::cout << c.id;
            if (c.broken) {
                std::cout << " [BROKEN: " << c.error << "]";
            } else {
                if (!c.name.empty()) std::cout << " — " << c.name;
                std::cout << " (" << (c.source == ConverterDesc::Source::Repo ? "repo" : "local")
                          << ")";
            }
            std::cout << std::endl;
        }
    } else if (type == "output") {
        struct Label { const char* name; };
        Label labels[] = {
            { "Corrected interferograms from selected files" },
            { "Uncorrected interferograms from selected files" },
            { "Average spectrum" },
            { "SNR spectrum" },
            { "Spectra from selected files" },
            { "Allan-Werle 3D" },
            { "Allan-Werle slice" },
            { "100% T transmission line" },
            { "100% T lines for all files" },
            { "100% T standard deviation" },
        };
        for (const auto& l : labels) {
            std::cout << l.name << std::endl;
        }
    } else if (type == "recent") {
        std::string configFilePath = getConfigFilePath();
        AppConfig config;
        if (std::filesystem::exists(configFilePath)) {
            config.loadFromFile(configFilePath);
        }
        for (const auto& entry : config.recentDatasets) {
            std::cout << entry.path << std::endl;
        }
    } else {
        std::cerr << "Error: Unknown list type '" << type << "'. "
                  << "Available: converter, output, recent" << std::endl;
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// Reset (-r)
// ---------------------------------------------------------------------------
static void handleReset() {
    std::string configPath = getConfigFilePath();
    if (std::filesystem::exists(configPath)) {
        std::filesystem::remove(configPath);
        std::cout << "Removed " << configPath << std::endl;
    }
    std::string imguiPath = getImguiIniPath();
    if (std::filesystem::exists(imguiPath)) {
        std::filesystem::remove(imguiPath);
        std::cout << "Removed " << imguiPath << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Template (-t)
// ---------------------------------------------------------------------------
static void handleTemplate() {
    json tpl;

    auto setting = [&](json& parent, const std::string& key, const json& defaultValue, const std::string& comment) {
        parent[key] = json::object({
            {"_comment", comment},
            {"value", defaultValue}
        });
    };

    // spectrum section
    json spec;
    setting(spec, "refLaserWavelengthUm", 1.550, "Reference laser wavelength in micrometers. Default: 1.550");
    setting(spec, "zeroPadK", 0, "Zero-padding factor (0-16). Output bins = N*(K+1). 0 = no padding.");
    setting(spec, "apodizationWindow", "Rectangular", "Apodization window: Rectangular, Gauss, Triangular, NortonBeer, DolphChebyshev");
    setting(spec, "gaussSigma", 1.0f, "Gauss sigma fraction (1.0-3.0). Only used when apodizationWindow = Gauss.");
    setting(spec, "rectWidth", 1.0f, "Rectangular window width fraction (0.05-1.0). Only used when apodizationWindow = Rectangular.");
    setting(spec, "rectAsymMode", true, "Rectangular window mode (true=asymmetric, false=symmetric).");
    setting(spec, "nortonBeerFwhm", 1.5f, "Norton-Beer FWHM parameter (1.0-2.0 step 0.1).");
    setting(spec, "dolphChebyshevAttenuationDb", 60.0f, "Dolph-Chebyshev attenuation in dB (50-160, step 10).");
    setting(spec, "detectorSensitivityKVperW", 0.0f, "Detector sensitivity in kV/W. 0 = skip conversion.");
    setting(spec, "xCorrectionMethod", "Hilbert", "X-axis correction method: Hilbert, PeakFinding");
    setting(spec, "peakProminence", 0.02, "Peak prominence threshold (0.0-0.5). Only used when xCorrectionMethod = PeakFinding.");
    setting(spec, "xUnit", "cm-1", "X axis unit: cm-1, um, THz");
    setting(spec, "yScale", "lin", "Y scale: lin, log10, dB");
    setting(spec, "yAxisMode", "all", "Y axis mode: all, tight, force");
    setting(spec, "forcedYMin", 0.0, "Forced Y axis minimum. Only used when yAxisMode = force.");
    setting(spec, "forcedYMax", 1.0, "Forced Y axis maximum. Only used when yAxisMode = force.");
    tpl["spectrum"] = spec;

    // average section
    json avg;
    setting(avg, "xUnit", "cm-1", "X axis unit for average plot: cm-1, um, THz");
    setting(avg, "yScale", "lin", "Y scale for average plot: lin, log10, dB");
    setting(avg, "yAxisMode", "all", "Y axis mode: all, tight, force");
    setting(avg, "forcedYMin", 0.0, "Forced Y axis minimum for average.");
    setting(avg, "forcedYMax", 1.0, "Forced Y axis maximum for average.");
    tpl["average"] = avg;

    // snr section
    json snr;
    setting(snr, "xUnit", "cm-1", "X axis unit for SNR plot: cm-1, um, THz");
    setting(snr, "yScale", "lin", "Y scale for SNR plot: lin, log10");
    setting(snr, "yAxisMode", "all", "Y axis mode: all, tight, force");
    setting(snr, "forcedYMin", 0.0, "Forced Y axis minimum for SNR.");
    setting(snr, "forcedYMax", 1.0, "Forced Y axis maximum for SNR.");
    tpl["snr"] = snr;

    // allan section
    json allan;
    setting(allan, "xUnit", "um", "X axis unit for Allan plot: cm-1, um, THz");
    setting(allan, "wavelengthDecimation", 5, "Spectral decimation factor (1-50). Higher = faster but coarser.");
    setting(allan, "xRangeMinUm", 1.0, "Minimum X range for Allan calculation in um.");
    setting(allan, "xRangeMaxUm", 30.0, "Maximum X range for Allan calculation in um.");
    setting(allan, "calcBase", "100% T", "Calculation base: 100% T or Spectrum");
    setting(allan, "sliceIndex", 0, "Slice index for 2D view (0 = first wavelength bin).");
    tpl["allan"] = allan;

    // t100 section
    json t100;
    setting(t100, "xUnit", "cm-1", "X axis unit for 100% T: cm-1, um, THz");
    setting(t100, "referenceSource", "File", "Reference source: File, CSV, Average");
    setting(t100, "csvReferencePath", "", "Path to reference CSV file. Only used when referenceSource = CSV.");
    setting(t100, "yAxisMode", "all", "Y axis mode: all, tight, force");
    setting(t100, "forcedYMin", 0.0, "Forced Y axis minimum for 100% T.");
    setting(t100, "forcedYMax", 100.0, "Forced Y axis maximum for 100% T.");
    setting(t100, "energyRatioNumA", "", "Energy ratio A numerator (wavenumber or 'max').");
    setting(t100, "energyRatioDenA", "", "Energy ratio A denominator (wavenumber or 'max').");
    setting(t100, "energyRatioNumB", "", "Energy ratio B numerator (wavenumber or 'max').");
    setting(t100, "energyRatioDenB", "", "Energy ratio B denominator (wavenumber or 'max').");
    setting(t100, "energyRatioNumC", "", "Energy ratio C numerator (wavenumber or 'max').");
    setting(t100, "energyRatioDenC", "", "Energy ratio C denominator (wavenumber or 'max').");
    tpl["t100"] = t100;

    // processing section
    json proc;
    setting(proc, "workerThreads", -1, "Number of worker threads (-1 = AUTO, otherwise 1/2/4/8/16).");
    tpl["processing"] = proc;

    std::ofstream ofs("template.json");
    if (!ofs.is_open()) {
        std::cerr << "Error: Could not create template.json" << std::endl;
        exit(1);
    }
    ofs << tpl.dump(2) << std::endl;
    ofs.close();
    std::cout << "Created template.json" << std::endl;
}

// ---------------------------------------------------------------------------
// JSON config applier
// ---------------------------------------------------------------------------
static int jsonXUnitToInt(const std::string& s) {
    if (s == "cm-1") return 0;
    if (s == "um")   return 1;
    if (s == "THz")  return 2;
    return 0;
}
static int jsonYScaleToInt(const std::string& s) {
    if (s == "lin")   return 0;
    if (s == "log10") return 1;
    if (s == "dB")    return 2;
    return 0;
}
static int jsonYAxisModeToInt(const std::string& s) {
    if (s == "all")   return 0;
    if (s == "tight") return 1;
    if (s == "force") return 2;
    return 0;
}
static int jsonApodToInt(const std::string& s) {
    if (s == "Rectangular")    return 0;
    if (s == "Gauss")          return 1;
    if (s == "Triangular")     return 2;
    if (s == "NortonBeer")     return 3;
    if (s == "DolphChebyshev") return 4;
    return 0;
}
static int jsonCalcBaseToInt(const std::string& s) {
    if (s == "100% T")  return 0;
    if (s == "Spectrum") return 1;
    return 0;
}

template<typename T>
static T jsonVal(const json& parent, const std::string& key, T defaultVal) {
    if (!parent.contains(key)) return defaultVal;
    const auto& v = parent[key];
    if (v.is_object() && v.contains("value")) {
        return v["value"].get<T>();
    }
    return v.get<T>();
}

static void applyJsonConfig(AppState& state, const json& j) {
    // Spectrum settings
    if (j.contains("spectrum")) {
        const auto& s = j["spectrum"];
        state.active->spectrum.refLaserTextbox  = jsonVal<float>(s, "refLaserWavelengthUm", 1.550f);
        state.active->spectrum.Kpadding         = jsonVal<int>(s, "zeroPadK", 0);
        state.active->spectrum.apodizationSelector = jsonApodToInt(jsonVal<std::string>(s, "apodizationWindow", "Rectangular"));
        state.active->spectrum.apodizationParams.gaussSigma   = jsonVal<float>(s, "gaussSigma", 1.0f);
        state.active->spectrum.apodizationParams.rectWidth    = jsonVal<float>(s, "rectWidth", 1.0f);
        state.active->spectrum.apodizationParams.rectAsymMode = jsonVal<bool>(s, "rectAsymMode", true);
        state.active->spectrum.apodizationParams.nortonBeerFwhm = jsonVal<float>(s, "nortonBeerFwhm", 1.5f);
        state.active->spectrum.apodizationParams.dolphChebyshevAt = jsonVal<float>(s, "dolphChebyshevAttenuationDb", 60.0f);
        state.active->spectrum.detectorSensitivity = jsonVal<float>(s, "detectorSensitivityKVperW", 0.0f);
        // NOTE: comparison is case-sensitive — "PeakFinding" only, "peakfinding" silently falls back to Hilbert
        std::string xMethod = jsonVal<std::string>(s, "xCorrectionMethod", "Hilbert");
        state.active->xCorrectionMethod = (xMethod == "PeakFinding") ? 1 : 0;
        state.active->peakProminenceThreshold = jsonVal<float>(s, "peakProminence", 0.02f);
        state.active->spectrum.xUnitSelector   = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->spectrum.yScaleSelector  = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->spectrum.yAxisMode       = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->spectrum.forcedYMin      = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->spectrum.forcedYMax      = jsonVal<double>(s, "forcedYMax", 1.0);

        // Sync prev fields
        state.active->spectrum.prevXUnitSelector = state.active->spectrum.xUnitSelector;
        state.active->spectrum.prevYScaleSelector = state.active->spectrum.yScaleSelector;
        state.active->spectrum.prevYAxisMode = state.active->spectrum.yAxisMode;
    }

    // Average settings
    if (j.contains("average")) {
        const auto& s = j["average"];
        state.active->averageSpectrum.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->averageSpectrum.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->averageSpectrum.yAxisMode    = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->averageSpectrum.forcedYMin   = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->averageSpectrum.forcedYMax   = jsonVal<double>(s, "forcedYMax", 1.0);
        state.active->averageSpectrum.prevXUnitSelector = state.active->averageSpectrum.xUnitSelector;
        state.active->averageSpectrum.prevYScaleSelector = state.active->averageSpectrum.yScaleSelector;
        state.active->averageSpectrum.prevYAxisMode = state.active->averageSpectrum.yAxisMode;
    }

    // SNR settings
    if (j.contains("snr")) {
        const auto& s = j["snr"];
        state.active->snrSpectrum.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->snrSpectrum.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->snrSpectrum.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->snrSpectrum.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->snrSpectrum.forcedYMax    = jsonVal<double>(s, "forcedYMax", 1.0);
        state.active->snrSpectrum.prevXUnitSelector = state.active->snrSpectrum.xUnitSelector;
        state.active->snrSpectrum.prevYScaleSelector = state.active->snrSpectrum.yScaleSelector;
        state.active->snrSpectrum.prevYAxisMode = state.active->snrSpectrum.yAxisMode;
    }

    // Allan settings
    if (j.contains("allan")) {
        const auto& s = j["allan"];
        state.active->allanVariance.xUnitSelector      = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "um"));
        state.active->allanVariance.wavelengthDecimation = std::max(1, jsonVal<int>(s, "wavelengthDecimation", 5));
        state.active->allanVariance.xRangeMin           = jsonVal<double>(s, "xRangeMinUm", 1.0);
        state.active->allanVariance.xRangeMax           = jsonVal<double>(s, "xRangeMaxUm", 30.0);
        state.active->allanVariance.calcBaseSelector    = jsonCalcBaseToInt(jsonVal<std::string>(s, "calcBase", "100% T"));
        state.active->allanVariance.selectedSliceIndex  = jsonVal<int>(s, "sliceIndex", 0);
    }

    // T100 settings
    if (j.contains("t100")) {
        const auto& s = j["t100"];
        state.active->t100.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->t100.prevXUnitSelector = state.active->t100.xUnitSelector;
        state.active->t100.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->t100.prevYAxisMode = state.active->t100.yAxisMode;
        state.active->t100.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->t100.forcedYMax    = jsonVal<double>(s, "forcedYMax", 100.0);

        std::string refSrc = jsonVal<std::string>(s, "referenceSource", "File");
        if (refSrc == "CSV")     state.active->t100.referenceSource = 1;
        else if (refSrc == "Average") state.active->t100.referenceSource = 2;
        else                     state.active->t100.referenceSource = 0;

        std::string csvPath = jsonVal<std::string>(s, "csvReferencePath", "");
        if (!csvPath.empty()) {
            strncpy(state.active->t100.csvPathBuffer, csvPath.c_str(), sizeof(state.active->t100.csvPathBuffer) - 1);
            state.active->t100.csvPathBuffer[sizeof(state.active->t100.csvPathBuffer) - 1] = '\0';
        }

        auto setRatio = [](char* dst, size_t sz, const json& j, const std::string& key) {
            std::string v = jsonVal<std::string>(j, key, "");
            strncpy(dst, v.c_str(), sz - 1);
            dst[sz - 1] = '\0';
        };
        setRatio(state.active->t100.energyRatioNumA, 32, s, "energyRatioNumA");
        setRatio(state.active->t100.energyRatioDenA, 32, s, "energyRatioDenA");
        setRatio(state.active->t100.energyRatioNumB, 32, s, "energyRatioNumB");
        setRatio(state.active->t100.energyRatioDenB, 32, s, "energyRatioDenB");
        setRatio(state.active->t100.energyRatioNumC, 32, s, "energyRatioNumC");
        setRatio(state.active->t100.energyRatioDenC, 32, s, "energyRatioDenC");
    }

    // Processing settings
    if (j.contains("processing")) {
        const auto& s = j["processing"];
        int wt = jsonVal<int>(s, "workerThreads", -1);
        state.reconfigurePool(wt);
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Compute spectrum for a single file and cache it in appState
// ---------------------------------------------------------------------------
static bool computeSpectrumForFile(AppState& state, const std::string& filePath, const std::string& fileId) {
    return state.active->spectrum.computeAndCacheSpectrum(filePath, fileId);
}

// ---------------------------------------------------------------------------
// Poll a batch computation (Average/SNR) until done
// ---------------------------------------------------------------------------
template<typename T>
static void pollUntilDone(T& panel) {
    panel.startCalculation();
    while (panel.calcInProgress) {
        panel.tickCalculation();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---------------------------------------------------------------------------
// Shared compute + export tail for -p and -w.
// Verbatim move of handleProcess steps 8-9 (output-type switch + setupT100Reference
// + pollUntilDone + computeSpectrumForFile + exportArtifact). Reads everything it
// needs from appState (loading differs between the two commands).
// ---------------------------------------------------------------------------
static bool computeAndExport(const HeadlessConfig& cfg) {
    // 8. Compute requested output
    std::string ot = cfg.outputType;
    bool computedOk = false;

    // Helper lambda to set T100 reference based on config
    auto setupT100Reference = [&]() {
        if (appState.active->t100.referenceAvailable) return true;
        if (appState.active->t100.referenceSource == 1 && appState.active->t100.csvPathBuffer[0] != '\0') {
            appState.active->t100.setReferenceFromCSV(appState.active->t100.csvPathBuffer);
        } else if (appState.active->t100.referenceSource == 2) {
            if (!appState.active->averageSpectrum.averageAvailable) {
                pollUntilDone(appState.active->averageSpectrum);
            }
            appState.active->t100.setReferenceFromAverage();
        } else {
            appState.active->t100.setReferenceFromCurrentSpectrum();
        }
        return appState.active->t100.referenceAvailable;
    };

    if (ot == "Corrected interferograms from selected files" ||
        ot == "Uncorrected interferograms from selected files") {
        // IFG export just needs raw data loaded (already done)
        computedOk = true;
    } else if (ot == "Spectra from selected files") {
        // Compute spectra for all selected files
        computedOk = true;
        for (size_t i = 0; i < appState.active->selectedFilenames.size(); i++) {
            if (!computeSpectrumForFile(appState, appState.active->selectedFiles[i],
                                        appState.active->selectedFilenames[i])) {
                computedOk = false;
                break;
            }
        }
    } else if (ot == "Average spectrum") {
        pollUntilDone(appState.active->averageSpectrum);
        computedOk = appState.active->averageSpectrum.averageAvailable;
    } else if (ot == "SNR spectrum") {
        // SNR needs at least 2 files checked
        int checked = 0;
        for (size_t i = 0; i < appState.active->filesSelectedForAveraging.size(); i++)
            if (appState.active->filesSelectedForAveraging[i]) checked++;
        if (checked < 2) {
            std::cerr << "Error: SNR requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        pollUntilDone(appState.active->snrSpectrum);
        computedOk = appState.active->snrSpectrum.snrAvailable;
    } else if (ot == "Allan-Werle 3D" || ot == "Allan-Werle slice") {
        int checked = 0;
        for (size_t i = 0; i < appState.active->filesSelectedForAveraging.size(); i++)
            if (appState.active->filesSelectedForAveraging[i]) checked++;
        if (checked < 2) {
            std::cerr << "Error: Allan variance requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        // Pre-compute spectra for all checked files (Allan needs them)
        for (size_t i = 0; i < appState.active->sortedFiles.size(); i++) {
            if (i < appState.active->filesSelectedForAveraging.size() && appState.active->filesSelectedForAveraging[i]) {
                const auto& fp = appState.active->sortedFiles[i];
                std::string fid = fp;
                size_t ls = fid.find_last_of("/\\");
                if (ls != std::string::npos) fid = fid.substr(ls + 1);
                computeSpectrumForFile(appState, fp, fid);
            }
        }
        pollUntilDone(appState.active->allanVariance);
        computedOk = appState.active->allanVariance.allanAvailable;
    } else if (ot == "100% T transmission line" ||
               ot == "100% T lines for all files" ||
               ot == "100% T standard deviation") {
        // Pre-compute spectrum for the first file (needed by setReferenceFromCurrentSpectrum)
        if (!appState.active->selectedFiles.empty()) {
            std::string fid0 = appState.active->selectedFilenames[0];
            if (appState.active->spectrum.cachedSpectra.find(fid0) == appState.active->spectrum.cachedSpectra.end()) {
                computeSpectrumForFile(appState, appState.active->selectedFiles[0], fid0);
            }
        }
        // Set up reference
        if (!setupT100Reference()) {
            std::cerr << "Error: Failed to set 100% T reference" << std::endl;
            exit(1);
        }

        // Compute transmittance for selected files
        if (!appState.active->t100.transmittanceAvailable) {
            for (const auto& fp : appState.active->selectedFiles) {
                std::string fid = fp;
                size_t ls = fid.find_last_of("/\\");
                if (ls != std::string::npos) fid = fid.substr(ls + 1);
                auto it = appState.active->spectrum.cachedSpectra.find(fid);
                if (it == appState.active->spectrum.cachedSpectra.end()) {
                    computeSpectrumForFile(appState, fp, fid);
                }
            }
            appState.active->t100.lastKnownSelection = appState.active->selectedFilenames;
            appState.active->t100.needsRecompute = true;
            // Explicitly compute transmittance for each file
            for (const auto& fid : appState.active->t100.lastKnownSelection) {
                appState.active->t100.computeTransmittanceForFile(fid);
            }
            appState.active->t100.transmittanceAvailable = true;
        }

        if (ot == "100% T standard deviation") {
            if (!appState.active->t100.stddevAvailable) {
                appState.active->t100.startStdCalculation();
                while (appState.active->t100.calcStdInProgress) {
                    appState.active->t100.tickStdCalculation();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            computedOk = appState.active->t100.stddevAvailable;
        } else {
            computedOk = appState.active->t100.transmittanceAvailable;
        }
    } else {
        std::cerr << "Error: Unknown output type '" << ot << "'. "
                  << "Available: Corrected interferograms from selected files, "
                  << "Uncorrected interferograms from selected files, "
                  << "Average spectrum, SNR spectrum, Spectra from selected files, "
                  << "Allan-Werle 3D, Allan-Werle slice, "
                  << "100% T transmission line, 100% T lines for all files, "
                  << "100% T standard deviation" << std::endl;
        exit(1);
    }

    if (!computedOk) {
        std::cerr << "Error: Computation failed for output type '" << ot << "'" << std::endl;
        exit(1);
    }

    // 9. Export
    appState.active->exportPanel.exportArtifact(ot, cfg.outputDir);
    std::cout << "Exported '" << ot << "' to " << cfg.outputDir << std::endl;
    return true;
}
// ---------------------------------------------------------------------------
// Workspace (-w): open a .h5 workspace, compute the requested artifact into it
// (the panels mirror derivatives into Workspace when hasWorkspace()), save in
// place (atomic temp+rename), then export. Mirrors GUI Save minus the prompt:
// captureViewState -> markConfigStale -> pruneStale -> H5Store::save.
// ---------------------------------------------------------------------------
static void handleWorkspace(const HeadlessConfig& cfg) {
    // 1. Validate output directory and input file
    if (!std::filesystem::exists(cfg.outputDir)) {
        std::cerr << "Error: Directory '" << cfg.outputDir << "' does not exist" << std::endl;
        exit(1);
    }
    if (!std::filesystem::is_regular_file(cfg.path)) {
        std::cerr << "Error: Workspace file '" << cfg.path << "' not found" << std::endl;
        exit(1);
    }

    // 2. Set appState cross-references (same as handleProcess; needed for the
    //    panels' workspace mirrors). Headless runs without the tab machinery:
    //    one canonical session, wired and activated (M4.5 live-object model).
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = cfg.path;
    sess->path = cfg.path;
    wireSessionPanels(appState, *sess);
    appState.sessions.push_back(std::move(sess));
    appState.active = appState.sessions.back().get();
    appState.activeSessionIdx = static_cast<int>(appState.sessions.size()) - 1;
    appState.lastActiveSessionIdx = appState.activeSessionIdx;
    appState.activeTabKind = ActiveTabKind::Workspace;

    // Note: -w stays single-workspace only; .cross.h5 multi-workspace export
    // (P18, beyond committed scope) would land here as a new -w output type.
    // 3. Open workspace (loads, sets datasetInfo/csvFiles, applyViewState,
    //    seedPanels, AdapterRegistry::s_workspace, currentDatasetName)
    try {
        openWorkspace(appState, cfg.path);
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to open workspace '" << cfg.path << "': " << e.what() << std::endl;
        exit(1);
    }

    if (appState.active->csvFiles.empty()) {
        std::cerr << "Error: No members found in workspace '" << cfg.path << "'" << std::endl;
        exit(1);
    }

    // 4. Optional config override (overrides saved view state when given)
    json j;
    if (!cfg.configPath.empty()) {
        try {
            std::ifstream ifs(cfg.configPath);
            if (!ifs.is_open()) {
                std::cerr << "Error: Config file '" << cfg.configPath << "' not found" << std::endl;
                exit(1);
            }
            ifs >> j;
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid JSON in config file '" << cfg.configPath << "': " << e.what() << std::endl;
            exit(1);
        }
        applyJsonConfig(appState, j);
    }

    // 5. Natural-sort the member list. workspaceFileList is lexicographic
    //    (std::sort on member ids) — for >9 members raw_10 would sort before
    //    raw_2. The GUI frame loop and applyViewState natural-sort; re-sorting
    //    is idempotent (applyViewState already built a natural-sorted list).
    appState.active->sortedFiles = appState.active->csvFiles;
    std::sort(appState.active->sortedFiles.begin(), appState.active->sortedFiles.end(), naturalSortCompare);

    // 6. Load EVERY member (no GUI limit in headless mode): rawDataCache feeds
    //    the IFG CSV writers. loadFileStatic routes the "HDF5 Workspace"
    //    sentinel to workspaceRead (adapter_registry.cpp). Mirrors
    //    handleProcess step 6, incl. the downsampling pass.
    for (size_t i = 0; i < appState.active->sortedFiles.size(); i++) {
        try {
            const auto& filePath = appState.active->sortedFiles[i];
            InterferogramData data = workspaceRead(appState.active->workspace, filePath);
            appState.active->rawDataCache.push_back(data);

            InterferogramData processed = data;
            if (appState.active->enableDownsampling && processed.dataSize() > appState.maxPointsBeforeDownsampling) {
                size_t factor = processed.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                std::vector<double> downRef, downPrim;
                for (size_t j = 0; j < processed.referenceDetector.size(); j += factor) {
                    downRef.push_back(processed.referenceDetector[j]);
                    downPrim.push_back(processed.primaryDetector[j]);
                }
                processed.referenceDetector = downRef;
                processed.primaryDetector = downPrim;
            }

            appState.active->loadedData.push_back(processed);
            appState.active->selectedFiles.push_back(filePath);
            std::string fname = filePath;
            size_t ls = fname.find_last_of("/\\");
            if (ls != std::string::npos) fname = fname.substr(ls + 1);
            appState.active->selectedFilenames.push_back(fname);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to load " << appState.active->sortedFiles[i] << ": " << e.what() << std::endl;
            continue;
        }
    }

    if (appState.active->selectedFiles.empty()) {
        std::cerr << "Error: Failed to load any members from workspace" << std::endl;
        exit(1);
    }

    appState.active->dataLoaded = true;

    // Mark all files as checked for averaging
    appState.active->filesSelectedForAveraging.clear();
    appState.active->filesSelectedForAveraging.resize(appState.active->sortedFiles.size(), true);

    // 7. Shared compute + export tail (writes derivatives into the Workspace)
    computeAndExport(cfg);

    // 8. Save in place. markConfigStale is the GUI save's first step — without
    //    it pruneStale drops nothing (stale is RAM-only), so a config that
    //    changed params would leave mismatched members behind.
    captureViewState(appState);
    markConfigStale(appState.active->workspace, appState);
    try {
        H5Store::save(cfg.path, appState.active->workspace.pruneStale());
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to save workspace '" << cfg.path << "': " << e.what() << std::endl;
        exit(1);
    }
    appState.active->workspace.dirty = false;

    std::cout << "Saved " << cfg.path << std::endl;
}

// ---------------------------------------------------------------------------
// Convert (-c): run a converter on <input> -> <output.h5>, validate, exit 0/1.
// Resolves the converter by id from the scanned set, or accepts a direct .py
// path. Uses the local clone as-is — no implicit network (deterministic CI).
// ---------------------------------------------------------------------------
static void handleConvert(const HeadlessConfig& cfg) {
    AppConfig config;
    std::string configFilePath = getConfigFilePath();
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
    }
    std::string repoDir = config.converterRepoDir.empty()
        ? appDataDir() + "/converter-repo" : config.converterRepoDir;
    ConverterRegistry::instance().refresh(appDataDir() + "/converters",
                                          config.converterPaths, repoDir);

    const ConverterDesc* desc = ConverterRegistry::instance().get(cfg.converter);
    ConverterDesc direct;
    if (!desc && std::filesystem::is_regular_file(cfg.converter)) {
        direct = parseConverterFile(cfg.converter, false);
        desc = &direct;
    }
    if (!desc) {
        std::cerr << "Error: Unknown converter '" << cfg.converter << "' "
                  << "(use -l converter to list, or pass a .py path)" << std::endl;
        exit(1);
    }

    std::string log, error;
    if (!runConverterSync(*desc, config.converterInterpreter, cfg.path,
                          cfg.outputDir, {}, log, error)) {
        if (log.empty() && !error.empty()) log = error;
        if (!log.empty()) std::cout << log << std::endl;
        std::cerr << "Error: Converter '" << cfg.converter << "' failed" << std::endl;
        exit(1);
    }
    if (!log.empty()) std::cout << log << std::endl;

    try {
        H5Store::validate(cfg.outputDir);
    } catch (const std::exception& e) {
        std::cerr << "Error: Converted file failed validation: " << e.what() << std::endl;
        exit(1);
    }
    std::cout << "Converted '" << cfg.path << "' -> '" << cfg.outputDir
              << "' using " << desc->id << std::endl;
}

// ---------------------------------------------------------------------------
// Sync converters (-sync-converters): clone on first run, pull afterwards.
// ---------------------------------------------------------------------------
static void handleSyncConverters() {
    AppConfig config;
    std::string configFilePath = getConfigFilePath();
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
    }
    std::string repoDir = config.converterRepoDir.empty()
        ? appDataDir() + "/converter-repo" : config.converterRepoDir;
    std::string url = config.converterRepoUrl.empty()
        ? "https://github.com/jmnich/fts_data_explorer_converters" : config.converterRepoUrl;

    std::string error;
    if (!ensureConverterRepo(url, repoDir, error)) {
        std::cerr << "Error: " << error << std::endl;
        exit(1);
    }
    std::cout << "Converters synced to " << repoDir << std::endl;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool parseHeadlessArgs(int argc, char* argv[], HeadlessConfig& cfg) {
    if (argc < 2) {
        return false; // No flags → normal mode
    }

    std::string flag = argv[1];

    if (flag == "-help") {
        if (argc != 2) {
            std::cerr << "Error: -help takes no arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Help;
        return false;
    }

    if (flag == "-v") {
        if (argc != 2) {
            std::cerr << "Error: -v takes no arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Version;
        return false;
    }

    if (flag == "-l") {
        cfg.command = HeadlessConfig::Command::List;
        if (argc >= 3) {
            cfg.listType = argv[2];
            if (argc > 3) {
                std::cerr << "Error: -l takes at most one argument" << std::endl;
                return true;
            }
        }
        return false;
    }


    if (flag == "-w") {
        if (argc < 5 || argc > 6) {
            std::cerr << "Error: -w requires <workspace.h5> <output type> <output dir> [<config>]"
                      << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Workspace;
        cfg.path = argv[2];
        cfg.outputType = argv[3];
        cfg.outputDir = argv[4];
        if (argc == 6) cfg.configPath = argv[5];
        return false;
    }

    if (flag == "-c") {
        if (argc != 5) {
            std::cerr << "Error: -c requires <converter> <input> <output.h5>" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Convert;
        cfg.converter = argv[2];
        cfg.path = argv[3];
        cfg.outputDir = argv[4];
        return false;
    }

    if (flag == "-sync-converters") {
        if (argc != 2) {
            std::cerr << "Error: -sync-converters takes no arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::SyncConverters;
        return false;
    }

    if (flag == "-t") {
        if (argc != 2) {
            std::cerr << "Error: -t takes no arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Template;
        return false;
    }

    if (flag == "-r") {
        if (argc != 2) {
            std::cerr << "Error: -r takes no arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Reset;
        return false;
    }

    std::cerr << "Error: Unknown flag '" << flag << "'" << std::endl;
    return true;
}

bool runHeadlessCommand(const HeadlessConfig& cfg) {
    switch (cfg.command) {
        case HeadlessConfig::Command::Help:
            handleHelp();
            return true;
        case HeadlessConfig::Command::Version:
            handleVersion();
            return true;
        case HeadlessConfig::Command::List:
            handleList(cfg.listType);
            return true;
        case HeadlessConfig::Command::Reset:
            handleReset();
            return true;
        case HeadlessConfig::Command::Template:
            handleTemplate();
            return true;
        case HeadlessConfig::Command::Workspace:
            handleWorkspace(cfg);
            return true;
        case HeadlessConfig::Command::Convert:
            handleConvert(cfg);
            return true;
        case HeadlessConfig::Command::SyncConverters:
            handleSyncConverters();
            return true;
        case HeadlessConfig::Command::None:
            return false;
    }
    return false;
}
