# Paperface 50 — Panel & Module Spec (extracted from the official manual v0.1, Dec 2023)

Signal conventions (manual pp. 5–7): WHITE jacks = DC control voltages 0..+5 V.
RED = logic/trigger pulses (rising edge triggers; level defines sustain).
BLACK = bipolar/audio, typically ±2.5 V, system range ±12 V. Special colors:
PURPLE = VCO sync, GREY = any-signal (router A0/A1/B, P&T inputs), ORANGE
concentric ring = LED indicator. System rails ±12 V; everything freely mixable.

## Panel rows (serge-modular.com/paperface)
- **HELIOS (voice, 17")**: OSCILLATOR (2") · OSCILLATOR+ (3") · SSG (3") ·
  DUAL CV-PRO (3") · TRIPLE WAVESHAPER (3") · 1973 VCF + BAL OUT (2")
- **PARIS-FLASH (slopes, 17")**: POSITIVE SLEW (3") · NEGATIVE SLEW (2") ·
  SSG (3") · ENVELOPE (2") · DUAL CV-PRO (3") · DUAL GATES (3") · 1973 VCF +
  BAL OUT (2")  [site lists 7 modules]
- **TEMPO DI ROMA (sequencing, 17")**: GATE SEQUENCER (2") ·
  SEQ4 + METRONOMIC CLOCK (6") · BI-DIRECTIONAL ROUTERS (2") ·
  SEQ4 + PEAK & TROUGH (6")
- **I/O strip (software addition, visually distinct)**: MIDI 1V/O · GATE ·
  VEL · CLK (host) · AUDIO IN L/R (FX build) · MAIN OUT L/R · VOLUME.

## Modules

### OSCILLATOR (VCO) — p8
Outs: saw white(0..5)+black(±2.5); sine-like white+black. Shape knob + WAVE
SHAPE CV (white, processor −/+). Freq: FREQ knob, 1V/O (white), 2× CV in
(white) with attenuverters (leftmost = fine ± major third when unpatched).
SYNC (purple): hard sync → harmonic-series lock. Range ~13 Hz–4.2 kHz + LFO
via CV. Tracking: temperature-compensated 6+ octaves (2022 redesign).

### OSCILLATOR+ (VCO+) — p9
VCO plus: PULSE out (red) w/ PULSEWIDTH knob + VC PWM (white, attenuator).
NOISE: WHITE (black), PINK (black), S/H SRC (white 0..5, for SSG random).

### SMOOTH & STEPPED GENERATOR — pp10–11
SMOOTH: IN (white), VC RATE (white, attenuverter), RATE knob, CYCLE (red,
Schmitt pulse out; patch to IN → 0..~5 V triangle, sub-1 Hz…~4 kHz), HOLD
(red; high = track-and-hold freeze), OUT (black+LED). Acts as slew/lag/LPG.
STEPPED: IN (white), VC RATE, RATE knob, SAMPLE (red; slews toward IN while
high → S&H / staircase), CYCLE (red pulse out), OUT (white+LED).
COUPLER: comparator smooth>stepped: red out 0/5 V; black "hot" out −12..+12 V.

### DUAL CV-PRO — p12
2 sections: 3 white ins w/ attenuverters (diamond graphics), OFFSET knob
0..+5 V, OUT (white) + level LED. Soft amplitude compression >≈16 Vpp.

### TRIPLE WAVESHAPER — p13
3 cascadable sections: IN, VC1, VC2 (whites), input attenuator knob (VCs
equally attenuated), full-wave-rectifier + nonlinearity core (tri→sine @2F,
saw→~sine @1F), outs white (0..5) + black (AC-coupled).

### 1973 VCF + BAL OUT — p14
BAL OUT: IN (black) → TRS graphic = tap to master output bus.
VCF: IN (black, GAIN knob), VC FREQ (white, attenuverter), FREQ knob, Q knob
(manual only, lowest full-left), outs LOW/HIGH/BAND (black), 12 dB/oct,
common center freq; LOW range toggle. Sine-osc via BP→IN feedback at high Q;
water-drops via fast CV at high Q, input attenuated.

### POSITIVE SLEW — p15 (dual sections)
Follows falling exactly, slews rising. IN (white), VC (white, processor −/+),
rate knob, START (red trigger → envelope rise to +5), SUSTAIN (red, hold at
+5 while high), pulse out (red ⊓, fires on reaching +5 V) + inverted pulse
(red ⊔), OUT black (diamond) + white (ramp glyph, LED). Uses: rise-only env,
monostable, VC pulse delay, saw osc, sub-harmonics.

### NEGATIVE SLEW — p16 (dual; top fast 10 s–3 kHz, bottom to 20 s)
Follows rising exactly, slews falling; pulse (red ⊓) at 0 V; PULSE→IN =
inverted-saw osc. IN, VC (processor −/+), rate knob, OUT black + white (LED).
WILDCARD (white, per section): slope shape 0..5 V — 2.5 linear → expo/log
extremes (affects period; usable as waveshaper on tracked input).

### ENVELOPE — p17
END (red out, trigger at end, only when START-triggered), START (red in;
ignored while running), CYCLE (red out; patch to START to loop → AR LFO),
HOLD (red in, freeze), SIZE (white VC + knob → WINDOW start point), WINDOW
(red out, high from window point to end; ⊓ glyph + LED), DURATION (white VC
w/ processor −/+ scaling both slopes), RISE knob, FALL knob, OUT (white+LED,
AR curve). Sub-harmonic generator at audio-rate triggering.

### DUAL GATES (2023) — p18
2 sections: IN (black), GAIN knob, mode toggle DC/AC/MOD, LIN VC (white),
LOG VC (white), MOD (black), OUT (black, diamond). DC = linear VCA; AC =
expo audio VCA; MOD = 4-quadrant multiply (ring at full GAIN); inputs usable
across modes.

### GATE SEQUENCER — p19 ("SEQUENCER")
8 red pulse outs (2 cols × 4 rows, LED under each), one-hot +5 V.
Counter-clockwise: stage 1 top-left → down left col (1–4), up right col
(5–8 top-right). CLOCK (red), RESET (red + push button), HOLD (red).

### SEQ4 — pp20–21 (×2, one w/ CLOCK sub-module, one w/ PEAK&TR)
4 stages × 3 rows (A/B/C knobs 0..5 V). Per stage: SELECT in (red, top),
per-stage GATE out (red, above knobs... top row), push button + LED (bottom).
Right column: common GATE out (red, high while button/SELECT held), A, B, C
(white outs), CLOCK, RESET, HOLD, UP/DOWN (red ins). No cable in UP/DOWN →
runs L→R; cable: HIGH = L→R, LOW = R→L. Sequence starts at last button stage.

### METRONOMIC CLOCK — p22 (sub-module of SEQ4 #1)
METRONOME red pulse out + LED; TEMPO knob + FINE knob (tiny range). Short
trigger chain, temperature-stable.

### BI-DIRECTIONAL ROUTERS — p23
3 sections: A1 (grey), B (grey), A0 (grey), SWITCH (red). OFF/low: A0↔B.
ON/high: A1↔B. Sections 1–2 momentary (follow SWITCH level); section 3 =
flip-flop (toggle per pulse) + FLOP out (red, inverted state; clock divider).
A0/A1/B carry any signal type, bidirectional.

### PEAK & TROUGH — p24 (sub-module of SEQ4 #2)
2 grey ins (diamonds), PEAK out (white) = max, TROUGH out (white) = min.
Logic OR / AND on 0/5 pulses.

## Visual language (manual drawings, for the UI)
Cream panel (#EFEAE0-ish) with thin black module frames; black section boxes
grouping related controls; module title centered top in clean geometric sans
caps; labels in small caps under jacks; knobs = black 8-lobed rosettes with
white pointer line; jacks = filled circles w/ darker center dot (colors per
code above); LEDs = orange concentric rings; push buttons = large red circles
with metal ring; toggle = vertical lever graphic; "processor" scales drawn as
`−  +` bracket lines under knobs; trapezoid link-lines connect knobs to their
jacks; diamond outlines mark processed/comparator inputs; BAL OUT drawn as
concentric TRS jack; arrow glyph between IN → BAL OUT.
