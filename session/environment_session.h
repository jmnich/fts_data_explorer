#pragma once

#include "pthread_compat.h"   // GCC 16+: must precede <future>/<mutex> (_GNU_SOURCE undefined)
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "session_base.h"
#include "spectral_pool.h"

struct AppState;

// Phase-3 M3.2 — instantiable cross-workspace analysis tab (audit §3.3).
// LIVE object, never folded: multiple instances of a type coexist, each owns
// its state + futures; tab switch only changes which instance renders.
// Picks use STABLE keys (workspace path, or "cross.h5#sourceId") resolved to
// live sessions at each use; a closed referenced session degrades the owning
// rows (marked unavailable, removable) — never re-pointed.
enum class EnvType { Absorbance, Comparator };   // Pca removed (Phase-2 user decision)

// Comparator artifact types: what to overlay across the selected datasets.
enum class ComparatorArtifact {
    AverageSpectrum,   // session average spectrum (cachedAverageX/Y)
    RawSpectrum,       // individual spectra members (via the spectral pool)
    Snr,               // SNR-per-wavelength (cachedSnrX/Y)
    T100,              // 100% T transmittance curves (cachedTransX/Y per member)
    Interferogram,     // raw interferograms (primary detector vs sample index)
};

const char* envTypeName(EnvType t);              // "Absorbance" / "Comparator"
const char* artifactLabel(ComparatorArtifact a); // "Average spectrum" / "SNR" / …

// One overlay curve: label + x/y already in the instance's display unit
// (or sample index for interferograms).
struct ComparatorCurve {
    std::string label;
    std::vector<double> x, y;
};

class EnvironmentSession : public SessionBase {
public:
    EnvType type = EnvType::Absorbance;
    std::string instanceName;            // "Absorbance 1", ... (unique window title)
    std::string comment;                 // free-text note (in-memory, Phase 3)
    char commentBuf[4096] = {};          // editor buffer, write-through to comment
    int xUnitSelector = 0;               // 0 cm-1, 1 um, 2 THz
    int prevXUnitSelector = 0;
    int yMode = 0;                       // Absorbance only: 0 T%, 1 A
    // Plot ranging (spectrum-view scheme, both types): 0 all, 1 tight, 2 force.
    int yAxisMode = 0;
    int prevYAxisMode = -1;
    double forcedYMin = 0.0, forcedYMax = 1.0;

    // Comparator selection.
    int artifactSelector = 0;            // ComparatorArtifact index
    std::vector<std::string> comparatorKeys;   // stable keys; empty = all open datasets
    // True once the user has toggled a checkbox: an empty comparatorKeys then
    // means "nothing selected", not "all open datasets".
    bool comparatorKeysExplicit = false;
    // Per-dataset member pick (multi-member artifacts: raw spectra / T100 / IFG).
    // key = dataset stable key, value = member id ("" / absent = first member).
    std::map<std::string, std::string> memberPicks;

    // Absorbance picks.
    // STABLE keys — resolved per use; degraded, never re-pointed.
    std::string refKey;
    std::string refMember;
    std::vector<std::pair<std::string, std::string>> samples;  // (workspaceKey, memberId)
    // Computed results: common grid (display unit) + raw clamped ratio per
    // sample (T%/A source, audit §5.2) + display curve per current yMode.
    std::vector<double> gridX, refY;
    std::map<std::pair<std::string, std::string>, std::vector<double>> ratioY;
    std::map<std::pair<std::string, std::string>, std::vector<double>> curveY;
    bool computed = false;

    // T100-pattern plot interaction (t100.h subset).
    bool isSelectingXRange = false;
    double selectionStartX = 0.0;
    double selectionEndX = 0.0;
    bool shouldAutoscale = true;
    double manualXMin = 0.0, manualXMax = 0.0;
    double manualYMin = 0.0, manualYMax = 0.0;
    bool leftArrowPressedLastFrame = false, rightArrowPressedLastFrame = false;
    bool leftArrowHandleFlag = false, rightArrowHandleFlag = false;
    double pendingNextXMin = 0.0, pendingNextXMax = -1.0;

    // Async compute (IMGUI_GUIDE §13): futures + main-thread counters only.
    std::vector<std::future<SpectralToolbox::ProcessedSpectrum>> pendingFutures_;
    std::vector<SpectralRef> pendingRefs_;         // aligned with pendingFutures_
    std::vector<ParamFingerprint> pendingFps_;     // aligned: params actually used
    std::vector<SpectralToolbox::ProcessedSpectrum> results_;  // cm-1, ref first
    int totalSubmitted_ = 0;
    int completedCount_ = 0;
    bool batchActive_ = false;

    EnvironmentSession(EnvType t, const std::string& name);
    EnvironmentSession(const EnvironmentSession&) = delete;
    EnvironmentSession& operator=(const EnvironmentSession&) = delete;

    // Enqueue poolComputeRaw per ref (workers capture by value, never touch
    // AppState — average_spectrum.cpp:616 pattern); cache hits enqueue a
    // trivial ready task. Main-thread only.
    void startCompute(AppState& s);
    // Poll ready futures; apply results on completion (main thread only).
    void tickAsync() override;
    void render() override;              // config window + view window (docked)
    void closeRequest() override;        // no persistence → direct removal
    bool isDirty() const override { return false; }   // nothing persisted in Phase 3
    const std::string& title() const override { return titleCache_; }
    void onActivate() override {}        // AppLoop sets needsRedraw
    void onDeactivate() override {}      // Phase 4: layout save

    // yMode toggle: rewrite curveY from ratioY — instant, no recompute.
    void applyYMode();
    // xUnit change: convert gridX in place (ratios are unit-independent).
    void convertXInPlace();

    // Extract overlay curves for the selected artifact from the selected
    // datasets (public for the roundtrip test; pure data extraction).
    std::vector<ComparatorCurve> gatherCurves(AppState& s);

private:
    std::string titleCache_;
    void finalizeCompute();              // ref grid + ratios + curveY (main thread)
    void renderConfigWindow();           // instanceName (pickers/selectors/comment)
    void renderViewWindow();             // instanceName + " View" (plot)
    void renderAbsorbanceConfig();
    void renderComparatorConfig();
    void renderDatasetSelector();        // comparator included-datasets checkbox list
    void renderXUnitButtons();
    void renderYAxisControls();
    void renderCommentEditor();
    // Common overlay plot with spectrum-view navigation (locked/tight/all Y,
    // shift+drag range, ESC fit-all, arrows pan, wheel zoom).
    void renderPlot(const std::vector<ComparatorCurve>& curves,
                    const std::string& xLabel, const std::string& yLabel,
                    bool hasGuideline, double guideline, bool showLegend);
    void exportCsv();
};

// Registry ops (main thread). createEnvironment auto-names from the monotonic
// counters and activates the new instance. removeEnvironment erases + fixes
// activeEnvIdx (== removed → focus the Session tab).
EnvironmentSession* createEnvironment(AppState& s, EnvType t);
void activateEnvironment(AppState& s, int idx);
void removeEnvironment(AppState& s, int idx);
