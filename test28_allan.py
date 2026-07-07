import csv
import os
import numpy as np
from scipy.signal import find_peaks
from scipy import signal
from scipy.signal import hilbert
import matplotlib.pyplot as plt
import matplotlib as mpl
import spectral_toolbox
import random



def oawvar(data,dt=1):
    """
    Overlapping Allan-Werle Variance 
    (the same notation as in the paper of Werle is used)
    can be inefficient computationally (although it is not crucial here), 
    but transparent and straight forward implementation
    data - input spectroscopic data
    dt - sampling rate
    """
    #OVERLAPPING Allan-Werle variance
    n=len(data)
    clusters=np.unique(np.arange(0,int(np.round(n/2)))+1).astype(np.int64)
    oawvar=[]
    
    for clus in clusters:
        M=n
        diff=[]
        for j in range (0,M-2*clus):
            as0=np.mean(data[j:j+clus])
            as1=np.mean(data[j+clus:j+2*clus])           
            diff.append((as1-as0))
        arr = np.array(diff, dtype=np.float64)
        if arr.size == 0 or np.isnan(arr).all():
            oawvar.append(np.nan)
        else:
            oawvar.append(np.nanmean(arr**2)/2)
    taus=clusters*dt
    return oawvar, taus


def processSpectrum(igmX, igmY, K, phaseCorrWindow, apodizationWindow, rotateIGMs, envelopeRotation, spectrumXMin, spectrumXMax):
    
    # note: this is a unified spectrum processing procedure designed for experimentation with algorithms
    # igmX, igmY - already corrected, uniformly sampled interferogram with X scaled in [um]
    # rotateIGMs - never disable, this is for injecting error

    # === outputs ===
    phToolIGM = None
    phaseCorrectionToolSpectrum = None
    sinCurve = None
    cosCurve = None
    phCurve = None
    corrSpectrum = None
    reconstructedIGM = None
    
    # === processing ===
    # prepare x-axis for the spectrum
    spectrumX = np.arange(1, len(igmX) * (K + 1) + 1)
    OPD = 2 * np.max(igmX)
    spectrumX = ((OPD * (K + 1)) / spectrumX)

    igmLenBeforePadding = len(igmX)
    
    # zero pad
    igmYPad = np.concatenate((igmY, np.zeros(len(igmY) * K)))


    # find the peak and mark indices for phase correction window
    maxidx = spectral_toolbox.find_nearest(igmYPad, np.max(igmYPad))
    lowidx = int(maxidx - (igmLenBeforePadding * (phaseCorrWindow/2.0)))
    highidx = int(maxidx + (igmLenBeforePadding * (phaseCorrWindow/2.0)))

    # mask the phase correction window
    mask = np.zeros(len(igmYPad))
    mask[lowidx:highidx] = 1.0

    dum = igmYPad * mask
    dum = spectral_toolbox.rotateInterferogramExtended(dum, rotateIGMs, envelopeRotation)
    phToolIGM = dum

    # calculate the tool spectrum - low resolution spectrum used for phase correction
    phSpectrum = np.fft.fft(dum)
    phaseCorrectionToolSpectrum = phSpectrum
    # unwrap the phase curve for the complex tool spectrum
    ph = np.angle(phSpectrum)
    ph = np.unwrap(ph)
    
    # these are the actual phase correction curves
    sin_curve = np.sin(ph)
    cos_curve = np.cos(ph)

    sinCurve = sin_curve
    cosCurve = cos_curve

    phCurve = ph

    # apodize, calculate and correct the actual spectrum    
    window = np.concatenate((spectral_toolbox.createAssymetricApodizationWindow(igmY, apodizationWindow), np.zeros(len(igmY) * K)))
    interferogramDataY = igmYPad * window

    interferogramDataY = spectral_toolbox.rotateInterferogramExtended(interferogramDataY, rotateIGMs, envelopeRotation)

    # correct the actual spectrum    
    spectrum = np.fft.fft(interferogramDataY)    
    spectrum_mag = np.abs(spectrum)
    spectrum_corrected = spectrum.real * cos_curve + spectrum.imag * sin_curve

    corrSpectrum = spectrum_corrected
    reconstructedIGM = np.fft.ifft(corrSpectrum)


    # cut all frequency-domain data to cover only the range of interest
    idx_max = spectral_toolbox.find_nearest(spectrumX, spectrumXMin)
    idx_min = spectral_toolbox.find_nearest(spectrumX, spectrumXMax)

    # Trim and reverse to ascending order (np.interp requires sorted xp)
    phCurve = phCurve[idx_min:idx_max][::-1]
    corrSpectrum = corrSpectrum[idx_min:idx_max][::-1]
    spectrum_mag = spectrum_mag[idx_min:idx_max][::-1]
    sinCurve = sinCurve[idx_min:idx_max][::-1]
    cosCurve = cosCurve[idx_min:idx_max][::-1]
    phaseCorrectionToolSpectrum = phaseCorrectionToolSpectrum[idx_min:idx_max][::-1]
    spectrumX = spectrumX[idx_min:idx_max][::-1]

    # convert to dictionary
    data_dict = {
        "spectrumX": spectrumX,
        "spectrumY": corrSpectrum,
        "spectrumYMag": spectrum_mag,
        "phaseCorrectionToolIGM": phToolIGM,
        "phaseCorrectionToolSpectrum": phaseCorrectionToolSpectrum,
        "phaseCorrectionSinCurve": sinCurve,
        "phaseCorrectionCosCurve": cosCurve,
        "phaseCurve": phCurve,
        "reconstructedIGM":reconstructedIGM
        }

    return data_dict


# dataset_name = "2024-06-18_11-01-03_prno2_5mm_1mms_avg100"
# dataset_name = "2025-04-16_11-49-30_ref2_diamond_mrpd_globar_1mm_0.2mms_avg30"
dataset_name = "2024-10-28_16-19-53_diamond_mrpd_combined_source_gascell_empty_ambient_5mm_0.5mms_avg100"
# paths_to_dataset = "/home/guowa/tmp_test_data/"
paths_to_dataset = "/home/jakub/tmp_test_data/"
ref_laser_wavelength = 1.548339 #um
detector_sensitivity = 3.725E3 # MRPD rng0

filter_interferograms = False
interferogram_decimation_factor = 1
# clip from both ends
samples_to_clip_after_filtering = 50

spectrum_config_x_min = 0.5
spectrum_config_x_max = 10000

# Selected wavelengths for Allan variance analysis (in um)
# These should be between 1 and 30 um
selected_wavelengths_um = [
    2.0,
    3.5,
    5.0,
    7.5,
    10.0,
    12.5,
    15.0,
    17.5,
    20.0,
    25.0
]

limit_averaged_interferograms = -1 # -1 to use all

# signal chain
external_divider = 1
external_gain = 1


# load data from file
loadedStuff = spectral_toolbox.loadDataset(
    path = paths_to_dataset + "/" + dataset_name,
    limit_averaged_interferograms = limit_averaged_interferograms,
    interferogram_decimation_factor = interferogram_decimation_factor,
    filter_interferograms = filter_interferograms,
    samples_to_clip_after_filtering = samples_to_clip_after_filtering,
    external_divider = external_divider,
    external_gain = external_gain)

raw_ref_interferograms = loadedStuff[0]
raw_meas_interferograms = loadedStuff[1]
comments_lines = loadedStuff[2]
measSettingsDictionaries = loadedStuff[3]

# Use all available interferograms
igm_index_from = 0
igm_index_to = len(raw_meas_interferograms) - 1

# create X axis for each interferogram, corrected for positioning errors and scaled in microns
hilbert_phases = spectral_toolbox.calculateXAxisFromHilbertTransform(raw_ref_interferograms, ref_laser_wavelength)

K = 10

uniformX = []
uniformY = []

for i in range(igm_index_from, igm_index_to + 1):
    correctedX = hilbert_phases[i]
    uniformX.append(np.linspace(start=0, stop=np.max(correctedX), num=len(correctedX), endpoint=True))
    uniformY.append(np.interp(uniformX[i], correctedX, raw_meas_interferograms[i]))

phaseCorrectionWindowWidth = 0.9
rotate_igms = False

# Use a single apodization window for SNR calculation
apodizationWindow = "nb_weak"

# Process all interferograms with the same apodization window
outputs = []
for i in range(igm_index_from, igm_index_to + 1):
    result = processSpectrum(
        igmX=uniformX[i], 
        igmY=uniformY[i], 
        K=K, 
        phaseCorrWindow=phaseCorrectionWindowWidth,
        apodizationWindow=apodizationWindow,
        rotateIGMs=rotate_igms,
        envelopeRotation=False,
        spectrumXMin=spectrum_config_x_min,
        spectrumXMax=spectrum_config_x_max
    )
    outputs.append(result)

# Collect all spectra for SNR calculation
# We'll use the magnitude spectra (spectrumYMag) for SNR calculation
all_spectra = []
common_spectrumX = outputs[0]["spectrumX"]  # Use first spectrum's X axis as reference

for output in outputs:
    # Interpolate to common X axis
    interpolated_spectrum = np.interp(common_spectrumX, output["spectrumX"], output["spectrumYMag"])
    all_spectra.append(interpolated_spectrum)

# Stack all spectra into a 2D array (num_interferograms x num_wavelengths)
all_spectra_array = np.array(all_spectra)

# Calculate mean and standard deviation across all interferograms for each wavelength
mean_spectrum = np.mean(all_spectra_array, axis=0)
std_spectrum = np.std(all_spectra_array, axis=0)

# Calculate SNR (Signal-to-Noise Ratio)
# SNR = mean / std_dev (avoid division by zero)
snr = np.zeros_like(mean_spectrum)
valid_indices = std_spectrum > 0
snr[valid_indices] = mean_spectrum[valid_indices] / std_spectrum[valid_indices]
snr[~valid_indices] = 0  # Set to 0 where std is 0 (no noise)

# ============================================
# ALLAN VARIANCE ANALYSIS FOR SIGNAL LEVEL
# ============================================

# Extract signal levels at selected wavelengths for all interferograms
signal_at_wavelengths = []
for wl in selected_wavelengths_um:
    # Find the index closest to the selected wavelength
    idx = spectral_toolbox.find_nearest(common_spectrumX, wl)
    # Extract signal level from all spectra at this wavelength
    signals = all_spectra_array[:, idx]
    signal_at_wavelengths.append(signals)

# Calculate Allan variance for each selected wavelength
allan_results = []
for i, wl in enumerate(selected_wavelengths_um):
    signals = signal_at_wavelengths[i]
    avar, tau = oawvar(signals)
    allan_results.append({
        'wavelength_um': wl,
        'tau': tau,
        'avar': avar,
        'signal_mean': np.mean(signals),
        'signal_std': np.std(signals)
    })

# Create Allan variance plot
plt.figure(figsize=(12, 8))

# Plot each wavelength's Allan deviation
colors = plt.cm.viridis(np.linspace(0, 1, len(selected_wavelengths_um)))
for i, result in enumerate(allan_results):
    wl = result['wavelength_um']
    avar = result['avar']
    tau = result['tau']
    
    plt.loglog(tau, avar, 'o-', color=colors[i], linewidth=1.5, 
               markersize=4, label=f'{wl:.1f} μm')

plt.xlabel('Integration Time (measurements)')
plt.ylabel('Allan Variance')
plt.title('Allan Variance of Signal Level at Selected Wavelengths')
plt.grid(True, which='both', alpha=0.3)
plt.legend(title='Wavelength', loc='best', fontsize=8)
plt.tight_layout()

# Plot signal values at selected wavelengths over time
plt.figure(figsize=(12, 8))
measurement_indices = np.arange(len(signal_at_wavelengths[0]))

for i, wl in enumerate(selected_wavelengths_um):
    signals = signal_at_wavelengths[i]
    plt.plot(measurement_indices, signals, 'o-', color=colors[i], 
             linewidth=1.5, markersize=4, label=f'{wl:.1f} μm')
    
    # Fit a line to the signal data
    slope, intercept = np.polyfit(measurement_indices, signals, 1)
    fit_line = slope * measurement_indices + intercept
    plt.plot(measurement_indices, fit_line, '--', color=colors[i], 
             linewidth=1, alpha=0.7)

plt.xlabel('Measurement Index')
plt.ylabel('Signal Level (a.u.)')
plt.title('Signal Level at Selected Wavelengths vs Measurement')
plt.grid(True, which='both', alpha=0.3)
plt.legend(title='Wavelength', loc='best', fontsize=8)
plt.tight_layout()

# Convert X axis from um to cm for plotting
spectrumX_cm = spectral_toolbox.convertUMtoCM(common_spectrumX)

# Plot SNR curve
plt.figure(figsize=(10, 6))
plt.plot(spectrumX_cm, snr, 'b-', linewidth=1.5, label='SNR', color='#C00E0E')
plt.xlabel('Wavenumber (cm$^{-1}$)')
plt.ylabel('SNR')
# plt.title('Signal-to-Noise Ratio (SNR) Curve')
plt.xscale('log')
plt.yscale('log')
plt.xlim((150, 20000))
plt.xticks(
    ticks=[150, 1000, 10000],
    labels=["150", "1000", "10000"]
)
plt.grid(True, which='both', alpha=0.3)
# plt.legend()
# plt.show()

# Optional: Plot mean spectrum with std dev for reference
plt.figure(figsize=(10, 6))
plt.plot(spectrumX_cm, mean_spectrum, 'b-', linewidth=1.5, label='Mean Spectrum')
plt.fill_between(spectrumX_cm, mean_spectrum - std_spectrum, mean_spectrum + std_spectrum, 
                 alpha=0.3, color='blue', label='±1 Std Dev')
plt.xlabel('Wavenumber (cm$^{-1}$)')
plt.ylabel('Intensity (a.u.)')
plt.title('Mean Spectrum with Standard Deviation')
plt.xscale('log')
plt.yscale('log')
plt.xlim((150, 20000))
plt.xticks(
    ticks=[150, 1000, 10000],
    labels=["150", "1000", "10000"]
)
plt.grid(True, which='both', alpha=0.3)
plt.legend()
plt.show()

print(f"Processed {len(outputs)} interferograms")
print(f"Number of spectral points: {len(common_spectrumX)}")
print("Done")
