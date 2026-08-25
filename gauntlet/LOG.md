# Gauntlet round log — append-only

## R0 — 2026-08-23 — Gauntlet frozen
GAUNTLET.md written and frozen before implementation. Reference material
captured: manual text (24 pp) + panel drawings pp. 8–24 rendered to PNG;
panel/module inventory consolidated in docs/PANEL_SPEC.md. Toolchain verified
on target Mac (Xcode 26.6, CMake 4.0.1, JUCE 8.0.6 at ~/JUCE, arm64 M2 Max).
Variant: Verifiable Gauntlet. Builder: this session (cloud container for
engine + tests, device bridge for mac builds). Critics: fresh subagents at
soft-bar rounds; hard gates are executable.

## R1 — engine + G2 first run
Gates: G1 partial (engine+tests compile clean, -Wall -Wextra). G2: 65/73.
Verdict: FAIL. Named gaps:
1. T02 ×5 — test-rig defect: saw-wrap detector defeated by polyBLEP edge
   smoothing (probabilistic 77% detection ≈ observed freq ratios).
2. T04 — real DSP defect: Negative Slew could not self-start; pulse was an
   edge-fired one-shot instead of the hardware's 0 V level comparator.
3. T15 ×2 — test written as lag-1 autocorr proxy instead of the FROZEN
   spectral-trend spec; S/H source wander far too narrow (σ≈0.04 V).

## R2 — fixes applied (gauntlet unchanged)
- NegSlew pulse → level comparator at 0 V (self-starting, authentic);
  PosSlew pulse → +5 V level comparator (mirror).
- S/H SRC → bounded random walk across 0..5 V.
- T02 measurement → interpolated sine zero-crossings (method corrected to
  measure the frozen quantity; tolerance unchanged at ±3 cents).
- T15 → banded Goertzel spectral trend exactly as frozen (white ±6 dB/oct,
  pink −3 ± 1.5 dB/oct); S/H bounded+wander check unchanged.
Result: G2 = 73/73 PASS. Perf: 48× realtime single-thread (container).

## R3 — plugin layer + UI first light
G1 full: instrument VST3 + FX VST3 + snapshot tool compile clean on Linux
(JUCE 8.0.6, xvfb render). UI self-review before formal critic: fixed io-strip
height bug (7in dead face), title truncation (fitted condensed), UTF-8 mojibake
(arrow/interpunct → drawn glyph/ASCII), row nameplates → boat lower rail,
DUAL GATES mode-label collisions, PULSEWIDTH box overflow. Factory patches
render with correct cable endpoints (verified visually on patch 1).

## R4 — soft-bar critic round 1 (fresh subagent, blind vs manual drawings)
VERDICT: NO. Named gaps: (1) SSG right-column jack inventory wrong in both
instances (invented VC jacks, wrong out color, coupler order flipped);
(2) jack color code broken on ROUTER B / TWS IN & VC2 / PEAK&TROUGH, and all
amber concentric LEDs rendered as illegible dots; (3) FILTER missing the
BAL. OUT TRS jack + IN diamond; knob arches absent panel-wide. Also: SEQ4
control-pair layout, invented label boxes, METRONOME garble, missing
gate-seq fan. One critic claim rejected with evidence: DUAL GATES LOG is
already white in the engine (critic misread the MOD jack).
Fixes: engine jack colors corrected (SSG VC→black diamonds, smooth out→white,
router B→white, TWS in→white/VC2→black); coupler order black-over-red; TRS
concentric-ring graphic + fat arrow; amber ring LED painter + larger LEDs;
candy arches auto on big knobs; diamonds/bowties as drawn; SEQ4 pairs +
column dividers; label boxes only where the drawings box them (FREQ, Q);
software-only hint labels dimmed and declared. Result: G2 still 73/73.

## R5 — soft-bar critic round 2 (fresh subagent)
VERDICT: YES — "convincingly recreates the reference." Remaining polish
items applied: ENVELOPE centre jack added as a live summed duration-CV input
(function undocumented in manual v0.1 — logged interpretation, no dead
jacks); SEQ4 UP/DOWN + CLOCK/RESET label clipping fixed; METRO-NOME
two-line; PEAK&TROUGH inputs → white per drawing; DUAL GATES undeclared LEDs
removed; CV-PRO labels dimmed + arches + out→LED link; VCO compartment
divider; TWS contiguous boxes; bat-lever toggle redraw; router labels above
jacks; FLOP full-size; no arch on TEMPO. G2: 73/73 after all changes.

## R6 — final gates on target hardware (macOS arm64, M2 Max)
G1 ✓ (Linux + macOS clean builds) · G2 ✓ 73/73 (container AND native macOS)
G3 ✓ AU+VST3+Standalone, both variants, Xcode 26.6 · G4 ✓ auval PASS:
aumu Pf50 Bktv and aumf Pf5F Bktv (JUCE maps an effect with MIDI input to
kAudioUnitType_MusicEffect — 'aumf', recorded as informational)
G5 ✓ installed to ~/Library/Audio/Plug-Ins/{Components,VST3}
Soft bar ✓ critic verdict YES (R5). GAUNTLET COMPLETE — exit by WIN.
