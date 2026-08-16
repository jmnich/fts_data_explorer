#pragma once
#include <iterator>

// Strong-absorption spectral ranges (cm-1) of common atmospheric gases,
// derived from HITRAN line data via hitran/generate_gas_bands.py (integrated
// line strength per 1 cm-1 bin, >=2% of each gas's peak band intensity).
// Quick-check aid only — pure data, no engine coupling.
struct HitranBand { double cmMin, cmMax; };

#include "gas_h2o.h"
#include "gas_co2.h"
#include "gas_ch4.h"
#include "gas_o3.h"
#include "gas_n2o.h"
#include "gas_co.h"
#include "gas_no.h"
#include "gas_no2.h"

// One gas's marker metadata: label, fixed palette color (0xAARRGGBB, drawn
// directly as ImU32), and its band list. Index i <-> the toggles in the
// "HITRAN Gas Markers" panel (std::array<bool,8> hitranGasEnabled).
struct HitranGas {
    const char* name;
    unsigned color;
    const HitranBand* bands;
    int count;
};

inline constexpr int kHitranGasCount = 8;
inline const HitranGas kHitranGases[kHitranGasCount] = {
    { "H2O", 0xFF4C72B0, kGasH2oBands, static_cast<int>(std::size(kGasH2oBands)) },
    { "CO2", 0xFFDD8452, kGasCo2Bands, static_cast<int>(std::size(kGasCo2Bands)) },
    { "CH4", 0xFF55A868, kGasCh4Bands, static_cast<int>(std::size(kGasCh4Bands)) },
    { "O3",  0xFFC44E52, kGasO3Bands,  static_cast<int>(std::size(kGasO3Bands))  },
    { "N2O", 0xFF8172B3, kGasN2oBands, static_cast<int>(std::size(kGasN2oBands)) },
    { "CO",  0xFF937860, kGasCoBands,  static_cast<int>(std::size(kGasCoBands))  },
    { "NO",  0xFFCCB974, kGasNoBands,  static_cast<int>(std::size(kGasNoBands))  },
    { "NO2", 0xFF64B5CD, kGasNo2Bands, static_cast<int>(std::size(kGasNo2Bands)) },
};
