// Assert-based conformance test for the HDF5 exchange layer. No test framework.
// Usage:
//   fts_hdf_roundtrip                  -> runs tests 1, 3, 4
//   fts_hdf_roundtrip <example.h5>     -> additionally runs test 2 (python-parser example)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

}  // namespace

int main(int argc, char** argv) {
    try {
        test1_handBuiltRoundTrip();
        test4_schemaAndMemberOrigin();

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
