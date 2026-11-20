"""Paperface 50 bench — measurement math (numpy only).

Conventions
-----------
* Spectra use a Kaiser window (beta = 20, sidelobes below -180 dB) over 1 s captures
  (1 Hz bins), so components down to ~-160 dB are resolvable. Tone power is the sum of
  |X|^2 across the main lobe, normalised so a sine of amplitude A reads A^2/2.
* Levels are dB relative to each case's ``ref_amp``: +-2.5 V peak (the engine's
  nominal audio level) for module jacks, +-1.0 for the master output (dBFS).
* Aliasing is measured in the audible band [20 Hz, 20 kHz] at the 48 kHz output rate.
  Renders made at 96/192 kHz are first decimated to 48 kHz with a linear-phase FIR
  (stopband from 28 kHz, > 150 dB), which shows what 2x/4x oversampling would buy.
"""
from __future__ import annotations

import math

import numpy as np

BETA = 20.0
MASK_BINS = 10          # half-width of the masked region around each tone, in 1 Hz bins
BAND = (20.0, 20000.0)  # analysis band
FLOOR = 1e-30


def db(x: float) -> float:
    return 10.0 * math.log10(max(float(x), FLOOR))


def db_amp(x: float) -> float:
    return 20.0 * math.log10(max(abs(float(x)), 1e-15))


# --------------------------------------------------------------- resampling --
def _kaiser_lowpass(numtaps: int, cutoff: float, beta: float) -> np.ndarray:
    """Windowed-sinc lowpass; cutoff is a fraction of the input sample rate."""
    n = np.arange(numtaps) - (numtaps - 1) / 2.0
    h = 2.0 * cutoff * np.sinc(2.0 * cutoff * n) * np.kaiser(numtaps, beta)
    return h / h.sum()


def decimate_to_48k(x: np.ndarray, sr: float) -> np.ndarray:
    factor = int(round(sr / 48000.0))
    if factor <= 1:
        return x.astype(np.float64)
    # passband to 20 kHz, cutoff 24 kHz, stopband 28 kHz (folds above 20 kHz only)
    h = _kaiser_lowpass(320 * factor + 1, 24000.0 / sr, 16.0)
    y = np.convolve(x.astype(np.float64), h, mode="same")
    edge = len(h)
    return y[edge:-edge:factor]


# ----------------------------------------------------------------- spectrum --
class Spectrum:
    def __init__(self, x: np.ndarray, sr: float):
        x = np.asarray(x, dtype=np.float64)
        n = len(x)
        w = np.kaiser(n, BETA)
        X = np.fft.rfft(x * w)
        self.sr, self.n = sr, n
        self.df = sr / n
        self.freqs = np.arange(len(X)) * self.df
        self.p = 2.0 * np.abs(X) ** 2 / (n * np.sum(w * w))       # power per bin (tone sums -> A^2/2)
        self.amp = 2.0 * np.abs(X) / np.sum(w)                      # coherent amplitude per bin

    def band_mask(self, lo=BAND[0], hi=BAND[1]):
        return (self.freqs >= lo) & (self.freqs <= min(hi, self.sr / 2 - 2 * MASK_BINS * self.df))

    def tone_mask(self, f: float, half=MASK_BINS):
        k = f / self.df
        return np.abs(np.arange(len(self.p)) - k) <= half

    def tone_power(self, f: float) -> float:
        return float(self.p[self.tone_mask(f)].sum())

    def peak_freq(self, f_guess: float, search_rel=0.02) -> float:
        lo, hi = f_guess * (1 - search_rel), f_guess * (1 + search_rel)
        idx = np.where((self.freqs >= lo) & (self.freqs <= hi))[0]
        if len(idx) == 0:
            return f_guess
        k = int(idx[np.argmax(self.amp[idx])])
        if 0 < k < len(self.amp) - 1:  # Gaussian (log-parabolic) interpolation
            a, b, c = (math.log(max(self.amp[k + d], 1e-300)) for d in (-1, 0, 1))
            denom = a - 2 * b + c
            off = 0.5 * (a - c) / denom if denom != 0 else 0.0
            return (k + off) * self.df
        return k * self.df


def harmonic_analysis(x, sr, f0, *, ref_amp=1.0, estimate_f0=True, band=BAND):
    """THD, THD+N, alias-to-signal ratio and worst spur for a periodic signal.

    Harmonic grid = k*f0 inside the band. Everything else in the band (except DC) is
    counted as alias/noise; for these deterministic renders it is aliasing plus float
    rounding noise (~-150 dB)."""
    sp = Spectrum(x, sr)
    if estimate_f0:
        f0 = sp.peak_freq(f0)
    inband = sp.band_mask(*band)
    hmask = np.zeros_like(inband)
    harm_p = []
    k = 1
    while k * f0 <= band[1]:
        m = sp.tone_mask(k * f0)
        hmask |= m
        harm_p.append(float(sp.p[m & inband].sum()))
        k += 1
    p_total = float(sp.p[inband].sum())
    p_harm = float(sp.p[inband & hmask].sum())
    p_fund = harm_p[0] if harm_p else 0.0
    p_rest = max(p_total - p_harm, 0.0)
    spur_region = inband & ~hmask
    if spur_region.any():
        i = int(np.argmax(np.where(spur_region, sp.amp, 0.0)))
        spur_amp, spur_f = float(sp.amp[i]), float(sp.freqs[i])
    else:
        spur_amp, spur_f = 0.0, float("nan")
    strongest = math.sqrt(2 * max(harm_p)) if harm_p else 0.0
    xa = np.asarray(x, dtype=np.float64)
    return {
        "f0": f0,
        "fund_db": db(p_fund / (ref_amp ** 2 / 2)),
        "thd_db": db(sum(harm_p[1:]) / p_fund) if p_fund > 0 else None,
        "thdn_db": db((p_total - p_fund) / p_total) if p_total > 0 else None,
        "alias_ratio_db": db(p_rest / p_harm) if p_harm > 0 else None,
        "worst_spur_dbc": db_amp(spur_amp / strongest) if strongest > 0 else None,
        "worst_spur_hz": spur_f,
        "dc_db": db_amp(xa.mean() / ref_amp),
        "peak_db": db_amp(np.max(np.abs(xa)) / ref_amp),
        "_spectrum": sp,
    }


def tone_amp(x, sr, f):
    sp = Spectrum(x, sr)
    return math.sqrt(2 * sp.tone_power(f))


# ------------------------------------------------------------- filter (IR) --
def filter_response(ir, sr, scale):
    H = np.fft.rfft(np.asarray(ir, dtype=np.float64)) / scale
    f = np.arange(len(H)) * sr / len(ir)
    return f, H


def interp_db_at(f, mag_db, fq):
    return float(np.interp(fq, f, mag_db))


def vcf_metrics(lp, bp, hp, sr, f_nom, res, imp, gain_ss):
    """Cutoff accuracy, resonance law and slopes from impulse responses."""
    t0 = int(np.argmax(np.abs(hp) > 0))  # first sample the filter saw the impulse
    scale = imp * gain_ss
    out = {"t0": t0}
    f, Hb = filter_response(bp[t0:], sr, scale)
    _, Hl = filter_response(lp[t0:], sr, scale)
    _, Hh = filter_response(hp[t0:], sr, scale)
    mb, ml, mh = (20 * np.log10(np.maximum(np.abs(H), 1e-15)) for H in (Hb, Hl, Hh))
    # band-pass peak = natural frequency of the SVF
    k = int(np.argmax(mb[1:])) + 1
    if 1 < k < len(mb) - 1:
        a, b, c = mb[k - 1], mb[k], mb[k + 1]
        off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    else:
        off = 0.0
    f_peak = (k + off) * (f[1] - f[0])
    out["f_nominal"] = f_nom
    out["f_peak"] = f_peak
    out["cutoff_err_cents"] = 1200 * math.log2(f_peak / f_nom)
    out["bp_peak_db"] = float(b if 1 < k < len(mb) - 1 else mb[k])
    out["bp_peak_expected_db"] = 20 * math.log10(res)   # |H_bp(f0)| = 1/k = res for this SVF
    out["lp_dc_db"] = interp_db_at(f, ml, min(10.0, f_nom / 50))
    if 8 * f_nom < 0.2 * sr:
        out["lp_slope_db_oct"] = (interp_db_at(f, ml, 8 * f_nom) - interp_db_at(f, ml, 4 * f_nom))
    else:
        out["lp_slope_db_oct"] = None
    if f_nom / 8 > 5.0:
        out["hp_slope_db_oct"] = (interp_db_at(f, mh, f_nom / 4) - interp_db_at(f, mh, f_nom / 8))
    else:
        out["hp_slope_db_oct"] = None
    # -3 dB point of the low-pass
    below = np.where((f > 1.0) & (ml < out["lp_dc_db"] - 3.0))[0]
    out["lp_m3db_hz"] = float(f[below[0]]) if len(below) else None
    out["_curves"] = (f, ml, mb, mh)
    return out
