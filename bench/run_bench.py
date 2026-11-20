#!/usr/bin/env python3
"""Paperface 50 measurement bench — one command, objective numbers, non-zero exit on regression.

    python3 bench/run_bench.py                       # null + metrics vs merge-base with origin/main
    python3 bench/run_bench.py --against v1.0.1      # compare against any git ref
    python3 bench/run_bench.py --no-against          # metrics vs committed bench/baseline/metrics.json
    python3 bench/run_bench.py --quick               # reduced case set (~seconds)
    python3 bench/run_bench.py --plugin-bench build  # also run tools/PluginBench (JUCE) from that build dir
    python3 bench/run_bench.py --update-baseline     # rewrite bench/baseline/metrics.json from this tree

Steps: compile bench/render/Bench.cpp against the working tree's engine (and the reference
tree's engine), render deterministic stimuli, time CPU, run the existing gauntlet suite,
analyse, check gates in bench/thresholds.json, write <out>/report.md + plots + metrics.json.

Exit code: 0 = no regression and no new spec failure; 1 = regression or new spec failure;
2 = harness error (build/render failed).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tarfile
import io
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import analysis as A  # noqa: E402

CXX_FLAGS = ["-std=c++17", "-O3", "-DNDEBUG", "-Wall", "-Wextra"]  # mirrors JUCE Release (-O3, no fast-math)


# ------------------------------------------------------------------ helpers --
def log(msg: str) -> None:
    print(f"[bench] {msg}", flush=True)


def run(cmd, **kw):
    log("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def git(*args, check=True) -> str:
    r = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip() if r.returncode == 0 else ""


def compiler() -> str:
    return os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or "g++"


def build_bench(engine_dir: Path, out_bin: Path) -> None:
    out_bin.parent.mkdir(parents=True, exist_ok=True)
    run([compiler(), *CXX_FLAGS, f"-I{engine_dir}", str(HERE / "render" / "Bench.cpp"), "-o", str(out_bin)])


def export_ref_engine(ref: str, dest: Path) -> Path:
    """Extract src/engine at `ref` into dest (no worktree, no checkout)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    data = subprocess.run(["git", "-C", str(REPO), "archive", "--format=tar", ref, "src/engine"],
                          check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        try:
            tf.extractall(dest, filter="data")
        except TypeError:  # Python < 3.12 without the extraction-filter backport
            tf.extractall(dest)
    return dest / "src" / "engine"


def resolve_ref(arg: str | None) -> str | None:
    if arg in (None, "none"):
        return None
    if arg != "auto":
        return git("rev-parse", "--verify", arg + "^{commit}")
    for base in ("origin/main", "main", "origin/master", "master"):
        if git("rev-parse", "--verify", "-q", base, check=False):
            mb = git("merge-base", "HEAD", base, check=False)
            if mb:
                return mb
    return None


def load_manifest(d: Path):
    cases = json.loads((d / "manifest.json").read_text())
    for c in cases:
        c["data"] = {p["label"]: np.fromfile(d / p["file"], dtype="<f4") for p in c["probes"]}
    return {c["name"]: c for c in cases}


def fmt(v, nd=1, unit=""):
    if v is None or (isinstance(v, float) and (math.isnan(v))):
        return "n/a"
    if isinstance(v, (int, float)) and v <= -150:
        return f"< -150{unit}"
    return f"{v:.{nd}f}{unit}"


# ----------------------------------------------------------------- analysis --
def analyse_renders(cases: dict) -> dict:
    m: dict = {"vco": [], "tws": [], "master_thd": [], "master_alias": [], "vcf_ir": [], "vcf_thd": [],
               "gates_thd": [], "ringmod": None, "dc": [], "noise": [], "latency": [], "zipper": [],
               "factory": [], "finite": True, "nonfinite_cases": []}
    for name, c in cases.items():
        for lbl, x in c["data"].items():
            if not np.all(np.isfinite(x)):
                m["finite"] = False
                m["nonfinite_cases"].append(f"{name}:{lbl}")

    def ha(x, sr, f0, ref, est=True):
        x48 = A.decimate_to_48k(x, sr)[:48000] if sr > 48000 else x[:48000]
        r = A.harmonic_analysis(x48, 48000.0, f0, ref_amp=ref, estimate_f0=est)
        r.pop("_spectrum")
        return r

    for name, c in sorted(cases.items()):
        g, sr, meta = c["group"], c["sr"], c["meta"]
        os_factor = int(round(sr / 48000))
        if g == "vco_alias":
            for wave in ("saw", "sine", "shape1", "pulse"):
                r = ha(c["data"][wave], sr, meta["f0"], meta["ref_amp"])
                m["vco"].append({"case": name, "wave": wave, "os": os_factor, **r, "f0": meta["f0"], "f0_measured": r["f0"]})
        elif g == "tws_alias":
            r = ha(c["data"]["out"], sr, meta["f0"], meta["ref_amp"], est=False)
            m["tws"].append({"case": name, "drive": meta["drive"], "os": os_factor, **r, "f0": meta["f0"]})
        elif g == "master_thd":
            r = ha(c["data"]["out"], sr, meta["f0"], meta["ref_amp"], est=False)
            gain = 10 ** (r["fund_db"] / 20) / (meta["in_amp"] / 2.5)
            m["master_thd"].append({"case": name, "level_db": meta["level_db"], "gain_db": A.db_amp(gain), **r})
        elif g == "master_alias":
            for p in ("raw", "master"):
                r = ha(c["data"][p], sr, meta["f0"], 1.0)
                m["master_alias"].append({"case": name, "path": p, "os": os_factor, **r, "f0": meta["f0"]})
        elif g == "vcf_ir":
            d = c["data"]
            r = A.vcf_metrics(d["lp"], d["bp"], d["hp"], sr, meta["f_nominal"], meta["res"], meta["imp"], meta["gain_ss"])
            r["case"] = name
            m["vcf_ir"].append(r)
        elif g == "vcf_thd":
            for p in ("lp", "x"):
                r = ha(c["data"][p], sr, meta["f0"], meta["ref_amp"], est=False)
                m["vcf_thd"].append({"case": name, "probe": p, "level_db": meta["level_db"], **r, "f0": meta["f0"]})
        elif g == "gates_thd":
            r = ha(c["data"]["out"], sr, meta["f0"], meta["ref_amp"], est=False)
            m["gates_thd"].append({"case": name, "level_db": meta["level_db"], **r})
        elif g == "gates_ringmod":
            x = c["data"]["out"]
            fc, fm = meta["fc"], meta["fm"]
            car = A.tone_amp(x, sr, fc)
            sb = max(A.tone_amp(x, sr, fc - fm), A.tone_amp(x, sr, fc + fm))
            mft = A.tone_amp(x, sr, fm)
            m["ringmod"] = {"carrier_vs_sideband_db": A.db_amp(car / sb), "mod_feedthrough_vs_sideband_db": A.db_amp(mft / sb)}
        elif g == "dc":
            for p, x in c["data"].items():
                m["dc"].append({"case": name, "probe": p, "dc_mv": float(np.mean(x)) * 1000.0,
                                "dc_db": A.db_amp(np.mean(x) / meta["ref_amp"])})
        elif g == "noise":
            for p, x in c["data"].items():
                ref = 1.0 if p.startswith("master") else meta["ref_amp"]
                m["noise"].append({"probe": p, "peak_db": A.db_amp(np.max(np.abs(x)) / ref),
                                   "rms_db": A.db_amp(math.sqrt(float(np.mean(x.astype(np.float64) ** 2))) / ref),
                                   "exact_zero": bool(np.all(x == 0))})
        elif g == "latency":
            x = c["data"]["out"]
            k = int(np.argmax(np.abs(x)))
            m["latency"].append({"route": name, "measured": k - int(meta["t0"]), "reported": int(meta["reported_latency"]),
                                 "peak_gain_db": A.db_amp(x[k] / (0.01 / 2.5))})
        elif g == "factory":
            L, R = c["data"]["L"].astype(np.float64), c["data"]["R"].astype(np.float64)
            m["factory"].append({"patch": name, "peak_dbfs": A.db_amp(max(np.max(np.abs(L)), np.max(np.abs(R)))),
                                 "rms_dbfs": A.db_amp(math.sqrt(np.mean(L ** 2))),
                                 "dc_dbfs": A.db_amp(np.mean(L)), "silent": bool(np.all(L == 0) and np.all(R == 0)),
                                 "over_0dbfs": int(np.sum(np.abs(L) > 1.0) + np.sum(np.abs(R) > 1.0))})
    # zipper: residual of block-stepped knob vs per-sample knob
    for mod in ("gates", "vcf"):
        ref = cases.get(f"zip_{mod}_b1")
        if not ref:
            continue
        for b in (64, 512):
            c = cases.get(f"zip_{mod}_b{b}")
            if not c:
                continue
            d = c["data"]["out"].astype(np.float64) - ref["data"]["out"].astype(np.float64)
            sp = A.Spectrum(d, 48000.0)
            hf = float(sp.p[(sp.freqs > 2000) & (sp.freqs < 20000)].sum())
            m["zipper"].append({"module": mod, "block": b,
                                "residual_rms_db": A.db_amp(math.sqrt(np.mean(d ** 2)) / 2.5),
                                "residual_peak_db": A.db_amp(np.max(np.abs(d)) / 2.5),
                                "residual_hf_db": A.db(hf / (2.5 ** 2 / 2))})
    return m


def null_test(head: dict, ref: dict, thr_db: float):
    rows = []
    for name in sorted(set(head) | set(ref)):
        if name not in head or name not in ref:
            rows.append({"case": name, "status": "missing-in-" + ("head" if name not in head else "ref")})
            continue
        h, r = head[name], ref[name]
        worst_peak, worst_rms, exact = -400.0, -400.0, True
        for lbl, x in h["data"].items():
            y = r["data"].get(lbl)
            if y is None or len(y) != len(x):
                exact = False; worst_peak = worst_rms = 0.0
                continue
            if not np.array_equal(x, y):
                exact = False
                d = x.astype(np.float64) - y.astype(np.float64)
                ra = h["meta"].get("ref_amp", 2.5)
                worst_peak = max(worst_peak, A.db_amp(np.max(np.abs(d)) / ra))
                worst_rms = max(worst_rms, A.db_amp(math.sqrt(np.mean(d ** 2)) / ra))
        status = "bit-exact" if exact else ("within" if worst_peak <= thr_db else "MISMATCH")
        rows.append({"case": name, "status": status, "peak_residual_db": None if exact else worst_peak,
                     "rms_residual_db": None if exact else worst_rms})
    return rows


def analyse_cpu(paths) -> dict:
    """CPU stats from one or more cpu.json files (all reps pooled).

    Shared/virtualised machines show large, bimodal run-to-run noise (frequency scaling,
    noisy neighbours), so the headline is the *best* rep: real-time factor from the median
    block time of the least-disturbed run, which is the standard min-time estimator."""
    paths = [paths] if isinstance(paths, Path) else list(paths)
    js = [json.loads(p.read_text()) for p in paths]
    j = js[0]
    budget_ns = j["block"] / j["sr"] * 1e9
    out = {"block": j["block"], "sr": j["sr"], "ftz_supported": j["ftz_supported"], "scenarios": {},
           "denormal_scan": {d["scenario"]: d for d in j.get("denormal_scan", [])}}
    groups: dict = {}
    for jj in js:
        for r in jj["runs"]:
            groups.setdefault((r["scenario"], r["ftz"]), []).append(np.asarray(r["block_ns"], dtype=np.float64))
    for (sc, ftz), runs in groups.items():
        rtfs = [budget_ns / np.median(b) for b in runs]
        best = runs[int(np.argmax(rtfs))]
        n_active = int(0.5 * j["sr"] / j["block"])
        tail = best[int(3.0 * j["sr"] / j["block"]):]
        out["scenarios"][f"{sc}|{'ftz' if ftz else 'noftz'}"] = {
            "scenario": sc, "ftz": ftz, "reps": len(runs),
            "rtf": float(max(rtfs)), "rtf_worst_rep": float(min(rtfs)),
            "p50_pct": float(np.percentile(best, 50) / budget_ns * 100),
            "p99_pct": float(np.percentile(best, 99) / budget_ns * 100),
            "max_pct": float(best.max() / budget_ns * 100),
            "tail_over_active": float(np.median(tail) / np.median(best[:n_active])) if len(tail) else None,
            "_block_ns": best.tolist(),
        }
    return out


# -------------------------------------------------------------------- gates --
class Gates:
    def __init__(self, thresholds: dict):
        self.t = thresholds
        self.rows = []  # (kind, id, status, detail)

    def spec(self, gid: str, ok: bool, detail: str):
        known = gid in self.t.get("known_failures", {})
        status = ("XPASS" if known else "PASS") if ok else ("XFAIL" if known else "FAIL")
        self.rows.append(("spec", gid, status, detail))

    def regress(self, gid: str, ok: bool, detail: str, warn_only=False):
        status = "PASS" if ok else ("WARN" if warn_only else "FAIL")
        self.rows.append(("regression", gid, status, detail))

    def failed(self):
        return [r for r in self.rows if r[2] == "FAIL"]


def check_spec(g: Gates, m: dict, cpu: dict | None, plugin: dict | None):
    t = g.t["spec"]
    g.spec("render.finite", m["finite"], "all captures finite" if m["finite"] else ", ".join(m["nonfinite_cases"][:5]))
    for n in m["noise"]:
        g.spec(f"noise.{n['probe']}", n["exact_zero"] or n["peak_db"] <= t["idle_noise_max_db"],
               f"silence in -> peak {fmt(n['peak_db'])} dB")
    for row in m["vco"]:
        if row["os"] != 1 or row["wave"] != "saw":
            continue
        lim = t["vco_saw_worst_spur_dbc"]
        ok = row["f0"] > lim["max_f0"] or row["worst_spur_dbc"] <= lim["limit"]
        g.spec(f"alias.vco_saw.{int(row['f0'])}", ok, f"worst alias spur {fmt(row['worst_spur_dbc'])} dBc (limit {lim['limit']} for f0 <= {lim['max_f0']} Hz)")
    for row in m["tws"]:
        if row["os"] != 1:
            continue
        lim = t["tws_alias_ratio_db"]
        ok = row["f0"] > lim["max_f0"] or row["alias_ratio_db"] <= lim["limit"]
        g.spec(f"alias.tws.{int(row['f0'])}.d{int(row['drive']*100)}", ok,
               f"alias/harmonic power {fmt(row['alias_ratio_db'])} dB (limit {lim['limit']})")
    for row in m["vcf_ir"]:
        g.spec(f"vcf.cutoff.{row['case']}", abs(row["cutoff_err_cents"]) <= t["vcf_cutoff_cents"],
               f"BP peak {row['f_peak']:.2f} Hz vs nominal {row['f_nominal']:.2f} Hz ({row['cutoff_err_cents']:+.2f} cents)")
        if row["lp_slope_db_oct"] is not None:
            g.spec(f"vcf.lp_slope.{row['case']}", abs(row["lp_slope_db_oct"] + 12) <= t["vcf_slope_tol_db"],
                   f"{row['lp_slope_db_oct']:+.2f} dB/oct (4f0->8f0)")
    for d in m["dc"]:
        g.spec(f"dc.{d['case']}.{d['probe']}", d["dc_db"] <= t["dc_max_db"], f"{d['dc_mv']:+.4f} mV ({fmt(d['dc_db'])} dB)")
    for f in m["factory"]:
        g.spec(f"factory.{f['patch']}.audible", not f["silent"] and f["peak_dbfs"] > t["factory_min_peak_dbfs"],
               f"peak {fmt(f['peak_dbfs'])} dBFS")
        g.spec(f"factory.{f['patch']}.dc", f["dc_dbfs"] <= t["dc_max_db"], f"DC {fmt(f['dc_dbfs'])} dBFS")
    for lat in m["latency"]:
        g.spec(f"latency.{lat['route']}", lat["measured"] == lat["reported"],
               f"measured {lat['measured']} smp, reported {lat['reported']}")
    if cpu:
        for sc, d in cpu["denormal_scan"].items():
            mods = ", ".join(f"{k} ({v} blocks)" for k, v in d["per_module"].items())
            g.spec(f"denormal.{sc}", d["underflow_blocks"] == 0 and d["subnormal_state_blocks"] == 0,
                   f"FTZ off: {d['underflow_blocks']} blocks raised underflow" + (f" in {mods}" if mods else "")
                   + f"; {d['subnormal_state_blocks']} blocks left subnormal state")
        s = cpu["scenarios"].get("stress_all_modules|ftz")
        if s:
            g.spec("cpu.rtf_stress", s["rtf"] >= t["cpu_min_rtf"], f"{s['rtf']:.1f}x realtime (min {t['cpu_min_rtf']}x)")
            g.spec("cpu.p99_block", s["p99_pct"] <= t["cpu_p99_block_pct"], f"p99 block {s['p99_pct']:.2f}% of budget")
    if plugin:
        for var, pj in plugin.items():
            if pj is None:
                continue
            for p in pj["patches"]:
                g.spec(f"plugin.{var}.patch{p['patch']}.block_invariance", p["block_invariance_max_abs"] == 0.0,
                       f"max |diff| across block sizes {p['block_invariance_max_abs']:.3g}")
            for r in pj.get("fx_routes", []):
                g.spec(f"plugin.fx.latency.{r['route']}", r["measured_latency"] == pj["reported_latency"],
                       f"measured {r['measured_latency']} smp, getLatencySamples() = {pj['reported_latency']}")


def metric_index(m: dict) -> dict:
    """Flat {id: value} of deterministic metrics, for regression comparison."""
    out = {}
    for r in m["vco"]:
        out[f"vco.{r['wave']}.{int(r['f0'])}.os{r['os']}.alias_ratio_db"] = r["alias_ratio_db"]
        out[f"vco.{r['wave']}.{int(r['f0'])}.os{r['os']}.worst_spur_dbc"] = r["worst_spur_dbc"]
    for r in m["tws"]:
        out[f"tws.{int(r['f0'])}.d{int(r['drive']*100)}.os{r['os']}.alias_ratio_db"] = r["alias_ratio_db"]
    for r in m["master_thd"]:
        out[f"master.thd.{r['level_db']:+g}"] = r["thd_db"]
    for r in m["master_alias"]:
        out[f"master_alias.{r['path']}.os{r['os']}.alias_ratio_db"] = r["alias_ratio_db"]
    for r in m["vcf_thd"]:
        out[f"vcf.thd.{r['probe']}.{int(r['f0'])}.{r['level_db']:+g}"] = r["thd_db"]
    for r in m["vcf_ir"]:
        out[f"vcf.cutoff_err_cents.{r['case']}"] = r["cutoff_err_cents"]
    for d in m["dc"]:
        out[f"dc.{d['case']}.{d['probe']}"] = d["dc_db"]
    for z in m["zipper"]:
        out[f"zipper.{z['module']}.b{z['block']}.residual_rms_db"] = z["residual_rms_db"]
    return {k: (None if v is None else float(v)) for k, v in out.items()}


def check_regressions(g: Gates, head_idx: dict, ref_idx: dict, label: str, tol_db: float):
    worse = []
    for k, hv in head_idx.items():
        rv = ref_idx.get(k)
        if hv is None or rv is None:
            continue
        if any(s in k for s in ("cutoff_err_cents",)):
            if abs(hv) > abs(rv) + 0.5:
                worse.append((k, rv, hv))
        elif hv > rv + tol_db and hv > -150:
            worse.append((k, rv, hv))
    g.regress(f"metrics.vs.{label}", not worse,
              "no metric worse than tolerance" if not worse else
              "; ".join(f"{k}: {a:.2f} -> {b:.2f}" for k, a, b in worse[:8]) + (f" (+{len(worse)-8} more)" if len(worse) > 8 else ""))


# -------------------------------------------------------------------- plots --
def make_plots(m: dict, cpu: dict | None, cases: dict, outdir: Path) -> list[str]:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        log("matplotlib not installed: skipping plots")
        return []
    files = []

    def save(fig, name):
        fig.tight_layout(); fig.savefig(outdir / name, dpi=110); plt.close(fig); files.append(name)

    # aliasing vs f0, per waveform and oversampling
    fig, axs = plt.subplots(1, 2, figsize=(11, 4))
    for wave, ls in (("saw", "-"), ("pulse", "--"), ("shape1", ":")):
        for os_, c in ((1, "C0"), (2, "C1"), (4, "C2")):
            rows = sorted([r for r in m["vco"] if r["wave"] == wave and r["os"] == os_], key=lambda r: r["f0"])
            if rows:
                axs[0].semilogx([r["f0"] for r in rows], [r["worst_spur_dbc"] for r in rows], ls, color=c, marker="o", ms=3,
                                label=f"{wave} {os_}x")
    axs[0].set(title="VCO worst alias spur (20 Hz-20 kHz)", xlabel="f0 (Hz)", ylabel="dBc"); axs[0].grid(alpha=.3); axs[0].legend(fontsize=7, ncol=3)
    for os_, c in ((1, "C0"), (2, "C1"), (4, "C2")):
        for d, ls in ((25, ":"), (75, "-"), (100, "--")):
            rows = sorted([r for r in m["tws"] if r["os"] == os_ and int(r["drive"] * 100) == d], key=lambda r: r["f0"])
            if rows:
                axs[1].semilogx([r["f0"] for r in rows], [r["alias_ratio_db"] for r in rows], ls, color=c, marker="o", ms=3, label=f"drive {d}% {os_}x")
    axs[1].set(title="Triple waveshaper alias/harmonic power", xlabel="f0 (Hz)", ylabel="dB"); axs[1].grid(alpha=.3); axs[1].legend(fontsize=7, ncol=3)
    from matplotlib.ticker import NullFormatter
    for ax, rows in ((axs[0], m["vco"]), (axs[1], m["tws"])):
        fs = sorted({r["f0"] for r in rows})
        if fs:
            ax.set_xticks(fs); ax.set_xticklabels([f"{f:g}" for f in fs]); ax.xaxis.set_minor_formatter(NullFormatter())
    save(fig, "aliasing.png")

    # VCF responses
    if m["vcf_ir"]:
        fig, axs = plt.subplots(1, 3, figsize=(13, 4), sharey=True)
        for r in m["vcf_ir"]:
            f, ml, mb, mh = r["_curves"]
            for ax, mm in zip(axs, (ml, mb, mh)):
                ax.semilogx(f[1:], mm[1:], lw=.8, label=r["case"])
        for ax, t in zip(axs, ("LP", "BP", "HP")):
            ax.set(title=f"1973 VCF {t} (small-signal)", xlabel="Hz", xlim=(10, 24000), ylim=(-80, 35)); ax.grid(alpha=.3)
        axs[0].set_ylabel("dB"); axs[2].legend(fontsize=6)
        save(fig, "vcf_response.png")

    # THD vs level
    fig, ax = plt.subplots(figsize=(6.5, 4))
    for label, rows in (("master out (tanh) 1 kHz", m["master_thd"]),
                        ("VCF input stage 1 kHz", [r for r in m["vcf_thd"] if r["probe"] == "x" and r["f0"] > 500]),
                        ("VCF input stage 100 Hz", [r for r in m["vcf_thd"] if r["probe"] == "x" and r["f0"] < 500]),
                        ("Dual gates DC mode", m["gates_thd"])):
        rows = sorted(rows, key=lambda r: r["level_db"])
        if rows:
            ax.plot([r["level_db"] for r in rows], [max(r["thd_db"], -160) for r in rows], marker="o", label=label)
    ax.set(title="THD vs input level (0 dB = +-2.5 V nominal)", xlabel="level (dB)", ylabel="THD (dB)"); ax.grid(alpha=.3); ax.legend(fontsize=7)
    save(fig, "thd_vs_level.png")

    # spectra examples
    ex = [("vco_4973_48k", "saw"), ("tws_3001_d075_48k", "out"), ("master_saw_4973_48k", "master")]
    fig, axs = plt.subplots(len(ex), 1, figsize=(10, 7), sharex=True)
    for ax, (cn, p) in zip(axs, ex):
        if cn in cases:
            c = cases[cn]
            sp = A.Spectrum(c["data"][p][:48000], 48000.0)
            ax.plot(sp.freqs, 20 * np.log10(np.maximum(sp.amp / c["meta"].get("ref_amp", 2.5), 1e-12)), lw=.5)
            ax.set(title=f"{cn} [{p}]", ylabel="dB", ylim=(-160, 5)); ax.grid(alpha=.3)
    axs[-1].set_xlabel("Hz")
    save(fig, "spectra.png")

    if cpu:
        fig, ax = plt.subplots(figsize=(10, 4))
        for key, s in sorted(cpu["scenarios"].items()):
            b = np.asarray(s["_block_ns"]) / (cpu["block"] / cpu["sr"] * 1e9) * 100
            ax.plot(b, lw=.5, label=key)
        ax.set(title=f"per-block CPU (block {cpu['block']} @ {cpu['sr']:.0f} Hz), % of real-time budget", xlabel="block", ylabel="%", yscale="log")
        ax.grid(alpha=.3); ax.legend(fontsize=6, ncol=3)
        save(fig, "cpu_blocks.png")
    return files


# ------------------------------------------------------------------- report --
def write_report(path: Path, ctx: dict) -> None:
    m, cpu, g, plugin = ctx["m"], ctx["cpu"], ctx["gates"], ctx["plugin"]
    L = []
    L.append("# Paperface 50 measurement report\n")
    L.append(f"- tree: `{ctx['head_desc']}`  reference: `{ctx['ref_desc']}`")
    L.append(f"- compiler: `{ctx['compiler']}` {' '.join(CXX_FLAGS)}; platform: {ctx['platform']}")
    L.append(f"- generated {time.strftime('%Y-%m-%d %H:%M:%S')} in {ctx['elapsed']:.1f} s\n")
    fails = g.failed()
    L.append(f"**Result: {'FAIL' if fails else 'PASS'}** — "
             f"{sum(r[2]=='PASS' for r in g.rows)} pass, {len(fails)} fail, "
             f"{sum(r[2]=='XFAIL' for r in g.rows)} known failures, {sum(r[2]=='XPASS' for r in g.rows)} unexpectedly passing, "
             f"{sum(r[2]=='WARN' for r in g.rows)} warnings.\n")
    interesting = [r for r in g.rows if r[2] != "PASS"]
    if interesting:
        L.append("| kind | gate | status | detail |\n|---|---|---|---|")
        kf = g.t.get("known_failures", {})
        for k, gid, st, det in interesting:
            note = kf.get(gid, {}).get("note", "") if st in ("XFAIL", "XPASS") else ""
            L.append(f"| {k} | `{gid}` | **{st}** | {det}{(' — ' + note) if note else ''} |")
        L.append("")

    if ctx["null"] is not None:
        nr = ctx["null"]
        L.append("## Null test vs reference\n")
        L.append(f"{sum(r['status']=='bit-exact' for r in nr)}/{len(nr)} cases bit-exact; "
                 f"{sum(r['status']=='within' for r in nr)} within {ctx['null_thr']} dB; "
                 f"{sum(r['status']=='MISMATCH' for r in nr)} mismatched.\n")
        bad = [r for r in nr if r["status"] not in ("bit-exact",)]
        if bad:
            L.append("| case | status | peak residual (dB re ref) | rms residual |\n|---|---|---|---|")
            for r in bad[:40]:
                L.append(f"| {r['case']} | {r['status']} | {fmt(r.get('peak_residual_db'))} | {fmt(r.get('rms_residual_db'))} |")
            L.append("")

    L.append("## Aliasing\n")
    L.append("Worst non-harmonic spur in 20 Hz–20 kHz (dBc re strongest harmonic), output at 48 kHz. "
             "2x/4x = engine run at 96/192 kHz and decimated, i.e. what oversampling would buy.\n")
    L.append("| wave | f0 | 1x | 2x | 4x | alias/harm power 1x |\n|---|---|---|---|---|---|")
    for wave in ("saw", "pulse", "shape1", "sine"):
        for f0 in sorted({r["f0"] for r in m["vco"]}):
            rs = {r["os"]: r for r in m["vco"] if r["wave"] == wave and r["f0"] == f0}
            if rs:
                L.append(f"| VCO {wave} | {f0:g} | " + " | ".join(fmt(rs[o]["worst_spur_dbc"]) if o in rs else "–" for o in (1, 2, 4))
                         + f" | {fmt(rs[1]['alias_ratio_db']) if 1 in rs else '–'} |")
    L.append("\n| TWS drive | f0 | 1x | 2x | 4x | (alias/harmonic power, dB) |\n|---|---|---|---|---|---|")
    for d in sorted({r["drive"] for r in m["tws"]}):
        for f0 in sorted({r["f0"] for r in m["tws"]}):
            rs = {r["os"]: r for r in m["tws"] if r["drive"] == d and r["f0"] == f0}
            L.append(f"| {d:.2f} | {f0:g} | " + " | ".join(fmt(rs[o]["alias_ratio_db"]) if o in rs else "–" for o in (1, 2, 4)) + " | |")
    if m["master_alias"]:
        L.append("\n| saw 4973 Hz | 1x alias/harm | 2x | 4x |\n|---|---|---|---|")
        for p in ("raw", "master"):
            rs = {r["os"]: r for r in m["master_alias"] if r["path"] == p}
            L.append(f"| {p} | " + " | ".join(fmt(rs[o]["alias_ratio_db"]) if o in rs else "–" for o in (1, 2, 4)) + " |")
    L.append("")

    L.append("## Nonlinearity (THD at 997 Hz unless noted)\n")
    L.append("| stage | level (dB re nominal) | THD (dB) | THD+N (dB) | gain (dB) |\n|---|---|---|---|---|")
    for r in sorted(m["master_thd"], key=lambda r: r["level_db"]):
        L.append(f"| master out tanh (vol 0.8) | {r['level_db']:+g} | {fmt(r['thd_db'])} | {fmt(r['thdn_db'])} | {r['gain_db']:+.2f} |")
    for r in sorted(m["vcf_thd"], key=lambda r: (r["probe"], r["f0"], r["level_db"])):
        lbl = "VCF input stage (HP+kBP+LP)" if r["probe"] == "x" else "VCF LOW out, fc 8 kHz"
        L.append(f"| {lbl} {r['f0']:g} Hz | {r['level_db']:+g} | {fmt(r['thd_db'])} | {fmt(r['thdn_db'])} | |")
    for r in m["gates_thd"]:
        L.append(f"| Dual gates DC mode, gain 1 | {r['level_db']:+g} | {fmt(r['thd_db'])} | {fmt(r['thdn_db'])} | |")
    if m["ringmod"]:
        L.append(f"\nRing mod (MOD mode, 997 Hz x 151 Hz): carrier {m['ringmod']['carrier_vs_sideband_db']:.1f} dB and "
                 f"modulator feedthrough {m['ringmod']['mod_feedthrough_vs_sideband_db']:.1f} dB below the sidebands.\n")

    L.append("## 1973 VCF (small-signal impulse response)\n")
    L.append("| case | nominal f0 | BP peak | error (cents) | BP peak dB (expected) | LP slope dB/oct | HP slope dB/oct | LP -3 dB |\n|---|---|---|---|---|---|---|---|")
    for r in m["vcf_ir"]:
        L.append(f"| {r['case']} | {r['f_nominal']:.1f} | {r['f_peak']:.1f} | {r['cutoff_err_cents']:+.2f} | "
                 f"{r['bp_peak_db']:.2f} ({r['bp_peak_expected_db']:.2f}) | {fmt(r['lp_slope_db_oct'],2)} | {fmt(r['hp_slope_db_oct'],2)} | {fmt(r['lp_m3db_hz'])} |")
    L.append("")

    L.append("## DC, idle noise, latency, parameter stepping\n")
    L.append("| DC case | probe | DC (mV) | dB re nominal |\n|---|---|---|---|")
    for d in m["dc"]:
        L.append(f"| {d['case']} | {d['probe']} | {d['dc_mv']:+.4f} | {fmt(d['dc_db'])} |")
    L.append("\n| idle (silence in) | peak dB | exact zero |\n|---|---|---|")
    for n in m["noise"]:
        L.append(f"| {n['probe']} | {fmt(n['peak_db'])} | {n['exact_zero']} |")
    L.append("\n| latency route (engine) | measured | reported | peak gain dB |\n|---|---|---|---|")
    for lat in m["latency"]:
        L.append(f"| {lat['route']} | {lat['measured']} | {lat['reported']} | {lat['peak_gain_db']:+.2f} |")
    L.append("\n| knob stepped per block | block | residual rms (dB) | residual peak | residual 2–20 kHz |\n|---|---|---|---|---|")
    for z in m["zipper"]:
        L.append(f"| {z['module']} | {z['block']} | {fmt(z['residual_rms_db'])} | {fmt(z['residual_peak_db'])} | {fmt(z['residual_hf_db'])} |")
    L.append("\n| factory patch (engine mirror) | peak dBFS | rms dBFS | DC dBFS | silent | samples > 0 dBFS |\n|---|---|---|---|---|---|")
    for f in m["factory"]:
        L.append(f"| {f['patch']} | {fmt(f['peak_dbfs'])} | {fmt(f['rms_dbfs'])} | {fmt(f['dc_dbfs'])} | {f['silent']} | {f['over_0dbfs']} |")
    L.append("")

    if cpu:
        L.append(f"## CPU (block {cpu['block']} @ {cpu['sr']:.0f} Hz, single thread)\n")
        L.append("Best of the repetitions (shared machines are noisy; worst rep shown for scale). % = share of the block's real-time budget.\n")
        L.append("| scenario | FTZ/DAZ | x realtime (best) | worst rep | p50 % | p99 % | max % | tail/active |\n|---|---|---|---|---|---|---|---|")
        for key, s in sorted(cpu["scenarios"].items()):
            L.append(f"| {s['scenario']} | {'on' if s['ftz'] else 'off'} | {s['rtf']:.1f} | {s['rtf_worst_rep']:.1f} | {s['p50_pct']:.2f} | {s['p99_pct']:.2f} | {s['max_pct']:.2f} | "
                     f"{fmt(s['tail_over_active'],2)} |")
        L.append("\nDenormal trace (FTZ/DAZ off, untimed; IEEE underflow flag checked around every module tick):\n")
        L.append("| scenario | blocks raising underflow | modules | blocks ending with subnormal state |\n|---|---|---|---|")
        for sc, d in cpu["denormal_scan"].items():
            L.append(f"| {sc} | {d['underflow_blocks']} | {', '.join(f'{k}: {v}' for k, v in d['per_module'].items()) or '–'} | {d['subnormal_state_blocks']} |")
        L.append("")
    if ctx.get("cpu_ref"):
        L.append("CPU vs reference (same machine, interleaved): " + ", ".join(
            f"{k.split('|')[0]} {v:+.1f}%" for k, v in ctx["cpu_ref"].items()) + "\n")

    if plugin and any(plugin.values()):
        L.append("## Plugin-level (real PF50Processor)\n")
        for var, pj in plugin.items():
            if not pj:
                continue
            L.append(f"- **{var}**: getLatencySamples() = {pj['reported_latency']}, getTailLengthSeconds() = {pj['reported_tail_s']}")
            for p in pj["patches"]:
                fl = ctx.get("release_floor", {}).get((var, p["patch"]))
                tail = ("silent for the whole render" if p["last_sample_above_m120dBFS"] < 0 else
                        f"above -120 dBFS until {p['last_sample_above_m120dBFS']/pj['sr']:.2f} s (note-off at {p['note_off']/pj['sr']:.2f} s)")
                L.append(f"  - patch {p['patch']}: block-size invariance max |diff| {p['block_invariance_max_abs']:.3g}; {tail}"
                         + (f"; peak over the last second {fmt(fl)} dBFS" if fl is not None else ""))
            for r in pj.get("fx_routes", []):
                L.append(f"  - FX route `{r['route']}`: measured latency {r['measured_latency']} smp, peak {A.db_amp(r['peak']/0.25):+.2f} dB")
        if ctx.get("mirror_null"):
            L.append("\nEngine-mirror vs real plugin factory renders: " + ", ".join(
                f"patch {k}: {v}" for k, v in ctx["mirror_null"].items()) + "\n")

    L.append("## Gauntlet suite\n")
    L.append(f"`tests/GauntletTests.cpp`: {ctx['gauntlet']}\n")
    for p in ctx["plots"]:
        L.append(f"![{p}]({p})")
    L.append("\n## All gates\n\n| kind | gate | status | detail |\n|---|---|---|---|")
    for k, gid, st, det in g.rows:
        L.append(f"| {k} | `{gid}` | {st} | {det} |")
    path.write_text("\n".join(L) + "\n")


def strip_private(o):
    if isinstance(o, dict):
        return {k: strip_private(v) for k, v in o.items() if not k.startswith("_")}
    if isinstance(o, list):
        return [strip_private(v) for v in o]
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, (np.bool_,)):
        return bool(o)
    return o


# --------------------------------------------------------------------- main --
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(HERE / "out"))
    ap.add_argument("--against", default="auto", help="git ref for null/regression tests ('auto' = merge-base with origin/main)")
    ap.add_argument("--no-against", dest="against", action="store_const", const="none")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--no-cpu", action="store_true")
    ap.add_argument("--strict-cpu", action="store_true", help="CPU regression vs reference fails instead of warns")
    ap.add_argument("--accept-null", action="store_true", help="intentional DSP change: null mismatches warn instead of fail")
    ap.add_argument("--plugin-bench", default=None, help="CMake build dir containing pf50_pluginbench(_fx)")
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    t_start = time.time()
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    (out / "head").mkdir(parents=True)
    thresholds = json.loads((HERE / "thresholds.json").read_text())

    try:
        build_bench(REPO / "src" / "engine", out / "bin" / "pf50_bench_head")
        ref = resolve_ref(args.against)
        ref_bin = None
        if ref:
            ref_engine = export_ref_engine(ref, out / "ref_tree")
            ref_bin = out / "bin" / "pf50_bench_ref"
            build_bench(ref_engine, ref_bin)
            (out / "ref").mkdir()
        q = ["--quick"] if args.quick else []
        run([str(out / "bin" / "pf50_bench_head"), "render", str(out / "head"), *q])
        if ref_bin:
            run([str(ref_bin), "render", str(out / "ref"), *q])
        cpu_head = cpu_ref = None
        if not args.no_cpu:
            # interleave head/ref twice so machine drift hits both equally
            for rnd in range(2 if ref_bin else 1):
                d = out / f"cpu_head_{rnd}"; d.mkdir()
                run([str(out / "bin" / "pf50_bench_head"), "cpu", str(d), *q])
                if ref_bin:
                    d = out / f"cpu_ref_{rnd}"; d.mkdir()
                    run([str(ref_bin), "cpu", str(d), *q])
        # gauntlet suite (existing hard gate G2)
        gbin = out / "bin" / "pf50_gauntlet"
        run([compiler(), *CXX_FLAGS, str(REPO / "tests" / "GauntletTests.cpp"), "-o", str(gbin)])
        gr = subprocess.run([str(gbin)], capture_output=True, text=True)
        (out / "gauntlet.txt").write_text(gr.stdout)
        gauntlet_summary = f"exit {gr.returncode}; " + (gr.stdout.strip().splitlines()[-1] if gr.stdout.strip() else "")
        plugin = {}
        if args.plugin_bench:
            bdir = Path(args.plugin_bench).resolve()
            (out / "plugin").mkdir()
            for var, tgt in (("instrument", "pf50_pluginbench"), ("fx", "pf50_pluginbench_fx")):
                exe = next(iter(bdir.glob(f"{tgt}_artefacts/**/{tgt}")), None)
                if exe is None:
                    log(f"{tgt} not found under {bdir}; build it with: cmake --build {bdir} --target {tgt}")
                    plugin[var] = None
                    continue
                run([str(exe), str(out / "plugin")])
                plugin[var] = json.loads((out / "plugin" / f"plugin_{var}.json").read_text())
    except subprocess.CalledProcessError as e:
        log(f"harness error: {e}")
        return 2

    log("analysing")
    head_cases = load_manifest(out / "head")
    m = analyse_renders(head_cases)
    cpu = None
    if not args.no_cpu:
        cpu = analyse_cpu(sorted(out.glob("cpu_head_*/cpu.json")))
    g = Gates(thresholds)
    check_spec(g, m, cpu, plugin or None)
    if gr.returncode != 0:
        g.spec("gauntlet.suite", False, gauntlet_summary)
    else:
        g.spec("gauntlet.suite", True, gauntlet_summary)

    head_idx = metric_index(m)
    null_rows, ref_desc, cpu_ref_delta = None, "none", None
    if ref_bin:
        ref_cases = load_manifest(out / "ref")
        null_rows = null_test(head_cases, ref_cases, thresholds["regression"]["null_peak_db"])
        mism = [r for r in null_rows if r["status"] not in ("bit-exact", "within")]
        g.regress("null.vs_reference", not mism,
                  f"{len(null_rows) - len(mism)}/{len(null_rows)} cases null" + (f"; mismatched: {', '.join(r['case'] for r in mism[:6])}" if mism else ""),
                  warn_only=args.accept_null)
        ref_m = analyse_renders(ref_cases)
        check_regressions(g, head_idx, metric_index(ref_m), "reference", thresholds["regression"]["metric_tol_db"])
        ref_desc = git("log", "-1", "--format=%h %s", ref, check=False) or ref
        if cpu:
            cpu_ref_delta = {}
            rs = analyse_cpu(sorted(out.glob("cpu_ref_*/cpu.json")))
            for key, hsc in cpu["scenarios"].items():
                if key.endswith("|ftz") and key in rs["scenarios"]:
                    cpu_ref_delta[key] = (rs["scenarios"][key]["rtf"] / hsc["rtf"] - 1.0) * 100.0  # + = head slower
            worst = max(cpu_ref_delta.values())
            g.regress("cpu.vs_reference", worst <= thresholds["regression"]["cpu_slowdown_pct"],
                      f"worst scenario {worst:+.1f}% CPU vs reference (limit +{thresholds['regression']['cpu_slowdown_pct']}%)",
                      warn_only=not args.strict_cpu)
    else:
        base = HERE / "baseline" / "metrics.json"
        if base.exists() and not args.update_baseline and not args.quick:
            check_regressions(g, head_idx, json.loads(base.read_text())["index"], "committed-baseline",
                              thresholds["regression"]["metric_tol_db"])

    mirror_null = {}
    if plugin.get("instrument"):
        for p in range(1, 5):
            name = f"patch_{['', 'first_voice', 'serge_bongos', 'random_dance', 'sequenced_line'][p]}"
            if name not in head_cases:
                continue
            ok = True
            for ch in ("L", "R"):
                pl = np.fromfile(out / "plugin" / f"plugin_instrument_patch{p}__{ch}.f32", dtype="<f4")
                en = head_cases[name]["data"][ch]
                n = min(len(pl), len(en))
                ok &= bool(np.array_equal(pl[:n], en[:n]))
            mirror_null[p] = "bit-exact" if ok else "DIFFERS"
            g.spec(f"plugin.mirror.patch{p}", ok, "engine mirror of factory patch nulls against the real plugin" if ok else "mirror drifted from PluginProcessor::loadFactoryPatch")

    release_floor = {}
    for var, pj in plugin.items():
        if not pj:
            continue
        for p in pj["patches"]:
            x = np.fromfile(out / "plugin" / f"plugin_{var}_patch{p['patch']}__L.f32", dtype="<f4")
            release_floor[(var, p["patch"])] = A.db_amp(np.max(np.abs(x[-int(pj["sr"]):])))
    plots = make_plots(m, cpu, head_cases, out)
    ctx = {"m": m, "cpu": cpu, "gates": g, "plugin": plugin, "null": null_rows,
           "null_thr": thresholds["regression"]["null_peak_db"],
           "head_desc": (git("log", "-1", "--format=%h %s", check=False) or "working tree") + (" (+ uncommitted changes)" if git("status", "--porcelain", "src/engine", check=False) else ""),
           "ref_desc": ref_desc, "compiler": subprocess.run([compiler(), "--version"], capture_output=True, text=True).stdout.splitlines()[0],
           "platform": f"{os.uname().sysname} {os.uname().machine}", "elapsed": time.time() - t_start, "plots": plots,
           "gauntlet": gauntlet_summary, "cpu_ref": cpu_ref_delta, "mirror_null": mirror_null,
           "release_floor": release_floor}
    write_report(out / "report.md", ctx)
    metrics = {"index": head_idx, "detail": strip_private(m), "cpu": strip_private(cpu) if cpu else None,
               "plugin": plugin, "gates": [list(r) for r in g.rows]}
    (out / "metrics.json").write_text(json.dumps(metrics, indent=1, default=float))
    if args.update_baseline:
        (HERE / "baseline").mkdir(exist_ok=True)
        (HERE / "baseline" / "metrics.json").write_text(json.dumps({"index": head_idx}, indent=1, sort_keys=True) + "\n")
        log("baseline updated: bench/baseline/metrics.json")

    for k, gid, st, det in g.rows:
        if st != "PASS":
            log(f"{st:5s} {k:10s} {gid}: {det}")
    fails = g.failed()
    log(f"report: {out / 'report.md'}")
    log(f"{'FAIL' if fails else 'PASS'}: {len(fails)} failing gate(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
