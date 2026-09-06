#pragma once
#include <cstdint>
#include <iterator>

// Raw 1 cm-1 line-strength envelopes of common atmospheric gases, derived from
// HITRAN line data via hitran/generate_gas_bands.py (integrated 296 K line
// strength per 1 cm-1 bin, normalized x10000 to the gas's peak bin). Quick-
// check aid only — pure data, no engine coupling. The app applies runtime
// smoothing + thresholding (hitran_panel.cpp) to draw band markers.
struct HitranBand { double cmMin, cmMax; };

// One strong transition: exact wavenumber (cm-1) and strength relative to the
// gas's strongest line x10000 (uint16). Committed lists are sorted by nuCm1.
struct HitranLine { float nuCm1; std::uint16_t swRel; };

#include "gas_h2o.h"
#include "gas_co2.h"
#include "gas_ch4.h"
#include "gas_o3.h"
#include "gas_n2o.h"
#include "gas_co.h"
#include "gas_no.h"
#include "gas_no2.h"

// Envelope grid metadata: value i covers [start + i*step, start + (i+1)*step).
inline constexpr double kHitranEnvelopeStartCm = 50.0;
inline constexpr double kHitranEnvelopeStepCm = 1.0;

// One gas's marker metadata: label, fixed palette color (0xAARRGGBB, drawn
// directly as ImU32), its raw envelope, and its committed strong-line list
// (exact transition positions; the bright peak ticks).
struct HitranGas {
    const char* name;
    unsigned color;
    const std::uint16_t* envelope;
    int envelopeBins;
    const HitranLine* lines;
    int lineCount;
};

inline constexpr int kHitranGasCount = 8;
inline const HitranGas kHitranGases[kHitranGasCount] = {
    { "H2O", 0xFF4C72B0, kGasH2oEnvelope, static_cast<int>(std::size(kGasH2oEnvelope)),
      kGasH2oLines, static_cast<int>(std::size(kGasH2oLines)) },
    { "CO2", 0xFFDD8452, kGasCo2Envelope, static_cast<int>(std::size(kGasCo2Envelope)),
      kGasCo2Lines, static_cast<int>(std::size(kGasCo2Lines)) },
    { "CH4", 0xFF55A868, kGasCh4Envelope, static_cast<int>(std::size(kGasCh4Envelope)),
      kGasCh4Lines, static_cast<int>(std::size(kGasCh4Lines)) },
    { "O3",  0xFFC44E52, kGasO3Envelope,  static_cast<int>(std::size(kGasO3Envelope)),
      kGasO3Lines,  static_cast<int>(std::size(kGasO3Lines))  },
    { "N2O", 0xFF8172B3, kGasN2oEnvelope, static_cast<int>(std::size(kGasN2oEnvelope)),
      kGasN2oLines, static_cast<int>(std::size(kGasN2oLines)) },
    { "CO",  0xFF937860, kGasCoEnvelope,  static_cast<int>(std::size(kGasCoEnvelope)),
      kGasCoLines,  static_cast<int>(std::size(kGasCoLines))  },
    { "NO",  0xFFCCB974, kGasNoEnvelope,  static_cast<int>(std::size(kGasNoEnvelope)),
      kGasNoLines,  static_cast<int>(std::size(kGasNoLines))  },
    { "NO2", 0xFF64B5CD, kGasNo2Envelope, static_cast<int>(std::size(kGasNo2Envelope)),
      kGasNo2Lines, static_cast<int>(std::size(kGasNo2Lines)) },
};

// "Strength threshold" selector: fraction of the gas's peak bin intensity.
// Level index i <-> kHitranThresholds[i].
inline constexpr int kHitranLevelCount = 4;
inline constexpr float kHitranThresholds[kHitranLevelCount] = { 0.001f, 0.01f, 0.02f, 0.10f };

// "Smoothing range" selector: running-mean width in cm-1 (1 = no smoothing).
// Level index i <-> kHitranSmoothOptions[i].
inline constexpr int kHitranSmoothLevelCount = 4;
inline constexpr int kHitranSmoothOptions[kHitranSmoothLevelCount] = { 1, 2, 5, 10 };