#pragma once

#include <string>

enum class DataType {
    UncorrectedDualIFG,   // WUST Mini FTS: dual detectors, needs Hilbert OPD correction
    CorrectedSingleIFG,   // ArcOptix IGMs: OPD already in meters, single detector
    PrecomputedSpectra    // ArcOptix Spectra Sequence: spectra only, no interferograms
};

struct DatasetInfo {
    DataType dataType;

    bool hasInterferograms      = false;
    bool hasReferenceChannel    = false;
    bool axisIsCorrected        = false;
    bool hasPrecomputedSpectra  = false;
    bool hasMetadataFile        = false;

    std::string adapterName;
};
