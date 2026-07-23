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
#include "adapters/adapter_registry.h"
#include "version.h"
#include "adapters/wust_mini_fts_adapter.h"
#include "adapters/arcoptix_igms_adapter.h"
#include "adapters/arcoptix_spectra_adapter.h"
#include "export.h"
#include "apodization.h"
#include "spectral_toolbox.h"

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
              << "  -l [type]      List available options. Types: data_adapter, output, recent.\n"
              << "  -o <path> <adapter>\n"
              << "                 Open GUI with dataset at <path> using <adapter>.\n"
              << "  -p <path> <adapter> <config> <output type> <output dir>\n"
              << "                 Process data in headless mode and export results.\n"
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
        std::cout << "Available list types: data_adapter, output, recent" << std::endl;
        return;
    }

    if (type == "data_adapter") {
        AdapterRegistry::instance().registerAdapter(std::make_unique<WustMiniFtsAdapter>());
        AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixIgmsAdapter>());
        AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixSpectraAdapter>());

        for (const auto& a : AdapterRegistry::instance().getAll()) {
            std::cout << a->getName() << std::endl;
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
            std::cout << entry.path;
            if (!entry.adapterName.empty()) {
                std::cout << " (" << entry.adapterName << ")";
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "Error: Unknown list type '" << type << "'. "
                  << "Available: data_adapter, output, recent" << std::endl;
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
        state.spectrum.refLaserTextbox  = jsonVal<float>(s, "refLaserWavelengthUm", 1.550f);
        state.spectrum.Kpadding         = jsonVal<int>(s, "zeroPadK", 0);
        state.spectrum.apodizationSelector = jsonApodToInt(jsonVal<std::string>(s, "apodizationWindow", "Rectangular"));
        state.spectrum.apodizationParams.gaussSigma   = jsonVal<float>(s, "gaussSigma", 1.0f);
        state.spectrum.apodizationParams.rectWidth    = jsonVal<float>(s, "rectWidth", 1.0f);
        state.spectrum.apodizationParams.rectAsymMode = jsonVal<bool>(s, "rectAsymMode", true);
        state.spectrum.apodizationParams.nortonBeerFwhm = jsonVal<float>(s, "nortonBeerFwhm", 1.5f);
        state.spectrum.apodizationParams.dolphChebyshevAt = jsonVal<float>(s, "dolphChebyshevAttenuationDb", 60.0f);
        state.spectrum.detectorSensitivity = jsonVal<float>(s, "detectorSensitivityKVperW", 0.0f);
        // NOTE: comparison is case-sensitive — "PeakFinding" only, "peakfinding" silently falls back to Hilbert
        std::string xMethod = jsonVal<std::string>(s, "xCorrectionMethod", "Hilbert");
        state.xCorrectionMethod = (xMethod == "PeakFinding") ? 1 : 0;
        state.peakProminenceThreshold = jsonVal<float>(s, "peakProminence", 0.02f);
        state.spectrum.xUnitSelector   = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.spectrum.yScaleSelector  = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.spectrum.yAxisMode       = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.spectrum.forcedYMin      = jsonVal<double>(s, "forcedYMin", 0.0);
        state.spectrum.forcedYMax      = jsonVal<double>(s, "forcedYMax", 1.0);

        // Sync prev fields
        state.spectrum.prevXUnitSelector = state.spectrum.xUnitSelector;
        state.spectrum.prevYScaleSelector = state.spectrum.yScaleSelector;
        state.spectrum.prevYAxisMode = state.spectrum.yAxisMode;
    }

    // Average settings
    if (j.contains("average")) {
        const auto& s = j["average"];
        state.averageSpectrum.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.averageSpectrum.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.averageSpectrum.yAxisMode    = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.averageSpectrum.forcedYMin   = jsonVal<double>(s, "forcedYMin", 0.0);
        state.averageSpectrum.forcedYMax   = jsonVal<double>(s, "forcedYMax", 1.0);
        state.averageSpectrum.prevXUnitSelector = state.averageSpectrum.xUnitSelector;
        state.averageSpectrum.prevYScaleSelector = state.averageSpectrum.yScaleSelector;
        state.averageSpectrum.prevYAxisMode = state.averageSpectrum.yAxisMode;
    }

    // SNR settings
    if (j.contains("snr")) {
        const auto& s = j["snr"];
        state.snrSpectrum.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.snrSpectrum.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.snrSpectrum.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.snrSpectrum.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.snrSpectrum.forcedYMax    = jsonVal<double>(s, "forcedYMax", 1.0);
        state.snrSpectrum.prevXUnitSelector = state.snrSpectrum.xUnitSelector;
        state.snrSpectrum.prevYScaleSelector = state.snrSpectrum.yScaleSelector;
        state.snrSpectrum.prevYAxisMode = state.snrSpectrum.yAxisMode;
    }

    // Allan settings
    if (j.contains("allan")) {
        const auto& s = j["allan"];
        state.allanVariance.xUnitSelector      = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "um"));
        state.allanVariance.wavelengthDecimation = jsonVal<int>(s, "wavelengthDecimation", 5);
        state.allanVariance.xRangeMin           = jsonVal<double>(s, "xRangeMinUm", 1.0);
        state.allanVariance.xRangeMax           = jsonVal<double>(s, "xRangeMaxUm", 30.0);
        state.allanVariance.calcBaseSelector    = jsonCalcBaseToInt(jsonVal<std::string>(s, "calcBase", "100% T"));
        state.allanVariance.selectedSliceIndex  = jsonVal<int>(s, "sliceIndex", 0);
    }

    // T100 settings
    if (j.contains("t100")) {
        const auto& s = j["t100"];
        state.t100.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.t100.prevXUnitSelector = state.t100.xUnitSelector;
        state.t100.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.t100.prevYAxisMode = state.t100.yAxisMode;
        state.t100.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.t100.forcedYMax    = jsonVal<double>(s, "forcedYMax", 100.0);

        std::string refSrc = jsonVal<std::string>(s, "referenceSource", "File");
        if (refSrc == "CSV")     state.t100.referenceSource = 1;
        else if (refSrc == "Average") state.t100.referenceSource = 2;
        else                     state.t100.referenceSource = 0;

        std::string csvPath = jsonVal<std::string>(s, "csvReferencePath", "");
        if (!csvPath.empty()) {
            strncpy(state.t100.csvPathBuffer, csvPath.c_str(), sizeof(state.t100.csvPathBuffer) - 1);
            state.t100.csvPathBuffer[sizeof(state.t100.csvPathBuffer) - 1] = '\0';
        }

        auto setRatio = [](char* dst, size_t sz, const json& j, const std::string& key) {
            std::string v = jsonVal<std::string>(j, key, "");
            strncpy(dst, v.c_str(), sz - 1);
            dst[sz - 1] = '\0';
        };
        setRatio(state.t100.energyRatioNumA, 32, s, "energyRatioNumA");
        setRatio(state.t100.energyRatioDenA, 32, s, "energyRatioDenA");
        setRatio(state.t100.energyRatioNumB, 32, s, "energyRatioNumB");
        setRatio(state.t100.energyRatioDenB, 32, s, "energyRatioDenB");
        setRatio(state.t100.energyRatioNumC, 32, s, "energyRatioNumC");
        setRatio(state.t100.energyRatioDenC, 32, s, "energyRatioDenC");
    }

    // Processing settings
    if (j.contains("processing")) {
        const auto& s = j["processing"];
        int wt = jsonVal<int>(s, "workerThreads", -1);
        state.reconfigurePool(wt);
    }
}

// ---------------------------------------------------------------------------
// Natural sort (replicated from main.cpp since it's static there)
// ---------------------------------------------------------------------------
static bool naturalSortCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (!std::isdigit(a[i]) || !std::isdigit(b[j])) {
            if (a[i] != b[j]) return a[i] < b[j];
            i++; j++;
        } else {
            size_t numStartA = i;
            size_t numStartB = j;
            while (i < a.size() && std::isdigit(a[i])) i++;
            while (j < b.size() && std::isdigit(b[j])) j++;
            int numA = std::stoi(a.substr(numStartA, i - numStartA));
            int numB = std::stoi(b.substr(numStartB, j - numStartB));
            if (numA != numB) return numA < numB;
        }
    }
    return a.size() < b.size();
}

// ---------------------------------------------------------------------------
// Compute spectrum for a single file and cache it in appState
// ---------------------------------------------------------------------------
static bool computeSpectrumForFile(AppState& state, const std::string& filePath, const std::string& fileId) {
    return state.spectrum.computeAndCacheSpectrum(filePath, fileId);
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
// Process (-p)
// ---------------------------------------------------------------------------
static void handleProcess(const HeadlessConfig& cfg) {
    // Validate output directory
    if (!std::filesystem::exists(cfg.outputDir)) {
        std::cerr << "Error: Directory '" << cfg.outputDir << "' does not exist" << std::endl;
        exit(1);
    }

    // 1. Load existing config for worker thread setting
    std::string configFilePath = getConfigFilePath();
    AppConfig config;
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
    }

    // 2. Register adapters
    AdapterRegistry::instance().registerAdapter(std::make_unique<WustMiniFtsAdapter>());
    AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixIgmsAdapter>());
    AdapterRegistry::instance().registerAdapter(std::make_unique<ArcoptixSpectraAdapter>());

    if (!AdapterRegistry::instance().getAdapter(cfg.adapter)) {
        std::cerr << "Error: Unknown adapter '" << cfg.adapter << "'" << std::endl;
        exit(1);
    }

    // 3. Set appState cross-references
    appState.spectrum.appState = &appState;
    appState.averageSpectrum.appState = &appState;
    appState.snrSpectrum.appState = &appState;
    appState.allanVariance.appState = &appState;
    appState.t100.appState = &appState;
    appState.exportPanel.appState = &appState;

    // 4. Configure thread pool
    appState.reconfigurePool(config.workerThreads);

    // 5. Apply adapter selection (this populates csvFiles, sets datasetInfo, etc.)
    appState.currentDirectory = cfg.path;
    applyAdapterSelection(cfg.adapter, cfg.path);

    if (appState.csvFiles.empty()) {
        std::cerr << "Error: No files found in dataset at '" << cfg.path << "'" << std::endl;
        exit(1);
    }

    // 6. Sort files and load
    appState.sortedFiles = appState.csvFiles;
    std::sort(appState.sortedFiles.begin(), appState.sortedFiles.end(), naturalSortCompare);

    // Set dataset name from directory
    std::string dirPath = cfg.path;
    size_t lastSlash = dirPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        appState.currentDatasetName = dirPath.substr(lastSlash + 1);
    } else {
        appState.currentDatasetName = dirPath;
    }

    // Load all files as selected (no limit in headless mode)
    size_t maxSel = appState.sortedFiles.size();
    for (size_t i = 0; i < appState.sortedFiles.size() && i < maxSel; i++) {
        try {
            const auto& filePath = appState.sortedFiles[i];
            InterferogramData data = appState.currentAdapter->loadFile(filePath);
            appState.rawDataCache.push_back(data);

            InterferogramData processed = data;
            if (appState.enableDownsampling && processed.dataSize() > appState.maxPointsBeforeDownsampling) {
                size_t factor = processed.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                std::vector<double> downRef, downPrim;
                for (size_t j = 0; j < processed.referenceDetector.size(); j += factor) {
                    downRef.push_back(processed.referenceDetector[j]);
                    downPrim.push_back(processed.primaryDetector[j]);
                }
                processed.referenceDetector = downRef;
                processed.primaryDetector = downPrim;
            }

            appState.loadedData.push_back(processed);
            appState.selectedFiles.push_back(filePath);
            std::string fname = filePath;
            size_t ls = fname.find_last_of("/\\");
            if (ls != std::string::npos) fname = fname.substr(ls + 1);
            appState.selectedFilenames.push_back(fname);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to load " << appState.sortedFiles[i] << ": " << e.what() << std::endl;
            continue;
        }
    }

    if (appState.selectedFiles.empty()) {
        std::cerr << "Error: Failed to load any files from dataset" << std::endl;
        exit(1);
    }

    appState.dataLoaded = true;

    // Mark all files as checked for averaging
    appState.filesSelectedForAveraging.clear();
    appState.filesSelectedForAveraging.resize(appState.sortedFiles.size(), true);

    // 7. Parse and apply JSON config
    json j;
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

    // 8. Compute requested output
    std::string ot = cfg.outputType;
    bool computedOk = false;

    // Helper lambda to set T100 reference based on config
    auto setupT100Reference = [&]() {
        if (appState.t100.referenceAvailable) return true;
        if (appState.t100.referenceSource == 1 && appState.t100.csvPathBuffer[0] != '\0') {
            appState.t100.setReferenceFromCSV(appState.t100.csvPathBuffer);
        } else if (appState.t100.referenceSource == 2) {
            if (!appState.averageSpectrum.averageAvailable) {
                pollUntilDone(appState.averageSpectrum);
            }
            appState.t100.setReferenceFromAverage();
        } else {
            appState.t100.setReferenceFromCurrentSpectrum();
        }
        return appState.t100.referenceAvailable;
    };

    if (ot == "Corrected interferograms from selected files" ||
        ot == "Uncorrected interferograms from selected files") {
        // IFG export just needs raw data loaded (already done)
        computedOk = true;
    } else if (ot == "Spectra from selected files") {
        // Compute spectra for all selected files
        computedOk = true;
        for (size_t i = 0; i < appState.selectedFilenames.size(); i++) {
            if (!computeSpectrumForFile(appState, appState.selectedFiles[i],
                                        appState.selectedFilenames[i])) {
                computedOk = false;
                break;
            }
        }
    } else if (ot == "Average spectrum") {
        pollUntilDone(appState.averageSpectrum);
        computedOk = appState.averageSpectrum.averageAvailable;
    } else if (ot == "SNR spectrum") {
        // SNR needs at least 2 files checked
        int checked = 0;
        for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
            if (appState.filesSelectedForAveraging[i]) checked++;
        if (checked < 2) {
            std::cerr << "Error: SNR requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        pollUntilDone(appState.snrSpectrum);
        computedOk = appState.snrSpectrum.snrAvailable;
    } else if (ot == "Allan-Werle 3D" || ot == "Allan-Werle slice") {
        int checked = 0;
        for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
            if (appState.filesSelectedForAveraging[i]) checked++;
        if (checked < 2) {
            std::cerr << "Error: Allan variance requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        // Pre-compute spectra for all checked files (Allan needs them)
        for (size_t i = 0; i < appState.sortedFiles.size(); i++) {
            if (i < appState.filesSelectedForAveraging.size() && appState.filesSelectedForAveraging[i]) {
                const auto& fp = appState.sortedFiles[i];
                std::string fid = fp;
                size_t ls = fid.find_last_of("/\\");
                if (ls != std::string::npos) fid = fid.substr(ls + 1);
                computeSpectrumForFile(appState, fp, fid);
            }
        }
        pollUntilDone(appState.allanVariance);
        computedOk = appState.allanVariance.allanAvailable;
    } else if (ot == "100% T transmission line" ||
               ot == "100% T lines for all files" ||
               ot == "100% T standard deviation") {
        // Pre-compute spectrum for the first file (needed by setReferenceFromCurrentSpectrum)
        if (!appState.selectedFilenames.empty()) {
            std::string fid0 = appState.selectedFilenames[0];
            if (appState.spectrum.cachedSpectra.find(fid0) == appState.spectrum.cachedSpectra.end()) {
                computeSpectrumForFile(appState, appState.selectedFiles[0], fid0);
            }
        }
        // Set up reference
        if (!setupT100Reference()) {
            std::cerr << "Error: Failed to set 100% T reference" << std::endl;
            exit(1);
        }

        // Compute transmittance for selected files
        if (!appState.t100.transmittanceAvailable) {
            for (const auto& fp : appState.selectedFiles) {
                std::string fid = fp;
                size_t ls = fid.find_last_of("/\\");
                if (ls != std::string::npos) fid = fid.substr(ls + 1);
                auto it = appState.spectrum.cachedSpectra.find(fid);
                if (it == appState.spectrum.cachedSpectra.end()) {
                    computeSpectrumForFile(appState, fp, fid);
                }
            }
            appState.t100.lastKnownSelection = appState.selectedFilenames;
            appState.t100.needsRecompute = true;
            // Explicitly compute transmittance for each file
            for (const auto& fid : appState.t100.lastKnownSelection) {
                appState.t100.computeTransmittanceForFile(fid);
            }
            appState.t100.transmittanceAvailable = true;
        }

        if (ot == "100% T standard deviation") {
            if (!appState.t100.stddevAvailable) {
                appState.t100.startStdCalculation();
                while (appState.t100.calcStdInProgress) {
                    appState.t100.tickStdCalculation();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            computedOk = appState.t100.stddevAvailable;
        } else {
            computedOk = appState.t100.transmittanceAvailable;
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
    appState.exportPanel.exportArtifact(ot, cfg.outputDir);
    std::cout << "Exported '" << ot << "' to " << cfg.outputDir << std::endl;
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

    if (flag == "-o") {
        if (argc != 4) {
            std::cerr << "Error: -o requires <path> and <adapter> arguments" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::OpenGUI;
        cfg.path = argv[2];
        cfg.adapter = argv[3];
        return false;
    }

    if (flag == "-p") {
        if (argc != 7) {
            std::cerr << "Error: -p requires <path> <adapter> <config> <output type> <output dir>" << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Process;
        cfg.path = argv[2];
        cfg.adapter = argv[3];
        cfg.configPath = argv[4];
        cfg.outputType = argv[5];
        cfg.outputDir = argv[6];
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
        case HeadlessConfig::Command::Process:
            handleProcess(cfg);
            return true;
        case HeadlessConfig::Command::OpenGUI:
        case HeadlessConfig::Command::None:
            return false;
    }
    return false;
}
