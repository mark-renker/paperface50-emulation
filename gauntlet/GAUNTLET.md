# GAUNTLET — Paperface 50 emulation (FROZEN 2026-08-23, pre-implementation)

Variant: **Verifiable Gauntlet** (gauntlet-loop skill). This file is frozen
before the first line of implementation code. Checks may be strengthened, never
weakened; any change is logged in LOG.md with a reason.

## Goal
Software emulation of the Serge Paperface 50th Anniversary system — all three
panels (Helios, Paris-Flash, Tempo di Roma) — as native macOS arm64 AU + VST3:
"Paperface 50" (instrument, aumu) and "Paperface 50 FX" (effect, aufx), with
full banana patching and panel UI faithful to the official manual drawings.
Installed to ~/Library/Audio/Plug-Ins for Ableton Live.

## Hard gates (all must pass; executable)

- **G1 build-linux** — engine + headless test target compile clean (-Wall) in
  the cloud container; VST3 target compiles under JUCE 8 on Linux.
- **G2 dsp-suite** — headless test suite (tests/GauntletTests.cpp), every
  assertion green:
  - T01 silence: unpatched engine renders exact 0; master path denormal-free.
  - T02 vco-tracking: 1V/oct error ≤ 3 cents across −3..+3 V around 261.63 Hz.
  - T03 vco-waves: saw/sine outs bounded, periodic, correct unipolar/bipolar
    ranges (white 0..5 V, black ±2.5 V nominal).
  - T04 negslew-cycle: pulse→input self-cycle oscillates; top section reaches
    ≥ 2.5 kHz at max rate and ≤ 0.15 Hz at min; pulse fires at 0 V; wildcard
    2.5 V ≈ linear, 0/5 V bend the slope.
  - T05 posslew-env: START trigger rises to +5 V within ±25% of dialed time,
    emits pulse at top, SUSTAIN holds, falls when released.
  - T06 ssg: smooth cycle mode yields 0..~5 V triangle; rate CV moves freq;
    HOLD freezes (track-and-hold); stepped side staircases on SAMPLE pulses in
    cycle mode; coupler red = 0/5 comparator, black hot swings ≥ ±10 V.
  - T07 vcf-1973: LP slope 12 ± 2.5 dB/oct measured 1→3 octaves above cutoff;
    HP mirror; BP peak at cutoff; stable (finite) at max Q under fast FM;
    LOW range toggle drops range by ≥ 4 octaves.
  - T08 gates: DC mode linear in VC (r² > 0.99); AC mode monotonic-expo, full
    CCW ≤ −60 dB; MOD mode sine×sine suppresses carrier ≥ 30 dB vs sidebands.
  - T09 sequencers: gate-seq one-hot advance 1→8 wrap, RESET to 1, HOLD
    freezes; SEQ4 A/B/C emit stage knob voltages, UP/DOWN direction obeys
    cable-present semantics, SELECT jumps stage, per-stage + common gates.
  - T10 logic: PEAK=max, TROUGH=min (OR/AND on 0/5 logic); router OFF connects
    A0↔B, ON connects A1↔B, momentary vs flip-flop; FLOP inverts; flip-flop
    divides a clock by 2.
  - T11 envelope: END fires only when START-triggered; END→START self-cycles
    at RISE+FALL-dependent freq; WINDOW pulse start tracks SIZE knob; HOLD
    freezes; retrigger ignored while running.
  - T12 feedback-stress: pathological cross-panel feedback patch, 10 s @48 kHz:
    no NaN/Inf, all buses within ±12 V, output bounded.
  - T13 state-roundtrip: params + cables serialize → restore → identical
    graph, values, and audio (first 512 samples bit-comparable).
  - T14 perf: full-system default patch renders ≥ 4× realtime single-thread in
    the cloud container (proxy for ≤ 25% of one M2 core).
  - T15 noise: white ≈ flat (±6 dB/oct trend), pink falls ~3 ± 1.5 dB/oct
    (100 Hz→6.4 kHz trend), S/H SRC bounded 0..5 V and non-repeating.
  - T16 cvpro: attenuvert sign flip exact, offset 0..5 V, soft-limit engages
    above ~±8 V and never hard-clips to rails.
  - T17 tws: triangle in → dominant output component at 2F (full-wave rect
    signature); VC inputs change spectrum; outputs bounded.
  - T18 clock: metronome freq within ±2% of knob law; FINE span ≤ ±3%.
- **G3 build-mac** — clean arm64 build of AU + VST3 + Standalone on the target
  Mac (Xcode 26 toolchain, JUCE 8.0.6).
- **G4 auval** — `auval -v aumu Pf50 Bktv` exits PASS. (`-strict` recorded in
  the log as informational.)
- **G5 install** — .component + .vst3 (both variants) present in
  ~/Library/Audio/Plug-Ins/{Components,VST3} with correct bundle structure.

## Soft bar (blind critic)

Reference: the official Paperface 50 manual panel drawings (pages 8–24,
rendered at /root/pf50/manual/page-*.png) and serge-modular.com photos.
Judging dimensions, ranked: (1) module layout & grouping per panel row,
(2) jack color code (white/red/black/purple/grey + LED treatment), (3) knob &
button shapes, (4) section boxes / trapezoid & diamond graphic devices,
(5) lettering style & labels, (6) overall panel proportions & cream finish.
Procedure: fresh critic subagent receives UI screenshot + manual drawings,
unlabeled order, names winner + 2–3 largest gaps with pixel-level evidence.
Exit: critic prefers ours or declares evidenced parity *for a software
recreation* (hardware-only properties — engraving depth, physical banana
stacking — are out of scope, logged below).

## Out of scope (declared now, not negotiated later)
TKB panel (v2); balanced TRS electrical behavior (BAL OUT = master-bus tap);
component-level analog nonidealities beyond behavioral emulation; PSU.

## Stop conditions
Win (all gates green + critic verdict); 5 rounds on any single piece → pause
for Mark; two rounds of unchanged gaps → pause; wrong-bar findings logged and
re-scoped only with consent.
