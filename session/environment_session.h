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
struct InterferogramMember;

// Phase-3 M3.2 — instantiable cross-workspace analysis tab (audit §3.3).
// LIVE object, never folded: multiple instances of a type coexist, each owns
// its state + futures; tab switch only changes which instance renders.
// Picks use STABLE keys (workspace path, or "cross.h5#sourceId") resolved to
// live sessions at each use; a closed referenced session degrades the owning
// rows (marked unavailable, removable) — never re-pointed.
enum class EnvType { Absorbance, Comparator };   // Pca removed (Phase-2 user decision)

// Comparator artifact types: what to overlay across the selected datasets.
// CorrectedInterferogram keeps value 4 so persisted configs from the merged
// "Interferogram" artifact (which preferred corrected) map faithfully.
enum class ComparatorArtifact {
    AverageSpectrum,       // session average spectrum (cachedAverageX/Y)
    RawSpectrum,           // individual spectra members (via the spectral pool)
    Snr,                   // SNR-per-wavelength (cachedSnrX/Y)
    T100,                  // 100% T transmittance curves (cachedTransX/Y per member)
    CorrectedInterferogram,// corrected interferograms (primary detector vs sample index)
    RawInterferogram,      // uncorrected interferograms (primary detector vs sample index)
};

const char* experimentTypeName(EnvType t);              // "Absorbance" / "Comparator"
const char* artifactLabel(ComparatorArtifact a); // "Average spectrum" / "SNR" / …

// One overlay curve: label + x/y already in the instance's display unit
// (or sample index for interferograms).
struct ComparatorCurve {
    std::string label;       // full label (legend / CSV headers)
    std::string shortLabel;  // compact label (cursor info box, no dataset name)
    std::vector<double> x, y;
};

// One selectable member of an artifact type. `xUnit` is the member's STORED
// x-unit (0/1/2), or -1 for interferograms (sample-index / OPD X, no conversion).
struct ArtifactMember {
    std::string id;
    int xUnit = 0;
    std::vector<double> x, y;
    bool stale = false;
};

struct ArtifactInfo {
    bool available = false;              // ≥1 member
    bool stale = false;                  // any member stale
    std::vector<ArtifactMember> members;
};

// Correction parameters used to derive the corrected-IFG OPD axis from a raw
// interferogram (mirrors the interferogram view + spectrum pipeline).
struct IfgDeriveParams {
    double laserUm = 1.550;
    int method = 0;                      // 0 Hilbert, 1 peak-finding
    float prominence = 0.02f;
};

class EnvironmentSession : public SessionBase {
public:
    EnvType type = EnvType::Absorbance;
    std::string id;                      // stable experiment id in the .cross.h5
                                         // ("" = transient, never saved)
    std::string instanceName;            // "Absorbance 1", ... (unique window title)
    // RENAME-STABLE identity (bugfix 2026-08-14): generated once at
    // construction, never changes. Tab-strip keys ("exp:<stripKey>") and the
    // ImPlot plot id ("##envPlot<stripKey>") use it, so renaming neither
    // shuffles the tab nor resets the plot's X range. Persisted experiments
    // are matched to the manifest order via their saved id.
    std::string stripKey;
    std::string comment;                 // free-text note (in-memory, Phase 3)
    char commentBuf[4096] = {};          // editor buffer, write-through to comment
    char nameBuf[256] = {};              // editor buffer for the inline rename
    // Phase 4: unsaved-changes flag (set by every mutating UI action, cleared
    // by [Save]) + result-staleness flag (stored fingerprints vs current
    // source params — advisory only; the pool cache re-verifies on every read).
    bool dirty = false;
    bool stale = false;
    // Tab-strip visibility (bugfix 2026-08-14): closing the tab HIDES it from
    // the strip — the instance stays live in experiments[] and re-opens via
    // the Active Experiments panel row (activateExperiment clears it).
    bool tabHidden = false;
    // FFT-param fingerprints per referenced source at compute time (M4.1);
    // the staleness badge compares these to the current params (M4.3).
    std::map<std::string, ParamFingerprint> storedFingerprints;
    int xUnitSelector = 0;               // 0 cm-1, 1 um, 2 THz
    int prevXUnitSelector = 0;
    int yMode = 0;                       // Absorbance only: 0 T%, 1 A
    // Plot ranging (spectrum-view scheme, both types): 0 all, 1 tight, 2 force.
    int yAxisMode = 0;
    int prevYAxisMode = -1;
    double forcedYMin = 0.0, forcedYMax = 1.0;
    // Y scale (spectrum-view scheme): 0 lin, 1 log10, 2 dB. log/dB are gated
    // off for T100 and Interferogram artifacts (non-positive values).
    int yScaleSelector = 0;
    int prevYScaleSelector = -1;
    // Tracking cursor (spectrum-view scheme): marks ALL displayed curves.
    bool showTrackingCursor = false;
    // Display-only stride downsampling to maxPointsBeforeDownsampling (the
    // interferogram-view scheme). OFF by default: full-resolution display;
    // the cursor reads full-res data and CSV export never downsamples.
    bool downsampleDisplay = false;

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
    // Last-rendered plot X limits (captured each frame in renderPlot) — the
    // "current plot area" source for the export X-range mode.
    double viewXMin = 0.0, viewXMax = 0.0;
    // Export X-range mode: 0 all, 1 current plot area, 2 manual.
    int exportXRangeMode = 0;
    double exportXMin = 0.0, exportXMax = 0.0;

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
    // Tab-selector close: DEACTIVATES the tab only (focus the Session tab) —
    // the instance stays live and listed in the Active Experiments panel.
    // Deletion happens exclusively via requestDelete() from that panel.
    void closeRequest() override;
    // Delete the instance: dirty or persisted experiments confirm via the
    // delete modal; transient clean instances remove directly.
    void requestDelete();
    bool isDirty() const override { return dirty; }
    const std::string& title() const override { return titleCache_; }
    void onActivate() override {}        // AppLoop sets needsRedraw
    void onDeactivate() override {}      // Phase 4: layout save

    // Rename the experiment (label + window title); marks dirty.
    void rename(const std::string& name);
    // Save this experiment into the open .cross.h5 (id assigned on first
    // save); clears dirty. No-op when no multi-workspace is open.
    void save(AppState& s);
    // Re-derive the staleness flag from storedFingerprints vs the sources'
    // current params (open tab → parked session; not open → persisted
    // workspace.json). Call at load and after [Compute].
    void updateStaleness(AppState& s);
    // Tab label with dirty star + staleness badge ("Name * ⚠").
    std::string tabLabel() const;

    // yMode toggle: rewrite curveY from ratioY — instant, no recompute.
    void applyYMode();
    // xUnit change: convert gridX in place (ratios are unit-independent).
    void convertXInPlace();

    // Extract overlay curves for the selected artifact from the selected
    // datasets (public for the roundtrip test; pure data extraction).
    std::vector<ComparatorCurve> gatherCurves(AppState& s);

private:
    std::string titleCache_;
    // Corrected-IFG derivation (sourceKey#memberId -> OPD axis um, RAM only):
    // rebuilt per entry when the source's correction params change.
    std::map<std::string, std::vector<double>> derivedOpdCache_;
    std::map<std::string, IfgDeriveParams> derivedOpdParams_;
    // Members available in a workspace for one artifact type. For
    // CorrectedInterferogram the persisted group wins; datasets without it
    // fall back to deriving corrected IFGs from the raw group (cached).
    ArtifactInfo artifactInfo(const Workspace& ws, ComparatorArtifact a,
                              const std::string& sourceKey);
    // Correction params for a comparator source: live session when open,
    // else the persisted workspace params, else defaults.
    IfgDeriveParams ifgDeriveParamsFor(const std::string& sourceKey,
                                       const Workspace& ws);
    // OPD axis (um) for an uncorrected IFG member: mirror displacement ×2
    // from the reference detector (view/pipeline algorithm), cached.
    std::vector<double> derivedOpdAxis(const std::string& sourceKey,
                                       const InterferogramMember& m,
                                       const IfgDeriveParams& p);
    void finalizeCompute();              // ref grid + ratios + curveY (main thread)
    void renderConfigWindow();           // instanceName (pickers/selectors/comment)
    void renderViewWindow();             // instanceName + " View" (plot)
    void renderAbsorbanceConfig();
    void renderComparatorConfig();
    void renderDatasetSelector();        // comparator included-datasets checkbox list
    void renderXUnitButtons();
    void renderYAxisControls();
    void renderYScaleButtons();          // lin/log/dB, gated off for T100/IFG
    void renderCursorToggle();
    // Docked "Plot Ranging" panel (comparator): X unit / Y scale / Y axis /
    // cursor — the spectrum-view navigation block, split into its own window.
    void renderRangingWindow();
    // Docked "Export" panel (comparator): X-range mode + manual min/max.
    void renderExportWindow();
    void renderCommentEditor();
    // Common overlay plot with spectrum-view navigation (locked/tight/all Y,
    // shift+drag range, ESC fit-all, arrows pan, wheel zoom).
    void renderPlot(const std::vector<ComparatorCurve>& curves,
                    const std::string& xLabel, const std::string& yLabel,
                    bool hasGuideline, double guideline, bool showLegend);
    void exportCsv();
};

// Registry ops (main thread). createExperiment auto-names from the monotonic
// counters and activates the new instance. removeExperiment erases + fixes
// activeExperimentIdx (== removed → focus the Session tab).
EnvironmentSession* createExperiment(AppState& s, EnvType t);
void activateExperiment(AppState& s, int idx);
void removeExperiment(AppState& s, int idx);
// Close ALL instances (project switch / go-home); resets the env tab state.
void clearExperiments(AppState& s);
// Project-open helper (main.cpp/session_tab/welcome open paths): load the
// manifest, clear the current experiments, then restore the persisted
// experiments from the file. Returns false on failure (err set).
bool crossOpenProject(AppState& s, const std::string& path, std::string& err);
