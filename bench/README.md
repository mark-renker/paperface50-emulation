# Measurement bench

Objective checks for every DSP change: null tests against a reference tree, aliasing,
THD/THD+N, filter response, DC, idle noise, latency (reported vs measured), parameter
stepping, denormals and CPU. One command, a markdown report with plots, and a non-zero
exit code on regression.

```bash
python3 -m pip install numpy matplotlib          # matplotlib only for plots
python3 bench/run_bench.py                        # vs merge-base with origin/main
python3 bench/run_bench.py --against v1.0.1       # vs any git ref
python3 bench/run_bench.py --quick --no-cpu       # ~10 s smoke run
# with the plugin-level bench (real PF50Processor; needs the JUCE build):
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target pf50_pluginbench pf50_pluginbench_fx
python3 bench/run_bench.py --plugin-bench build
```

Output goes to `bench/out/` (`report.md`, `metrics.json`, `*.png`, raw captures). Exit code:
0 = clean, 1 = regression or new spec failure, 2 = harness error.

## How it works

| piece | what it does |
|---|---|
| `render/Bench.cpp` | JUCE-free renderer. Builds the engine graph per case, injects deterministic stimuli through the I/O strip (sines in double precision, impulses, MIDI-equivalent gate/pitch), reads jack values directly, writes float32 captures + `manifest.json`. `cpu` mode times 128-sample blocks with FTZ/DAZ on and off and traces denormals. |
| `run_bench.py` | Compiles `Bench.cpp` against the working tree's `src/engine` **and** against `src/engine` extracted from the reference ref (`git archive`, no checkout), renders both, nulls them, analyses, applies gates, writes the report. Also builds and runs `tests/GauntletTests.cpp`. |
| `analysis.py` | numpy-only measurement math (Kaiser-20 windowed 1 s FFTs, harmonic masking, decimating FIR, SVF metrics). |
| `../tools/PluginBench.cpp` | JUCE console app (instrument and FX builds) driving the real `PF50Processor`: factory patches via `loadFactoryPatch`, block-size invariance (1/32/128/441/512/2048), `getLatencySamples()` vs measured FX-path latency, release tail. Its factory renders are nulled against the engine-level mirror in `Bench.cpp`, so the mirror cannot drift silently. |
| `thresholds.json` | Spec budgets, regression tolerances, and `known_failures` (spec gates failing when recorded, each with its root cause). |
| `baseline/metrics.json` | Deterministic metrics of the commit that recorded them; used only with `--no-against`. |

## What is measured

* **Null test:** every capture (114 cases, about 230 probes) vs the reference tree. Bit-exact, or peak residual ≤ -120 dB re nominal, else FAIL. Label a PR `dsp-change` (or pass `--accept-null`) when the sound is meant to change; metric regressions still fail.
* **Aliasing:** worst non-harmonic spur (dBc) and alias-to-harmonic power ratio in 20 Hz–20 kHz for the VCO saw, pulse, sine and shaped sine at 0.5–10 kHz, the triple waveshaper at four drives and four frequencies, and the master output stage. Each is rendered at 48 kHz and also with the engine at 96/192 kHz then decimated, which shows what 2x/4x oversampling would buy.
* **Nonlinearity:** THD and THD+N vs level for the master output tanh, the 1973 VCF input stage (reconstructed exactly as HP + k·BP + LP) and its LOW out, and DUAL GATES in DC mode; ring-mod carrier and modulator suppression.
* **1973 VCF:** small-signal impulse responses; natural frequency (band-pass peak) vs knob law in cents, resonance peak vs the expected 1/k, LP/HP slopes, LP -3 dB point.
* **DC and idle noise:** DC on the bipolar outputs, and silence through every audio path (must stay exact zero or below -140 dB).
* **Latency:** impulse through FX routes, measured vs reported (engine level and real plugin).
* **Parameter stepping:** a knob updated once per host block (as `processBlock` does) vs per sample; residual level and its 2–20 kHz part.
* **Denormals:** with FTZ/DAZ off, the IEEE underflow flag is checked around every module's `tick()`, plus a scan of jacks, LEDs and filter state for subnormals.
* **CPU:** six scenarios (idle, the four factory patches, an all-modules stress patch), FTZ on/off, 5 interleaved repetitions. Headline is the best repetition (min-time estimator), because shared machines are bimodally noisy (about 2x between reps here). Against a reference, both trees are timed interleaved on the same machine; a slowdown over 10% warns (`--strict-cpu` makes it fail).

## Conventions

0 dB = ±2.5 V peak (the engine's nominal audio level) for module jacks; dBFS for the master
output, which is mixed exactly as `PluginProcessor::processBlock` does (I/O strip OUT plus
the BAL OUT taps). Compiler flags mirror the JUCE Release build (`-O3`, no fast-math).

## Not covered yet

* macOS/arm64 numbers: CI runs on Linux x86-64. Null results are platform-specific (libm),
  so compare a Mac run against a Mac reference, which `--against` does automatically.
* Real host behaviour (automation timing, offline-render tail handling): the plugin bench
  calls `processBlock` directly and does not load the VST3/AU binaries in a host.
* Modulation-domain metrics (FM/sync aliasing, envelope timing jitter) and `auval`.
