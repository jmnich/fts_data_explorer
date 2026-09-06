// Standalone assert check for the batch recipe model (session/batch_engine.h):
// JSON validation, round-trip identity, built-ins, capture-from-workspace, and
// derivative stripping. The pure logic is header-only — nothing to link.
// Build:
//   g++ -std=c++17 -I. -Ifftw-3.3.10/api \
//       -Ibuild/linux-release/_deps/nlohmann_json-src/include \
//       playground/tests/batch_recipes/test_batch_recipes.cpp \
//       -o /tmp/test_batch_recipes && /tmp/test_batch_recipes
#include "session/batch_engine.h"

#include <cassert>
#include <cstdio>

static bool recipesEqual(const Recipe& a, const Recipe& b) {
    if (a.name != b.name || a.comment != b.comment ||
        a.artifacts != b.artifacts ||
        a.zeroPadK != b.zeroPadK || a.apodWindow != b.apodWindow ||
        a.xCorrectionMethod != b.xCorrectionMethod ||
        a.prominenceThreshold != b.prominenceThreshold ||
        a.hasRefLaserOverride != b.hasRefLaserOverride ||
        a.refLaserUm != b.refLaserUm ||
        a.hasSensitivityOverride != b.hasSensitivityOverride ||
        a.detectorSensitivityKVPerW != b.detectorSensitivityKVPerW ||
        a.allanDecimation != b.allanDecimation ||
        a.allanXMinUm != b.allanXMinUm || a.allanXMaxUm != b.allanXMaxUm ||
        a.allanCalcBase != b.allanCalcBase)
        return false;
    if (a.apodParams.gaussSigma != b.apodParams.gaussSigma ||
        a.apodParams.rectWidth != b.apodParams.rectWidth ||
        a.apodParams.rectAsymMode != b.apodParams.rectAsymMode ||
        a.apodParams.nortonBeerFwhm != b.apodParams.nortonBeerFwhm ||
        a.apodParams.dolphChebyshevAt != b.apodParams.dolphChebyshevAt ||
        a.apodParams.hammingAlpha != b.apodParams.hammingAlpha ||
        a.apodParams.kaiserBeta != b.apodParams.kaiserBeta)
        return false;
    return a.energyRatios == b.energyRatios;
}

// ── 1. recipeFromJson accept/reject table ───────────────────────────────────
static void testRecipeFromJson() {
    std::string err;
    nlohmann::json valid = {
        {"name", "Test recipe"},
        {"artifacts", {"spectra", "average"}},
        {"spectrum", {{"zeroPadK", 4},
                      {"apodization", {{"window", "norton_beer"},
                                       {"nortonBeerFwhm", 1.3}}},
                      {"xCorrectionMethod", "hilbert"},
                      {"prominenceThreshold", 0.03}}},
        {"overrides", {{"refLaserUm", 1.55}, {"detectorSensitivityKVPerW", 2.5}}},
    };
    Recipe r = recipeFromJson(valid, err);
    assert(err.empty());
    assert(r.name == "Test recipe");
    assert(r.zeroPadK == 4);
    assert(r.apodWindow == static_cast<int>(ApodizationWindow::NortonBeer));
    assert(r.apodParams.nortonBeerFwhm == 1.3f);
    assert(r.xCorrectionMethod == 0 && r.prominenceThreshold == 0.03f);
    assert(r.hasRefLaserOverride && r.refLaserUm == 1.55);
    assert(r.hasSensitivityOverride && r.detectorSensitivityKVPerW == 2.5);

    // name required
    err.clear(); recipeFromJson(nlohmann::json{{"artifacts", {"spectra"}}}, err);
    assert(!err.empty() && err.find("name") != std::string::npos);
    err.clear(); recipeFromJson(nlohmann::json{{"name", ""}, {"artifacts", {"spectra"}}}, err);
    assert(!err.empty());
    // artifacts required, non-empty, known values
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}}, err);
    assert(!err.empty());
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {}}}, err);
    assert(!err.empty());
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr "}}}, err);
    assert(!err.empty() && err.find("snr ") != std::string::npos);
    // K bounds 0..16
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"zeroPadK", -1}}}}, err);
    assert(!err.empty() && err.find("zeroPadK") != std::string::npos);
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"zeroPadK", 17}}}}, err);
    assert(!err.empty());
    // window names (all 10), unknown rejected
    const char* windows[10] = {"rectangular", "gauss", "triangular", "norton_beer",
                               "dolph_chebyshev", "hamming", "blackman_harris",
                               "hann", "happ_genzel", "kaiser"};
    for (const char* w : windows) {
        err.clear();
        Recipe rr = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                                  {"spectrum", {{"apodization", {{"window", w}}}}}}, err);
        assert(err.empty());
        assert(batch_recipe_detail::windowName(rr.apodWindow) == w);
    }
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"apodization", {{"window", "norton"}}}}}}, err);
    assert(!err.empty() && err.find("window") != std::string::npos);
    // FWHM range 1.0..2.0
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"apodization", {{"window", "norton_beer"},
                                                                                {"nortonBeerFwhm", 0.5}}}}}}, err);
    assert(!err.empty() && err.find("apodization") != std::string::npos);
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"apodization", {{"window", "norton_beer"},
                                                                                {"nortonBeerFwhm", 2.5}}}}}}, err);
    assert(!err.empty());
    // xCorrectionMethod is case-sensitive
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"spectrum", {{"xCorrectionMethod", "Peaks"}}}}, err);
    assert(!err.empty());
    err.clear();
    Recipe peaks = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                                 {"spectrum", {{"xCorrectionMethod", "peaks"}}}}, err);
    assert(err.empty() && peaks.xCorrectionMethod == 1);
    // overrides: absent and null both mean "use dataset's value"
    err.clear();
    Recipe noOv = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}}}, err);
    assert(err.empty() && !noOv.hasRefLaserOverride && !noOv.hasSensitivityOverride);
    err.clear();
    Recipe nullOv = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                                  {"overrides", {{"refLaserUm", nullptr},
                                                                 {"detectorSensitivityKVPerW", nullptr}}}}, err);
    assert(err.empty() && !nullOv.hasRefLaserOverride && !nullOv.hasSensitivityOverride);
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"snr"}},
                                               {"overrides", {{"refLaserUm", -1.0}}}}, err);
    assert(!err.empty());
    // allan validation
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"allan"}},
                                               {"allan", {{"wavelengthDecimation", 0}}}}, err);
    assert(!err.empty());
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"allan"}},
                                               {"allan", {{"xRangeMinUm", 30.0}, {"xRangeMaxUm", 1.0}}}}, err);
    assert(!err.empty());
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"allan"}},
                                               {"allan", {{"calcBase", "T%"}}}}, err);
    assert(!err.empty());
    err.clear();
    Recipe allan = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"allan"}},
                                                 {"allan", {{"wavelengthDecimation", 3},
                                                            {"xRangeMinUm", 2.0}, {"xRangeMaxUm", 40.0},
                                                            {"calcBase", "Spectrum"}}}}, err);
    assert(err.empty());
    assert(allan.allanDecimation == 3 && allan.allanXMinUm == 2.0 &&
           allan.allanXMaxUm == 40.0 && allan.allanCalcBase == 1);
    // t100 energyRatios band validation ("max" / number)
    err.clear(); recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"t100"}},
                                               {"t100", {{"energyRatios",
                                                   {{{"num", "abc"}, {"den", "2000"}},
                                                    {{"num", "2000"}, {"den", "1000"}},
                                                    {{"num", "150"}, {"den", "max"}}}}}}}, err);
    assert(!err.empty());
    err.clear();
    Recipe t = recipeFromJson(nlohmann::json{{"name", "x"}, {"artifacts", {"t100"}},
                                             {"t100", {{"energyRatios",
                                                 {{{"num", "4000"}, {"den", "2000"}},
                                                  {{"num", "2000"}, {"den", "1000"}},
                                                  {{"num", "150"}, {"den", "Max"}}}}}}}, err);
    assert(err.empty());
    assert(t.energyRatios[2].second == "Max");
}

// ── 2. round-trip identity ──────────────────────────────────────────────────
static void testRoundTrip() {
    for (const Recipe& r : builtinRecipes()) {
        std::string err;
        Recipe back = recipeFromJson(recipeToJson(r), err);
        assert(err.empty());
        assert(recipesEqual(r, back));
    }
    // override-carrying recipe with a gauss window + allan section
    Recipe o;
    o.name = "Overrides";
    o.artifacts = {"spectra", "allan"};
    o.zeroPadK = 6;
    o.apodWindow = static_cast<int>(ApodizationWindow::Gauss);
    o.apodParams.gaussSigma = 2.4f;
    o.xCorrectionMethod = 1;
    o.prominenceThreshold = 0.11f;
    o.hasRefLaserOverride = true;
    o.refLaserUm = 1.63;
    o.hasSensitivityOverride = true;
    o.detectorSensitivityKVPerW = 3.2;
    o.allanDecimation = 9;
    o.allanXMinUm = 0.5;
    o.allanXMaxUm = 12.0;
    o.allanCalcBase = 1;
    std::string err;
    Recipe back = recipeFromJson(recipeToJson(o), err);
    assert(err.empty());
    assert(recipesEqual(o, back));
    // t100-only recipe keeps the t100 section; non-t100 recipes drop it
    const nlohmann::json avgJson = recipeToJson(builtinRecipes()[0]);
    const nlohmann::json allJson = recipeToJson(builtinRecipes()[3]);
    assert(avgJson.find("t100") == avgJson.end());
    assert(allJson.find("t100") != allJson.end());
    assert(allJson.find("allan") == allJson.end());
}

// ── 3. built-ins sanity ─────────────────────────────────────────────────────
static void testBuiltins() {
    const auto& bs = builtinRecipes();
    assert(bs.size() == 6);
    for (const Recipe& r : bs) {
        assert(r.zeroPadK == 2);
        assert(r.apodWindow == static_cast<int>(ApodizationWindow::NortonBeer));
        assert(r.xCorrectionMethod == 0);
        assert(!r.hasRefLaserOverride && !r.hasSensitivityOverride);
        assert(!recipeHas(r, "allan"));
        assert(!r.name.empty());
    }
    const float ladder[6] = {1.2f, 1.4f, 1.6f, 1.2f, 1.4f, 1.6f};
    for (size_t i = 0; i < bs.size(); ++i)
        assert(bs[i].apodParams.nortonBeerFwhm == ladder[i]);
    assert(bs[0].artifacts == std::vector<std::string>({"spectra", "average"}));
    assert(bs[3].artifacts == std::vector<std::string>({"spectra", "average", "snr", "t100"}));
    assert(bs[0].name == "Average spectrum - NB weak");
    assert(bs[2].name == "Average spectrum - NB strong");
    assert(bs[5].name == "All - NB strong");
}

// ── 4. capture-from-workspace mirroring ─────────────────────────────────────
static Workspace syntheticWorkspace() {
    Workspace ws;
    ws.format = "unified-spectral-data-container";

    InterferogramMember ifgDeriv, ifgOrig;
    ifgDeriv.kind = MemberKind::Derivative;
    ifgOrig.kind = MemberKind::Original;
    ws.uncorrectedIfg.members = {ifgDeriv, ifgOrig};

    TwoColumnMember specDeriv, specOrig;
    specDeriv.kind = MemberKind::Derivative;
    specOrig.kind = MemberKind::Original;
    ws.spectra.members = {specDeriv, specOrig};
    TwoColumnMember avg;
    avg.kind = MemberKind::Derivative;
    ws.averageSpectra.members = {avg};
    T100Member t100;
    t100.kind = MemberKind::Derivative;
    ws.t100.members = {t100};
    AllanMember allan;
    allan.kind = MemberKind::Derivative;
    ws.allanWerle.members = {allan};

    ws.workspaceJson = {
        {"applications", {{"FTS Data Explorer", {
            {"spectrumView", {
                {"xUnit", 0}, {"refLaserUm", 1.58}, {"zeroPadK", 3},
                {"detectorSensitivityKVPerW", 2.5},
                {"apodization", {{"window", "norton_beer"}, {"nortonBeerFwhm", 1.3}}},
            }},
            {"plotDefaults", {{"xCorrectionMethod", 1}, {"peakProminence", 0.05}}},
            {"t100View", {{"energyRatios", {
                {{"num", "4000"}, {"den", "2000"}},
                {{"num", "500"}, {"den", "250"}},
                {{"num", "150"}, {"den", "max"}},
            }}}},
            {"allanView", {{"wavelengthDecimation", 7}, {"xRangeMin", 2.0},
                           {"xRangeMax", 40.0}, {"calcBase", 1}}},
        }}}},
    };
    return ws;
}

static void testRecipeFromWorkspace() {
    std::string err;
    Workspace ws = syntheticWorkspace();
    Recipe r = recipeFromWorkspace(ws, false, false, err);
    assert(err.empty());
    // derivative groups → artifacts, in canonical order; snr absent
    assert(r.artifacts == std::vector<std::string>({"spectra", "average", "t100", "allan"}));
    assert(r.zeroPadK == 3);
    assert(r.apodWindow == static_cast<int>(ApodizationWindow::NortonBeer));
    assert(r.apodParams.nortonBeerFwhm == 1.3f);
    assert(r.xCorrectionMethod == 1 && r.prominenceThreshold == 0.05f);
    assert(!r.hasRefLaserOverride && r.refLaserUm == 1.58);
    assert(!r.hasSensitivityOverride && r.detectorSensitivityKVPerW == 2.5);
    assert(r.energyRatios[0] == std::make_pair(std::string("4000"), std::string("2000")));
    assert(r.energyRatios[1] == std::make_pair(std::string("500"), std::string("250")));
    assert(r.energyRatios[2] == std::make_pair(std::string("150"), std::string("max")));
    assert(r.allanDecimation == 7 && r.allanXMinUm == 2.0 &&
           r.allanXMaxUm == 40.0 && r.allanCalcBase == 1);

    // override checkboxes pin the captured values
    Recipe ov = recipeFromWorkspace(ws, true, true, err);
    assert(err.empty());
    assert(ov.hasRefLaserOverride && ov.refLaserUm == 1.58);
    assert(ov.hasSensitivityOverride && ov.detectorSensitivityKVPerW == 2.5);

    // dataset with no derivatives → error, no recipe
    Workspace bare;
    bare.format = "unified-spectral-data-container";
    Recipe none = recipeFromWorkspace(bare, false, false, err);
    assert(!err.empty() && none.artifacts.empty());
}

// ── 5. derivative stripping ─────────────────────────────────────────────────
static void testStripAllDerivatives() {
    Workspace ws = syntheticWorkspace();
    stripAllDerivatives(ws);
    for (const auto& m : ws.uncorrectedIfg.members) assert(m.kind == MemberKind::Original);
    for (const auto& m : ws.spectra.members) assert(m.kind == MemberKind::Original);
    assert(ws.uncorrectedIfg.members.size() == 1);   // original kept
    assert(ws.spectra.members.size() == 1);          // original kept
    assert(ws.averageSpectra.members.empty());
    assert(ws.snrSpectra.members.empty());
    assert(ws.correctedIfg.members.empty());
    assert(ws.t100.members.empty());
    assert(ws.allanWerle.members.empty());
}

int main() {
    testRecipeFromJson();
    testRoundTrip();
    testBuiltins();
    testRecipeFromWorkspace();
    testStripAllDerivatives();
    std::printf("batch_recipes: all checks passed\n");
    return 0;
}
