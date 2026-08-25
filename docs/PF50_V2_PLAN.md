# Paperface 50 V2 — Component-Level Modeling & Roadmap (2026-08-24)

Full version with chart + linked sources: https://claude.ai/code/artifact/e6e29fd9-97ca-40c9-acfa-56005ef17180
Editable cost model: PF50_v2_cost_model.xlsx (this folder).

## Recommendation (up front)
Per-module accuracy switch on a staged ladder: T1/T2 calibrated static curves
everywhere (near-free) → T3 semi-physical for slopes/VCF/gates/VCO → T4 DK/WDF
for slopes + TWS (+VCF), with a NOVA hero block for the TWS as the A/B. Keep
the realtime path on CPU. Do the engine-architecture work first (island
scheduling, devirtualization, SIMD, patch-aware bypass): worth ~an order of
magnitude, more than any accelerator. ANE: no for the signal path, yes for
offline calibration. RTX 5090: no in the loop, yes as the training/dataset
factory, conditional yes for a block-latency ensemble mode.

## Nonlinearity inventory (what's audible, even indirectly)
- Slope cores ×9 (slews, SSG, env): TL072-class integrator driven by two BJT
  differential pairs (tanh current limits → the Serge ramp curvature) + LM3900
  Norton end comparator. Audio-rate cycling makes curvature = timbre.
- Triple Waveshaper ×3: LM3900 Norton amps + diode nets (DAFx-18 white-box
  precedent: Gormond/Esqueda/Pöntynen/Parker).
- 1973 VCF ×2: SVF integrator saturation, resonance compression, input drive.
- Dual Gates ×4: expo VCA law + 4-quadrant core (2023 schematic unpublished —
  measure/trace).
- VCO ×2: expo-converter error, saw-reset truncation, diode sine shaper, sync.
- CV-Pro soft knee ≳16 Vpp; P&T precision-rectifier crossover.
- Comparator edges (~12 red outputs): hardware slew-limited; v1's naive edges
  alias at audio rate — cheapest audible fix in the plan.

## Tier ladder & computed budgets (% of one M2 Max P-core @48 kHz)
Anchors: measured v1 = 55.5× realtime (1,310 cyc/sample vs 72.7k budget);
analytic per-section costs ±40% (all editable in the xlsx).

| Tier | cyc/sample | M2/Max | M4 | M5est | Typical patch (40% coverage, M2) |
|---|---|---|---|---|---|
| T0 v1 behavioral (measured) | 1,310 | 1.8% | 1.1% | 1.0% | 1.8% |
| T1 static NL + 2×OS | 3,658 | 5.0% | 3.2% | 2.8% | 3.1% |
| T2 ADAA static NL | 2,920 | 4.0% | 2.5% | 2.3% | 2.7% |
| T3 semi-physical, 2×OS | 22,310 | 30.7% | 19.5% | 17.2% | 13.4% |
| T5h T3 + NOVA hero blocks | 29,310 | 40.3% | 25.6% | 23.1% | 17.2% |
| T5n all-neural (10 blocks) | 36,910 | 50.8% | 32.2% | 30.8% | 21.4% |
| T4 full DK/WDF, 2×OS | 67,110 | 92.3% | 58.6% | 51.9% | 38.0% |
| T4 full DK/WDF, 4×OS | 132,910 | 182.8% | 116% | 102.7% | 74.2% |

Verdict bands: ≤15% comfortable · 30–70% Diva-class CPU-heavy · >100% needs
island threading (fits the M2 Max's 8 P-cores). fp64 in the VCF loop would
double that block on NEON — measure before committing.

Per-block assignments: slopes/SSG → T3→T4 (small circuits, identity-defining);
TWS → T4 white-box + T5 NOVA hero A/B; VCF → T3/T4 white-box only (feedback
patching + self-osc make neural filters fragile); gates → T3 from measurements;
VCO → T3; CV-Pro/P&T → T1/T2; comparator edges → BLEP/slew now.

## Architecture before accelerators
1. Island scheduling: SCC partition of the cable graph; block-process
   feedback-free islands (32–64 samples), single-sample cadence only inside
   real cycles; enables multi-core distribution. ~3–8× win.
2. Devirtualize the per-sample tick (~⅓ of today's whole budget is dispatch).
3. NEON-vectorize within blocks; parallel sections as SIMD lanes.
4. Oversample only nonlinear cores (shared halfbands at island edges).

## Apple Neural Engine — verdict
NO for the signal path: ANE reachable only via Core ML; no realtime/latency
guarantees, execution not pinnable; Apple's own realtime-audio-ML answer is
BNNS Graph *on the CPU* (WWDC24 session, updated WWDC25). A patchable graph
with 1-sample feedback (20.8 µs serial budget) can't host dispatch measured in
100s of µs. YES for offline/async: parameter fitting, model distillation,
patch-match features; block-latency sidecars → BNNS Graph on an E-core.

## RTX 5090 / Linux GPU — verdict
NO in the loop: launch ~3–10 µs + PCIe hops vs 20.8 µs/sample; persistent
kernels die on jitter; a single GPU thread is far slower than one P-core and
per-sample parallelism (~25 modules) strands the chip. Evidence: GPU Audio SDK
(Mar 2025) schedules 100–200 µs execution windows, DAG topology; headline demo
is 100+ parallel NAM instances — throughput, not latency. YES as the factory:
NOVA/NAM-A2 training over dense control spaces on the dual-5090 tower (+
TinyBox), differentiable fitting of T3/T4 component values, SPICE dataset
generation. CONDITIONAL: v3 "ensemble mode" — 32–128 full systems at
128-sample block latency (poly / generative wall-of-Serge), Linux VST3.

## Roadmap
- P0 (weeks): comparator-edge BLEP/slew; banana-stack current-fight model;
  per-instance tolerance seeds + thermal drift; island scheduler + devirt.
- P1 (1–2 mo): SPICE decks (slope, TWS, SVF); T1/T2 curves behind per-module
  Fast/Analog switch; gauntlet extended with null-tests vs SPICE/hardware.
- P2 (2–4 mo): T3 slopes/VCF/gates/VCO; T4 TWS + slopes; NOVA TWS hero
  trained on the 5090s; patch-aware bypass; island threading.
- P3: TKB panel w/ MPE; CLAP build; preset morphing + constraint randomizer;
  scope module; silver-enamel skin; signing/notarization + pluginval CI;
  Faust export of T3 cores for Daisy patch.Init().

## Watchlist
±40% cost estimates (de-risk: build ONE T4 slope core, measure, rescale);
Dual Gates schematic unpublished; NAM A2/Slimmable may halve neural costs;
possible future lower-level ANE access; fp64 pressure in VCF at high Q.
