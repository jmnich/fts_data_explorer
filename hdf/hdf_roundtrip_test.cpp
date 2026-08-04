// Assert-based conformance test for the HDF5 exchange layer. No test framework.
// Usage:
//   fts_hdf_roundtrip                  -> runs tests 1, 3, 4
//   fts_hdf_roundtrip <example.h5>     -> additionally runs test 2 (python-parser example)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <algorithm>

#include "h5_store.h"
#include "hdf5_util.h"
#include "workspace.h"

namespace {

bool vecEq(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

void test1_handBuiltRoundTrip() {
    Workspace ws;
    ws.format = "unified-spectral-data-container";
    ws.created = "2026-08-02T00:00:00Z";
    ws.measurementConfig = {{"instrument", {{"model", "WUST Mini FTS"}}}};
    ws.measurementComment = "test";
    ws.tags = "ftir, sample";
    ws.workspaceJson = {{"app", {{"name", "FTS Data Explorer"}}}};

    InterferogramMember igm;
    igm.id = "record_0";
    igm.kind = MemberKind::Original;
    igm.col0 = {1.0, 2.0, 3.0};
    igm.col1 = {4.0, 5.0, 6.0};
    igm.columns = {"Reference detector", "Primary detector"};
    igm.units = {"V", "V"};
    ws.uncorrectedIfg.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    ws.uncorrectedIfg.members.push_back(igm);

    const char* tmpFile = "/tmp/fts_hdf_roundtrip_1.h5";
    H5Store::save(tmpFile, ws);
    Workspace back = H5Store::load(tmpFile);
    assert(back.uncorrectedIfg.members.size() == 1);
    assert(vecEq(back.uncorrectedIfg.members[0].col0, igm.col0));
    assert(vecEq(back.uncorrectedIfg.members[0].col1, igm.col1));
    assert(back.uncorrectedIfg.members[0].kind == MemberKind::Original);
    assert(back.measurementComment == "test");
    assert(back.tags == "ftir, sample");
    assert(back.workspaceJson.dump() == ws.workspaceJson.dump());
    assert(back.uncorrectedIfg.origin == ws.uncorrectedIfg.origin);
    std::remove(tmpFile);
    printf("roundtrip: hand-built workspace OK\n");
}

void test2_pythonExampleRoundTrip(const std::string& examplePath) {
    Workspace orig = H5Store::load(examplePath);
    assert(orig.format == "unified-spectral-data-container");
    assert(orig.uncorrectedIfg.members.size() > 0);

    std::string copy = examplePath + ".copy.h5";
    H5Store::save(copy, orig);
    Workspace back = H5Store::load(copy);

    assert(back.uncorrectedIfg.members.size() == orig.uncorrectedIfg.members.size());
    for (size_t i = 0; i < orig.uncorrectedIfg.members.size(); ++i) {
        const auto& a = orig.uncorrectedIfg.members[i];
        const auto& b = back.uncorrectedIfg.members[i];
        assert(a.id == b.id);
        assert(a.kind == b.kind);
        assert(a.columns == b.columns);
        assert(a.units == b.units);
        assert(vecEq(a.col0, b.col0));
        assert(vecEq(a.col1, b.col1));
    }
    assert(back.measurementComment == orig.measurementComment);
    assert(back.tags == orig.tags);
    assert(back.measurementConfig == orig.measurementConfig);

    // Cross-check conformance: file is valid per the strict validator too.
    H5Store::validate(copy);
    std::remove(copy.c_str());
    printf("roundtrip: python-parser example OK (%zu interferograms)\n",
           orig.uncorrectedIfg.members.size());
}

void test3_originalDataProtection(const std::string& examplePath) {
    Workspace ws = H5Store::load(examplePath);
    auto& m = ws.uncorrectedIfg.members[0];
    double before = m.col0[0];
    m.col0[0] += 100.0;  // mutate an original in RAM

    bool threw = false;
    try {
        H5Store::save(examplePath, ws);  // same path: must refuse and leave file untouched
    } catch (const H5Error&) {
        threw = true;
    }
    assert(threw);

    Workspace reloaded = H5Store::load(examplePath);
    assert(reloaded.uncorrectedIfg.members[0].col0[0] == before);
    printf("roundtrip: original-data protection OK (refused + untouched)\n");
}

void test4_schemaAndMemberOrigin() {
    Workspace ws;
    ws.format = "unified-spectral-data-container";
    ws.created = "2026-08-02T00:00:00Z";

    ws.uncorrectedIfg.schema = "interferogram";
    ws.uncorrectedIfg.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    ws.uncorrectedIfg.config = "{}";

    InterferogramMember igm;
    igm.id = "rec";
    igm.kind = MemberKind::Original;
    igm.col0 = {1.0, 2.0};
    igm.col1 = {3.0, 4.0};
    igm.columns = {"Ref", "Prim"};
    igm.units = {"V", "V"};
    ws.uncorrectedIfg.members.push_back(igm);

    ws.spectra.schema = "spectrum/v1";
    TwoColumnMember spec;
    spec.id = "spec_1";
    spec.kind = MemberKind::Derivative;
    spec.x = {1000.0, 2000.0};
    spec.y = {0.5, 0.6};
    spec.columns = {"x", "y"};
    spec.units = {"cm-1", "V"};
    spec.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    spec.config = R"({"inputs":["/igm_uncorrected_x/rec"]})";
    ws.spectra.members.push_back(spec);

    const char* tmpFile = "/tmp/fts_hdf_roundtrip_schema.h5";
    H5Store::save(tmpFile, ws);
    Workspace back = H5Store::load(tmpFile);

    assert(back.uncorrectedIfg.schema == "interferogram");
    assert(back.spectra.schema == "spectrum/v1");
    assert(back.uncorrectedIfg.members.size() == 1);
    assert(back.spectra.members.size() == 1);
    assert(back.spectra.members[0].origin == spec.origin);
    assert(back.spectra.members[0].config == spec.config);
    assert(back.spectra.members[0].kind == MemberKind::Derivative);
    assert(back.uncorrectedIfg.origin == ws.uncorrectedIfg.origin);
    assert(back.uncorrectedIfg.config == ws.uncorrectedIfg.config);
    assert(back.inputsAreValid());
    std::remove(tmpFile);
    printf("roundtrip: schema + per-member origin/config OK\n");
}

// ── Phase 2 tests ──────────────────────────────────────────────────────────

static Workspace buildDerivativeWorkspace() {
    Workspace ws;
    ws.format = "unified-spectral-data-container";
    ws.created = "2026-08-03T00:00:00Z";
    ws.measurementConfig = {{"instrument", {{"model", "WUST Mini FTS"}}}};
    ws.measurementComment = "deriv test";
    ws.tags = "ftir";
    ws.workspaceJson = {{"app", {{"name", "FTS Data Explorer"}}}};

    ws.uncorrectedIfg.schema = "interferogram";
    ws.uncorrectedIfg.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    ws.uncorrectedIfg.config = "{}";
    for (int i = 0; i <= 5; i++) {
        InterferogramMember igm;
        igm.id = "record_" + std::to_string(i);
        igm.kind = MemberKind::Original;
        igm.col0 = {1.0, 2.0, 3.0};
        igm.col1 = {4.0, 5.0, 6.0};
        igm.columns = {"Reference detector", "Primary detector"};
        igm.units = {"V", "V"};
        ws.uncorrectedIfg.members.push_back(igm);
    }

    TwoColumnMember spec;
    spec.id = "spec_record_0";
    spec.kind = MemberKind::Derivative;
    spec.x = {1000.0, 2000.0, 3000.0};
    spec.y = {0.5, 0.6, 0.7};
    spec.columns = {"x", "y"};
    spec.units = {"cm-1", "V"};
    spec.origin = makeOriginJson("FTS Data Explorer", "26.08.3").dump();
    spec.config = R"({"inputs":["/igm_uncorrected_x/record_0"],"params":{"K":2}})";
    ws.spectra.schema = "spectrum/v1";
    ws.spectra.members.push_back(spec);

    TwoColumnMember avg;
    avg.id = "average";
    avg.kind = MemberKind::Derivative;
    avg.x = {1000.0, 2000.0};
    avg.y = {0.4, 0.5};
    avg.columns = {"x", "y"};
    avg.units = {"cm-1", "V"};
    avg.config = R"({"inputs":["/igm_uncorrected_x/record_0"],"count":1})";
    ws.averageSpectra.schema = "average_spectrum/v1";
    ws.averageSpectra.members.push_back(avg);

    TwoColumnMember snr;
    snr.id = "snr";
    snr.kind = MemberKind::Derivative;
    snr.x = {1000.0, 2000.0};
    snr.y = {10.0, 12.0};
    snr.config = R"({"inputs":["/igm_uncorrected_x/record_0"],"fileCount":1})";
    ws.snrSpectra.schema = "snr_spectrum/v1";
    ws.snrSpectra.members.push_back(snr);

    AllanMember allan;
    allan.id = "allan";
    allan.kind = MemberKind::Derivative;
    allan.taus = {1.0, 2.0};
    allan.wavelengths = {1000.0, 2000.0};
    allan.surface = {1.0, 2.0, 3.0, 4.0};
    allan.config = R"({"inputs":["/igm_uncorrected_x/record_0"]})";
    ws.allanWerle.schema = "allan_werle/v1";
    ws.allanWerle.members.push_back(allan);

    T100Member t;
    t.id = "t100";
    t.kind = MemberKind::Derivative;
    t.reference.x = {1000.0, 2000.0};
    t.reference.y = {1.0, 1.1};
    t.reference.columns = {"x", "y"};
    t.reference.units = {"cm-1", "a.u."};
    t.stddev.x = {1000.0, 2000.0};
    t.stddev.y = {0.1, 0.2};
    T100Member::Curve c1, c2;
    c1.fileId = "record_0"; c1.x = {1000.0, 2000.0}; c1.y = {99.0, 98.0};
    c2.fileId = "record_1"; c2.x = {1000.0, 2000.0}; c2.y = {97.0, 96.0};
    t.curves.push_back(c1);
    t.curves.push_back(c2);
    t.config = R"({"inputs":["/igm_uncorrected_x/record_0"],"reference":{"source":"file","path":"/igm_uncorrected_x/record_0"}})";
    ws.t100.schema = "t100/v1";
    ws.t100.members.push_back(t);
    return ws;
}

static bool membersEqual(const TwoColumnMember& a, const TwoColumnMember& b) {
    return a.id == b.id && a.kind == b.kind && a.columns == b.columns &&
           a.units == b.units && a.origin == b.origin && a.config == b.config &&
           vecEq(a.x, b.x) && vecEq(a.y, b.y);
}

static bool allanEqual(const AllanMember& a, const AllanMember& b) {
    return a.id == b.id && a.kind == b.kind && a.config == b.config &&
           vecEq(a.taus, b.taus) && vecEq(a.wavelengths, b.wavelengths) &&
           vecEq(a.surface, b.surface);
}

static bool t100Equal(const T100Member& a, const T100Member& b) {
    if (a.id != b.id || a.kind != b.kind || a.config != b.config) return false;
    if (!vecEq(a.reference.x, b.reference.x) || !vecEq(a.reference.y, b.reference.y)) return false;
    if (!vecEq(a.stddev.x, b.stddev.x) || !vecEq(a.stddev.y, b.stddev.y)) return false;
    if (a.curves.size() != b.curves.size()) return false;
    for (size_t i = 0; i < a.curves.size(); ++i) {
        if (a.curves[i].fileId != b.curves[i].fileId) return false;
        if (!vecEq(a.curves[i].x, b.curves[i].x)) return false;
        if (!vecEq(a.curves[i].y, b.curves[i].y)) return false;
    }
    return true;
}

void test5_derivativeRoundTrip() {
    Workspace ws = buildDerivativeWorkspace();
    const char* tmpFile = "/tmp/fts_hdf_roundtrip_deriv.h5";
    H5Store::save(tmpFile, ws);
    Workspace back = H5Store::load(tmpFile);

    assert(back.uncorrectedIfg.members.size() == 6);
    assert(back.spectra.members.size() == 1);
    assert(membersEqual(back.spectra.members[0], ws.spectra.members[0]));
    assert(back.averageSpectra.members.size() == 1);
    assert(membersEqual(back.averageSpectra.members[0], ws.averageSpectra.members[0]));
    assert(back.snrSpectra.members.size() == 1);
    assert(membersEqual(back.snrSpectra.members[0], ws.snrSpectra.members[0]));
    assert(back.allanWerle.members.size() == 1);
    assert(allanEqual(back.allanWerle.members[0], ws.allanWerle.members[0]));
    assert(back.t100.members.size() == 1);
    assert(t100Equal(back.t100.members[0], ws.t100.members[0]));
    assert(back.inputsAreValid());
    std::remove(tmpFile);
    printf("roundtrip: derivative members round-trip OK\n");
}

void test6_deletedOriginalAuthorization() {
    Workspace ws = buildDerivativeWorkspace();
    const char* tmpFile = "/tmp/fts_hdf_roundtrip_del.h5";
    H5Store::save(tmpFile, ws);

    // Delete record_5 from RAM and authorize the deletion.
    ws.uncorrectedIfg.members.erase(ws.uncorrectedIfg.members.begin() + 5);
    ws.deletedOriginalPaths.push_back("/igm_uncorrected_x/record_5");
    H5Store::save(tmpFile, ws);  // must not throw

    Workspace reloaded = H5Store::load(tmpFile);
    assert(reloaded.uncorrectedIfg.members.size() == 5);
    for (const auto& m : reloaded.uncorrectedIfg.members)
        assert(m.id != "record_5");

    // A fresh load (empty authorization list) saves fine — the member is simply absent.
    Workspace fresh = H5Store::load(tmpFile);
    H5Store::save(tmpFile, fresh);
    std::remove(tmpFile);
    printf("roundtrip: deleted-original authorization OK\n");
}

void test7_unauthorizedOriginalDeletionThrows() {
    Workspace ws = buildDerivativeWorkspace();
    const char* tmpFile = "/tmp/fts_hdf_roundtrip_unauth.h5";
    H5Store::save(tmpFile, ws);

    ws.uncorrectedIfg.members.erase(ws.uncorrectedIfg.members.begin());  // record_0, NOT authorized
    bool threw = false;
    try {
        H5Store::save(tmpFile, ws);
    } catch (const H5Error&) {
        threw = true;
    }
    assert(threw);
    std::remove(tmpFile);
    printf("roundtrip: unauthorized original deletion refused OK\n");
}

void test8_danglingT100RefPath() {
    Workspace ws = buildDerivativeWorkspace();
    assert(ws.danglingInputs().empty());

    ws.t100.members[0].config =
        R"({"inputs":["/igm_uncorrected_x/record_0"],"reference":{"source":"file","path":"/igm_uncorrected_x/ghost"}})";
    auto dangling = ws.danglingInputs();
    assert(std::find(dangling.begin(), dangling.end(), "/igm_uncorrected_x/ghost") != dangling.end());

    // Empty path (CSV reference) is not dangling.
    ws.t100.members[0].config =
        R"({"reference":{"source":"csv","path":""}})";
    assert(ws.danglingInputs().empty());
    printf("roundtrip: dangling t100 reference.path OK\n");
}

void test9_pruneStale() {
    Workspace ws = buildDerivativeWorkspace();
    for (auto& m : ws.spectra.members)        m.stale = true;
    for (auto& m : ws.averageSpectra.members) m.stale = true;
    for (auto& m : ws.allanWerle.members)     m.stale = true;
    for (auto& m : ws.t100.members)           m.stale = true;

    TwoColumnMember fresh;
    fresh.id = "average2";
    fresh.kind = MemberKind::Derivative;
    fresh.x = {1.0}; fresh.y = {2.0};
    fresh.config = R"({"inputs":["/igm_uncorrected_x/record_0"]})";
    ws.averageSpectra.members.push_back(fresh);

    Workspace pruned = ws.pruneStale();
    assert(pruned.uncorrectedIfg.members.size() == 6);        // originals survive
    assert(pruned.spectra.members.empty());                    // stale derivative dropped
    assert(pruned.averageSpectra.members.size() == 1);
    assert(pruned.averageSpectra.members[0].id == "average2"); // fresh survives
    assert(pruned.snrSpectra.members.size() == 1);             // not marked stale
    assert(pruned.allanWerle.members.empty());
    assert(pruned.t100.members.empty());
    assert(ws.spectra.members.size() == 1);                    // original ws untouched
    printf("roundtrip: pruneStale OK\n");
}

void test10_markDependentsStaleCascade() {
    Workspace ws = buildDerivativeWorkspace();
    // t100 references the average; snr references only the original.
    ws.t100.members[0].config =
        R"({"inputs":["/igm_uncorrected_x/record_0"],"reference":{"source":"average","path":"/average_spectra/average"}})";

    auto affected = markDependentsStale(ws, "/average_spectra/average");
    assert(std::find(affected.begin(), affected.end(), "/t100/t100") != affected.end());
    assert(ws.t100.members[0].stale);
    assert(!ws.snrSpectra.members[0].stale);
    assert(ws.averageSpectra.members[0].kind == MemberKind::Derivative);  // itself not stale
    assert(memberPathIsStale(ws, "/t100/t100"));
    assert(!memberPathIsStale(ws, "/snr_spectra/snr"));
    printf("roundtrip: markDependentsStale cascade OK\n");
}

void test11_timestampHelpers() {
    assert(timestampHMS("2026-08-01T12:34:56Z") == "12:34:56");
    assert(timestampHMS("") == "");
    assert(timestampHMS("no-time") == "");
    assert(timestampHMS("2026-08-01") == "");
    assert(timestampHMS("2026-08-01T12:34:56+02:00") == "12:34:56");

    Workspace ws;
    InterferogramMember orig;
    orig.id = "record_0";
    orig.kind = MemberKind::Original;
    orig.timestamp = "2026-08-01T12:34:56Z";
    ws.uncorrectedIfg.members.push_back(orig);

    InterferogramMember noTs;
    noTs.id = "record_1";
    noTs.kind = MemberKind::Original;   // original but empty timestamp
    ws.uncorrectedIfg.members.push_back(noTs);

    InterferogramMember der;
    der.id = "record_2";
    der.kind = MemberKind::Derivative;  // derivative (timestamp ignored)
    der.timestamp = "2026-08-01T09:00:00Z";
    ws.uncorrectedIfg.members.push_back(der);

    TwoColumnMember specOrig;
    specOrig.id = "orig_spectrum";
    specOrig.kind = MemberKind::Original;
    specOrig.timestamp = "2026-08-02T01:02:03Z";
    ws.spectra.members.push_back(specOrig);

    TwoColumnMember specDeriv;
    specDeriv.id = "spec_record_0";
    specDeriv.kind = MemberKind::Derivative;
    specDeriv.timestamp = "2026-08-02T04:05:06Z";
    ws.spectra.members.push_back(specDeriv);

    assert(memberTimestampHMS(ws, "record_0") == "12:34:56");
    assert(memberTimestampHMS(ws, "record_1") == "");        // empty timestamp
    assert(memberTimestampHMS(ws, "record_2") == "");        // derivative
    assert(memberTimestampHMS(ws, "missing") == "");         // absent id
    assert(memberTimestampHMS(ws, "orig_spectrum") == "01:02:03");
    assert(memberTimestampHMS(ws, "spec_record_0") == "");   // derivative

    assert(isOriginalSpectraMember(ws, "orig_spectrum"));
    assert(!isOriginalSpectraMember(ws, "spec_record_0"));   // derivative
    assert(!isOriginalSpectraMember(ws, "record_0"));        // IFG id, not spectra
    printf("roundtrip: timestamp helpers OK\n");
}

void test12_specMemberFixedId() {
    // Regression: wsUpsertSpectrum must erase-then-push the FIXED id
    // spec_<ifgId> on recompute. makeUniqueId would produce a suffixed twin
    // (spec_<ifgId>_2), leaving the old member behind — then
    // findSpectrumMember (exact spec_<ifgId>) keeps seeing the stale one and
    // ifgIdFromSpectrumMember parses the twin's id wrong, so every recompute
    // looks stale and gets pruned at Save (spectra never persisted).
    Workspace ws = buildDerivativeWorkspace();   // one spec_record_0
    assert(ws.spectra.members.size() == 1);

    // Recompute: replace spec_record_0 with a fresh member of the SAME id.
    ws.spectra.members.erase(
        std::remove_if(ws.spectra.members.begin(), ws.spectra.members.end(),
                       [](const TwoColumnMember& m) { return m.id == "spec_record_0"; }),
        ws.spectra.members.end());
    TwoColumnMember fresh;
    fresh.id = "spec_record_0";
    fresh.kind = MemberKind::Derivative;
    fresh.x = {1.0, 2.0};
    fresh.y = {3.0, 4.0};
    fresh.columns = {"x", "y"};
    fresh.units = {"cm-1", "V"};
    fresh.config = R"({"inputs":["/igm_uncorrected_x/record_0"]})";
    ws.spectra.members.push_back(fresh);

    assert(ws.spectra.members.size() == 1);
    assert(ws.spectra.members[0].id == "spec_record_0");
    // The bridge's ifgId derivation (substr after "spec_") round-trips.
    assert(ws.spectra.members[0].id.substr(5) == "record_0");
    // Document the old bug: a unique-id generator would have made a twin.
    assert(makeUniqueId("spec_record_0", {"spec_record_0"}) == "spec_record_0_2");

    // The fixed-id member persists and reloads losslessly.
    const char* tmpFile = "/tmp/fts_hdf_roundtrip_specid.h5";
    H5Store::save(tmpFile, ws);
    Workspace back = H5Store::load(tmpFile);
    assert(back.spectra.members.size() == 1);
    assert(back.spectra.members[0].id == "spec_record_0");
    assert(back.inputsAreValid());
    std::remove(tmpFile);
    printf("roundtrip: spectrum fixed-id upsert OK\n");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        test1_handBuiltRoundTrip();
        test4_schemaAndMemberOrigin();
        test5_derivativeRoundTrip();
        test6_deletedOriginalAuthorization();
        test7_unauthorizedOriginalDeletionThrows();
        test8_danglingT100RefPath();
        test9_pruneStale();
        test10_markDependentsStaleCascade();
        test11_timestampHelpers();
        test12_specMemberFixedId();

        if (argc > 1) {
            test2_pythonExampleRoundTrip(argv[1]);
            test3_originalDataProtection(argv[1]);
        } else {
            printf("skipping python-example tests (no .h5 path argument)\n");
        }
        printf("ALL TESTS PASSED\n");
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
}
