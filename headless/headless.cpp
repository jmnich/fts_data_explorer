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
    return appDataDir() + "/imgui.ini";
}

// Directory-stripped basename (Windows member ids can carry '\'). Used in the
// member-load loops and the (removed) pre-compute loops — see F14.
static std::string basenameOf(const std::string& p) {
    size_t ls = p.find_last_of("/\\");
    return (ls == std::string::npos) ? p : p.substr(ls + 1);
}

// Number of successfully LOADED members. F10: the SNR/Allan "≥2 files" checks
// must count loaded members (selectedFiles), not the checkbox vector
// (filesSelectedForAveraging is all-true by construction in headless, so it
// would pass even when most members failed to load).
static int countCheckedFiles() {
    return static_cast<int>(appState.active->selectedFiles.size());
}

// "Load AppConfig + resolve converterRepoDir" — duplicated 3× in handleList /
// handleConvert / handleSyncConverters (F14). The returned config keeps the
// other persisted fields (converterPaths, recentDatasets, interpreter) alive.
static AppConfig loadAppConfigWithRepoDir(std::string& repoDir) {
    AppConfig config;
    std::string configFilePath = getConfigFilePath();
    if (std::filesystem::exists(configFilePath)) {
        config.loadFromFile(configFilePath);
    }
    repoDir = config.converterRepoDir.empty()
        ? appDataDir() + "/converter-repo" : config.converterRepoDir;
    return config;
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
              << "                 NOTE: 'Allan-Werle 3D'/'Allan-Werle slice' export one\n"
              << "                 wavelengthDecimation-th of the wavelength bins by\n"
              << "                 default (decimation 5, X range 1-30 um — the GUI\n"
              << "                 defaults); set config allan.wavelengthDecimation=1 for\n"
              << "                 full resolution.\n"
              << "  -c <converter> <input> <output.h5>\n"
              << "                 Run a converter (<id> from -l converter, or a direct\n"
              << "                 .py path) on <input>, validate the result, exit 0/1.\n"
              << "                 Uses the local clone as-is (no implicit network).\n"
              << "  -cmp <sample.h5> <reference.h5> <output type> <output dir> [<config.json>]\n"
              << "                 Compare two workspaces: compute the average spectrum\n"
              << "                 from each and export BOTH curves on a shared axis\n"
              << "                 (sample interpolated onto the reference grid) — the\n"
              << "                 same view the UI Comparator overlay shows. The\n"
              << "                 optional processing config applies to BOTH workspaces.\n"
              << "                 Output type: Comparator spectra.\n"
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
        std::string repoDir;
        AppConfig config = loadAppConfigWithRepoDir(repoDir);
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
            { "Absorbance from selected files" },
            { "Transmittance from selected files" },
        };
        for (const auto& l : labels) {
            std::cout << l.name << std::endl;
        }
    } else if (type == "recent") {
        std::string repoDir;
        AppConfig config = loadAppConfigWithRepoDir(repoDir);
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
    // Also remove per-tab-type and per-workspace layout snapshots + .sel
    // sidecars that live next to imgui.ini in appDataDir.
    std::filesystem::path dir = std::filesystem::path(imguiPath).parent_path();
    if (!dir.empty() && std::filesystem::exists(dir)) {
        const std::string prefix = "imgui.ini.layout.";
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            const std::string fname = entry.path().filename().string();
            if (fname.rfind(prefix, 0) == 0) {
                std::filesystem::remove(entry.path());
            }
        }
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
// Enum-string helpers. F9: unknown strings must NOT silently default to index
// 0 — a typo in the config would silently change the computation. Warn on
// stderr and fall back to the documented default.
static int jsonXUnitToInt(const std::string& s) {
    if (s == "cm-1") return 0;
    if (s == "um")   return 1;
    if (s == "THz")  return 2;
    std::cerr << "Warning: unknown xUnit '" << s << "' — using cm-1" << std::endl;
    return 0;
}
static int jsonYScaleToInt(const std::string& s) {
    if (s == "lin")   return 0;
    if (s == "log10") return 1;
    if (s == "dB")    return 2;
    std::cerr << "Warning: unknown yScale '" << s << "' — using lin" << std::endl;
    return 0;
}
static int jsonYAxisModeToInt(const std::string& s) {
    if (s == "all")   return 0;
    if (s == "tight") return 1;
    if (s == "force") return 2;
    std::cerr << "Warning: unknown yAxisMode '" << s << "' — using all" << std::endl;
    return 0;
}
static int jsonApodToInt(const std::string& s) {
    if (s == "Rectangular")    return 0;
    if (s == "Gauss")          return 1;
    if (s == "Triangular")     return 2;
    if (s == "NortonBeer")     return 3;
    if (s == "DolphChebyshev") return 4;
    std::cerr << "Warning: unknown apodizationWindow '" << s
              << "' — using Rectangular" << std::endl;
    return 0;
}
static int jsonCalcBaseToInt(const std::string& s) {
    if (s == "100% T")  return 0;
    if (s == "Spectrum") return 1;
    std::cerr << "Warning: unknown calcBase '" << s << "' — using 100% T" << std::endl;
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
        // F9: clamp to the GUI ranges (spectrum.cpp InputInt/SliderFloat
        // bounds) — an out-of-range config must not silently change the math.
        state.active->spectrum.Kpadding         = std::clamp(jsonVal<int>(s, "zeroPadK", 0), 0, 16);
        state.active->spectrum.apodizationSelector = jsonApodToInt(jsonVal<std::string>(s, "apodizationWindow", "Rectangular"));
        state.active->spectrum.apodizationParams.gaussSigma   = std::clamp(jsonVal<float>(s, "gaussSigma", 1.0f), 1.0f, 3.0f);
        state.active->spectrum.apodizationParams.rectWidth    = std::clamp(jsonVal<float>(s, "rectWidth", 1.0f), 0.05f, 1.0f);
        state.active->spectrum.apodizationParams.rectAsymMode = jsonVal<bool>(s, "rectAsymMode", true);
        state.active->spectrum.apodizationParams.nortonBeerFwhm = std::clamp(jsonVal<float>(s, "nortonBeerFwhm", 1.5f), 1.0f, 2.0f);
        state.active->spectrum.apodizationParams.dolphChebyshevAt = std::clamp(jsonVal<float>(s, "dolphChebyshevAttenuationDb", 60.0f), 50.0f, 160.0f);
        state.active->spectrum.detectorSensitivity = jsonVal<float>(s, "detectorSensitivityKVperW", 0.0f);
        // F12: case-insensitive match — "peakfinding"/"peakFinding" must not
        // silently fall back to Hilbert.
        std::string xMethod = jsonVal<std::string>(s, "xCorrectionMethod", "Hilbert");
        std::transform(xMethod.begin(), xMethod.end(), xMethod.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        state.active->xCorrectionMethod = (xMethod == "peakfinding") ? 1 : 0;
        state.active->peakProminenceThreshold = std::clamp(jsonVal<float>(s, "peakProminence", 0.02f), 0.0f, 0.5f);
        state.active->spectrum.plot.xUnitSelector   = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->spectrum.plot.yScaleSelector  = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->spectrum.plot.yAxisMode       = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->spectrum.plot.forcedYMin      = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->spectrum.plot.forcedYMax      = jsonVal<double>(s, "forcedYMax", 1.0);

        // Sync prev fields
        state.active->spectrum.plot.prevXUnitSelector = state.active->spectrum.plot.xUnitSelector;
        state.active->spectrum.plot.prevYScaleSelector = state.active->spectrum.plot.yScaleSelector;
        state.active->spectrum.plot.prevYAxisMode = state.active->spectrum.plot.yAxisMode;

        // The config applies AFTER openWorkspace's seedPanelsFromWorkspace
        // restored caches from a saved member matching the SAVED params. The
        // applied params may differ, leaving the seeded caches stale — drop
        // them so every downstream ensure (writeSpectraCsv, the T100 pipeline)
        // recomputes with the config's params instead of silently reusing the
        // saved spectra (test3 K1/K4 regression).
        state.active->spectrum.cachedSpectra.clear();
        state.active->spectrum.cachedFrequencies.clear();
        state.active->spectrum.lastPrimaryDetectors.clear();
        state.active->spectrum.lastSpectrumParams.clear();
        state.active->spectrum.pendingSpectra_.clear();
    }

    // Average settings
    if (j.contains("average")) {
        const auto& s = j["average"];
        state.active->averageSpectrum.plot.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->averageSpectrum.plot.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->averageSpectrum.plot.yAxisMode    = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->averageSpectrum.plot.forcedYMin   = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->averageSpectrum.plot.forcedYMax   = jsonVal<double>(s, "forcedYMax", 1.0);
        state.active->averageSpectrum.plot.prevXUnitSelector = state.active->averageSpectrum.plot.xUnitSelector;
        state.active->averageSpectrum.plot.prevYScaleSelector = state.active->averageSpectrum.plot.yScaleSelector;
        state.active->averageSpectrum.plot.prevYAxisMode = state.active->averageSpectrum.plot.yAxisMode;
    }

    // SNR settings
    if (j.contains("snr")) {
        const auto& s = j["snr"];
        state.active->snrSpectrum.plot.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->snrSpectrum.plot.yScaleSelector = jsonYScaleToInt(jsonVal<std::string>(s, "yScale", "lin"));
        state.active->snrSpectrum.plot.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->snrSpectrum.plot.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->snrSpectrum.plot.forcedYMax    = jsonVal<double>(s, "forcedYMax", 1.0);
        state.active->snrSpectrum.plot.prevXUnitSelector = state.active->snrSpectrum.plot.xUnitSelector;
        state.active->snrSpectrum.plot.prevYScaleSelector = state.active->snrSpectrum.plot.yScaleSelector;
        state.active->snrSpectrum.plot.prevYAxisMode = state.active->snrSpectrum.plot.yAxisMode;
    }

    // Allan settings
    if (j.contains("allan")) {
        const auto& s = j["allan"];
        state.active->allanVariance.xUnitSelector      = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "um"));
        state.active->allanVariance.wavelengthDecimation = std::clamp(jsonVal<int>(s, "wavelengthDecimation", 5), 1, 50);
        state.active->allanVariance.xRangeMin           = jsonVal<double>(s, "xRangeMinUm", 1.0);
        state.active->allanVariance.xRangeMax           = jsonVal<double>(s, "xRangeMaxUm", 30.0);
        state.active->allanVariance.calcBaseSelector    = jsonCalcBaseToInt(jsonVal<std::string>(s, "calcBase", "100% T"));
        state.active->allanVariance.selectedSliceIndex  = jsonVal<int>(s, "sliceIndex", 0);
    }

    // T100 settings
    if (j.contains("t100")) {
        const auto& s = j["t100"];
        state.active->t100.plot.xUnitSelector = jsonXUnitToInt(jsonVal<std::string>(s, "xUnit", "cm-1"));
        state.active->t100.plot.prevXUnitSelector = state.active->t100.plot.xUnitSelector;
        state.active->t100.plot.yAxisMode     = jsonYAxisModeToInt(jsonVal<std::string>(s, "yAxisMode", "all"));
        state.active->t100.plot.prevYAxisMode = state.active->t100.plot.yAxisMode;
        state.active->t100.plot.forcedYMin    = jsonVal<double>(s, "forcedYMin", 0.0);
        state.active->t100.plot.forcedYMax    = jsonVal<double>(s, "forcedYMax", 100.0);

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

// F5a: the CSV writers swallow write failures and early-return on empty
// selections — "Exported ..." must not be printed when nothing was written.
// Snapshot the newest .csv mtime in the output dir before exporting; after the
// export at least one .csv must be newer.
static std::filesystem::file_time_type newestCsvMtime(const std::string& dir) {
    auto newest = std::filesystem::file_time_type::min();
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const std::string& p = e.path().string();
        if (p.size() < 4 || p.compare(p.size() - 4, 4, ".csv") != 0) continue;
        newest = std::max(newest, e.last_write_time());
    }
    return newest;
}

// ---------------------------------------------------------------------------
// Shared compute + export tail for -w. Reads everything it needs from
// appState (loading differs between the two commands).
// ---------------------------------------------------------------------------
static bool computeAndExport(const HeadlessConfig& cfg) {
    // 8. Compute requested output
    std::string ot = cfg.outputType;
    bool computedOk = false;

    // Set T100 reference based on the panel's referenceSource (File/CSV/
    // Average). Shared by the T100 and Absorbance/Transmittance branches (F4).
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

    // Shared T100 transmittance pipeline (F4): reference setup + per-file
    // compute. Tracks per-file success (F5b — transmittanceAvailable must not
    // be set unconditionally) and derives the export set from the LOADED
    // members (F5a — a fresh member seed without a saved selection must still
    // export every file).
    auto ensureT100Transmittance = [&]() {
        // Pre-compute the first file's spectrum: setReferenceFromCurrentSpectrum
        // (referenceSource=File) reads the spectrum cache and returns without a
        // reference when it is missing.
        if (!appState.active->selectedFiles.empty()) {
            std::string fid0 = appState.active->selectedFilenames[0];
            if (appState.active->spectrum.cachedSpectra.find(fid0) == appState.active->spectrum.cachedSpectra.end()) {
                appState.active->spectrum.computeAndCacheSpectrum(appState.active->selectedFiles[0], fid0);
            }
        }
        if (!setupT100Reference()) {
            std::cerr << "Error: Failed to set 100% T reference" << std::endl;
            exit(1);
        }

        appState.active->t100.lastKnownSelection = appState.active->selectedFilenames;
        appState.active->t100.needsRecompute = true;
        bool anyOk = false;
        for (const auto& fid : appState.active->t100.lastKnownSelection) {
            if (appState.active->t100.cachedTransY.find(fid) == appState.active->t100.cachedTransY.end()) {
                if (!appState.active->t100.computeTransmittanceForFile(fid)) continue;
            }
            anyOk = true;
        }
        appState.active->t100.transmittanceAvailable = anyOk;
        return anyOk;
    };

    if (ot == "Corrected interferograms from selected files" ||
        ot == "Uncorrected interferograms from selected files") {
        // F7: the GUI gates IFG artifacts on datasetInfo.hasInterferograms;
        // headless must not run xAxisFromHilbert on a wavenumber axis (or dump
        // spectra as "Reference/Primary") for precomputed-spectra workspaces.
        if (!appState.active->datasetInfo.hasInterferograms) {
            std::cerr << "Error: workspace has no interferograms (hasInterferograms=false)" << std::endl;
            exit(1);
        }
        computedOk = true;
    } else if (ot == "Spectra from selected files") {
        // F6: writeSpectraCsv (export.cpp) ensures each checked file's
        // spectrum is cached and warns-and-skips on failure — no pre-compute
        // loop (and no hard exit(1) on a single failure) needed here.
        computedOk = true;
    } else if (ot == "Average spectrum") {
        pollUntilDone(appState.active->averageSpectrum);
        computedOk = appState.active->averageSpectrum.averageAvailable;
    } else if (ot == "SNR spectrum") {
        // F10: count loaded members (selectedFiles), not the checkbox vector.
        if (countCheckedFiles() < 2) {
            std::cerr << "Error: SNR requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        pollUntilDone(appState.active->snrSpectrum);
        computedOk = appState.active->snrSpectrum.snrAvailable;
    } else if (ot == "Allan-Werle 3D" || ot == "Allan-Werle slice") {
        // F3: no pre-compute loop — AllanVariance phase 0 re-reads every file
        // via workspaceRead in its own workers and never consults
        // spectrum.cachedSpectra. The removed loop's only effect was
        // wsMirrorSpectrum persisting unwanted spectra/spec_* members.
        if (countCheckedFiles() < 2) {
            std::cerr << "Error: Allan variance requires at least 2 files selected for averaging" << std::endl;
            exit(1);
        }
        pollUntilDone(appState.active->allanVariance);
        computedOk = appState.active->allanVariance.allanAvailable;
    } else if (ot == "100% T transmission line" ||
               ot == "100% T lines for all files" ||
               ot == "100% T standard deviation") {
        if (!ensureT100Transmittance()) {
            std::cerr << "Error: 100% T transmittance computation failed" << std::endl;
            exit(1);
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
            computedOk = true;
        }
    } else if (ot == "Absorbance from selected files" ||
               ot == "Transmittance from selected files") {
        // Reuse the T100 transmittance pipeline (T% = spec/ref × 100); the
        // export writers convert to -log10(T) or fractional T.
        if (!ensureT100Transmittance()) {
            std::cerr << "Error: Transmittance computation failed" << std::endl;
            exit(1);
        }
        computedOk = true;
    } else {
        std::cerr << "Error: Unknown output type '" << ot << "'. "
                  << "Available: Corrected interferograms from selected files, "
                  << "Uncorrected interferograms from selected files, "
                  << "Average spectrum, SNR spectrum, Spectra from selected files, "
                  << "Allan-Werle 3D, Allan-Werle slice, "
                  << "100% T transmission line, 100% T lines for all files, "
                  << "100% T standard deviation, "
                  << "Absorbance from selected files, "
                  << "Transmittance from selected files" << std::endl;
        exit(1);
    }

    if (!computedOk) {
        std::cerr << "Error: Computation failed for output type '" << ot << "'" << std::endl;
        exit(1);
    }

    // 9. Export
    auto beforeExport = newestCsvMtime(cfg.outputPath);
    appState.active->exportPanel.exportArtifact(ot, cfg.outputPath);
    // F5a: fail instead of claiming success when the writers wrote nothing.
    if (newestCsvMtime(cfg.outputPath) == beforeExport) {
        std::cerr << "Error: No CSV file was produced for output type '" << ot << "'" << std::endl;
        exit(1);
    }
    std::cout << "Exported '" << ot << "' to " << cfg.outputPath << std::endl;
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
    if (!std::filesystem::exists(cfg.outputPath)) {
        std::cerr << "Error: Directory '" << cfg.outputPath << "' does not exist" << std::endl;
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
    //    F13: naturalBasenameLess matches the GUI/applyViewState order (a
    //    full-id natural sort can disagree for ids with directory prefixes).
    appState.active->sortedFiles = appState.active->csvFiles;
    std::sort(appState.active->sortedFiles.begin(), appState.active->sortedFiles.end(), naturalBasenameLess);

    // 6. Load EVERY member (no GUI limit in headless mode): rawDataCache feeds
    //    the IFG CSV writers. loadFileStatic routes the "HDF5 Workspace"
    //    sentinel to workspaceRead (adapter_registry.cpp).
    //    F1: no downsampling pass — enableDownsampling defaults to true here
    //    (applySessionDefaults is GUI-only, configPtr is null) but nothing in
    //    headless reads loadedData; every export re-reads full density via
    //    rawDataCache or workspaceRead. The decimated copy was dead work with
    //    a referenceDetector-based factor/loop bound that also truncated the
    //    primary channel to the reference length.
    for (size_t i = 0; i < appState.active->sortedFiles.size(); i++) {
        try {
            const auto& filePath = appState.active->sortedFiles[i];
            InterferogramData data = workspaceRead(appState.active->workspace, filePath);
            appState.active->rawDataCache.push_back(data);
            appState.active->loadedData.push_back(std::move(data));
            appState.active->selectedFiles.push_back(filePath);
            appState.active->selectedFilenames.push_back(basenameOf(filePath));
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
    std::string repoDir;
    AppConfig config = loadAppConfigWithRepoDir(repoDir);
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
                          cfg.outputPath, {}, log, error)) {
        if (log.empty() && !error.empty()) log = error;
        if (!log.empty()) std::cout << log << std::endl;
        std::cerr << "Error: Converter '" << cfg.converter << "' failed" << std::endl;
        exit(1);
    }
    if (!log.empty()) std::cout << log << std::endl;

    try {
        H5Store::validate(cfg.outputPath);
    } catch (const std::exception& e) {
        std::cerr << "Error: Converted file failed validation: " << e.what() << std::endl;
        exit(1);
    }
    std::cout << "Converted '" << cfg.path << "' -> '" << cfg.outputPath
              << "' using " << desc->id << std::endl;
}

// ---------------------------------------------------------------------------
// Sync converters (-sync-converters): clone on first run, pull afterwards.
// ---------------------------------------------------------------------------
static void handleSyncConverters() {
    std::string repoDir;
    AppConfig config = loadAppConfigWithRepoDir(repoDir);
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
// Compare (-cmp): compute average spectrum from two workspaces, emit ratio/diff.
// ---------------------------------------------------------------------------
static void computeAvgSpectrum(const std::string& h5Path,
                               std::vector<double>& outX, std::vector<double>& outY,
                               const json* cfg = nullptr, int pinnedXUnit = 0) {
    // Open workspace, load all members, compute spectra, average on common grid.
    auto sess = std::make_unique<WorkspaceSession>();
    sess->key = h5Path;
    sess->path = h5Path;
    wireSessionPanels(appState, *sess);
    appState.sessions.push_back(std::move(sess));
    appState.active = appState.sessions.back().get();
    appState.activeSessionIdx = static_cast<int>(appState.sessions.size() - 1);
    appState.lastActiveSessionIdx = appState.activeSessionIdx;
    appState.activeTabKind = ActiveTabKind::Workspace;

    try {
        openWorkspace(appState, h5Path);
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to open workspace '" << h5Path << "': " << e.what() << std::endl;
        exit(1);
    }
    if (appState.active->csvFiles.empty()) {
        std::cerr << "Error: No members in workspace '" << h5Path << "'" << std::endl;
        exit(1);
    }
    // Optional processing config — the -cmp command passes the same config to
    // both workspaces so the ratio/difference is a like-for-like comparison.
    if (cfg && !cfg->empty()) applyJsonConfig(appState, *cfg);
    // F8: -cmp must compare in ONE X unit. Without a config each workspace
    // restores its own saved averageView.xUnit, and resampleToGrid would
    // interpolate across incompatible unit systems (endpoint-clamped flat
    // sample curve). Pin both to the same unit — cm-1 unless the config
    // overrides (handleCompare derives pinnedXUnit from the same config).
    appState.active->averageSpectrum.plot.xUnitSelector = pinnedXUnit;
    appState.active->averageSpectrum.plot.prevXUnitSelector = pinnedXUnit;
    appState.active->sortedFiles = appState.active->csvFiles;
    // F13: naturalBasenameLess matches the GUI/applyViewState order.
    std::sort(appState.active->sortedFiles.begin(), appState.active->sortedFiles.end(), naturalBasenameLess);
    // F1: no downsampling pass (see handleWorkspace) — the average workers
    // re-read full density via workspaceRead.
    for (size_t i = 0; i < appState.active->sortedFiles.size(); i++) {
        try {
            const auto& filePath = appState.active->sortedFiles[i];
            InterferogramData data = workspaceRead(appState.active->workspace, filePath);
            appState.active->rawDataCache.push_back(data);
            appState.active->loadedData.push_back(std::move(data));
            appState.active->selectedFiles.push_back(filePath);
            appState.active->selectedFilenames.push_back(basenameOf(filePath));
        } catch (const std::exception& e) {
            // F11: mirror the -w handler — warn and skip instead of silently
            // continuing (a fully-failed load otherwise surfaces only as
            // "Average spectrum computation failed").
            std::cerr << "Warning: Failed to load " << appState.active->sortedFiles[i]
                      << ": " << e.what() << std::endl;
            continue;
        }
    }
    appState.active->dataLoaded = true;
    appState.active->filesSelectedForAveraging.resize(appState.active->sortedFiles.size(), true);
    pollUntilDone(appState.active->averageSpectrum);
    if (!appState.active->averageSpectrum.averageAvailable) {
        std::cerr << "Error: Average spectrum computation failed for '" << h5Path << "'" << std::endl;
        exit(1);
    }
    outX = appState.active->averageSpectrum.cachedAverageX;
    outY = appState.active->averageSpectrum.cachedAverageY;
}

static void handleCompare(const HeadlessConfig& cfg) {
    if (!std::filesystem::is_regular_file(cfg.path)) {
        std::cerr << "Error: Sample workspace '" << cfg.path << "' not found" << std::endl;
        exit(1);
    }
    if (!std::filesystem::is_regular_file(cfg.referencePath)) {
        std::cerr << "Error: Reference workspace '" << cfg.referencePath << "' not found" << std::endl;
        exit(1);
    }
    if (!std::filesystem::exists(cfg.outputPath)) {
        std::cerr << "Error: Directory '" << cfg.outputPath << "' does not exist" << std::endl;
        exit(1);
    }

    // Optional processing config (applied to both workspaces below)
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
            std::cerr << "Error: Invalid config: " << e.what() << std::endl;
            exit(1);
        }
    }

    // F8: pin both workspaces to ONE X unit so the averages land on grids in
    // the same unit system (cm-1 unless the config overrides with
    // average.xUnit). The CSV header below uses the same pinned unit.
    int pinnedXUnit = 0;
    if (!j.empty() && j.contains("average")) {
        const auto& av = j["average"];
        if (av.contains("xUnit"))
            pinnedXUnit = jsonXUnitToInt(jsonVal<std::string>(av, "xUnit", "cm-1"));
    }

    // Compute average spectra — the same optional processing config is applied
    // to BOTH workspaces so the ratio/difference is a like-for-like comparison.
    std::vector<double> sampleX, sampleY, refX, refY;
    computeAvgSpectrum(cfg.path, sampleX, sampleY, &j, pinnedXUnit);
    auto sampleSess = std::move(appState.sessions.back());
    appState.sessions.pop_back();
    sampleSess.reset();   // free the sample session's loaded data before the reference
    computeAvgSpectrum(cfg.referencePath, refX, refY, &j, pinnedXUnit);
    // F15: the reference session is never used after extraction — pop it like
    // the sample session instead of leaking it as appState.active.
    auto refSess = std::move(appState.sessions.back());
    appState.sessions.pop_back();
    refSess.reset();

    // Export both average spectra on the shared reference grid — the same
    // overlay the UI Comparator shows, with the sample interpolated onto the
    // reference axis (resampleToGrid handles both ascending/descending X,
    // endpoint-clamped — same as the panels).
    auto interpSample = resampleToGrid(sampleX, sampleY, refX);

    std::string slug = std::filesystem::path(cfg.path).stem().string();
    // Sanitize: replace non-alphanumeric/underscore/dash with underscore
    for (char& c : slug)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            c = '_';
    std::string ot = cfg.outputType;

    // X-unit label follows the pinned average-panel selector (matches
    // export.cpp); the reference session is already freed, so use the pinned
    // unit directly.
    const char* xLabel = "Wavenumber [cm-1]";
    if (pinnedXUnit == 1) xLabel = "Wavelength [um]";
    else if (pinnedXUnit == 2) xLabel = "Frequency [THz]";

    if (ot == "Comparator spectra") {
        std::string path = cfg.outputPath + "/" + slug + "_comparator_spectra.csv";
        std::ofstream ofs(path);
        if (!ofs.is_open()) { std::cerr << "Error: cannot write " << path << std::endl; exit(1); }
        ofs << std::setprecision(15);
        ofs << xLabel << ",Sample average,Reference average\n";
        for (size_t i = 0; i < refX.size(); i++)
            ofs << refX[i] << "," << interpSample[i] << "," << refY[i] << "\n";
        ofs.close();
        std::cout << "Exported 'Comparator spectra' to " << cfg.outputPath << std::endl;
    } else {
        std::cerr << "Error: Unknown comparator output type '" << ot << "'. "
                  << "Available: Comparator spectra" << std::endl;
        exit(1);
    }
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
        cfg.outputPath = argv[4];
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
        cfg.outputPath = argv[4];
        return false;
    }

    if (flag == "-cmp") {
        if (argc < 6 || argc > 7) {
            std::cerr << "Error: -cmp requires <sample.h5> <reference.h5> <output type> <output dir> [<config>]"
                      << std::endl;
            return true;
        }
        cfg.command = HeadlessConfig::Command::Compare;
        cfg.path = argv[2];
        cfg.referencePath = argv[3];
        cfg.outputType = argv[4];
        cfg.outputPath = argv[5];
        if (argc == 7) cfg.configPath = argv[6];
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
        case HeadlessConfig::Command::Compare:
            handleCompare(cfg);
            return true;
        case HeadlessConfig::Command::None:
            return false;
    }
    return false;
}
