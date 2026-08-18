#!/usr/bin/env python3
"""Turn maplab's benchmark JSON and experiment CSVs into the figures in docs/img/.

Every figure in the README and EXPERIMENTS.md is produced by this script from files in
results/, so a claim in the prose can always be traced back to a number in a file.

Usage:
    python3 bench/plots/plot.py --results results --out docs/img

Cache boundaries are drawn on the size sweeps. They default to the machine RESULTS.md
describes; override with --l1/--l2/--l3 (bytes) when running elsewhere.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import sys
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, LogLocator, NullLocator
except ImportError:  # pragma: no cover
    sys.exit("matplotlib is required: pip install matplotlib")

# --------------------------------------------------------------------------------------
# Style. One palette, used consistently, so the same implementation is the same colour in
# every figure in the repo.
# --------------------------------------------------------------------------------------
COLORS = {
    "maplab": "#1f77b4",
    "maplab::flat_map": "#1f77b4",
    "maplab (at 7/8 ceiling)": "#5fa2dd",
    "maplab-scalar": "#8fbcd4",
    "std::unordered_map": "#d62728",
    "ankerl::unordered_dense": "#2ca02c",
    "absl::flat_hash_map": "#ff7f0e",
    "simd": "#1f77b4",
    "scalar": "#8fbcd4",
    "hit": "#1f77b4",
    "miss": "#ff7f0e",
}
ACCENT = "#1f77b4"
MUTED = "#8a8a8a"

plt.rcParams.update({
    "figure.dpi": 130,
    "savefig.dpi": 130,
    "font.size": 9,
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.labelsize": 9,
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linestyle": "-",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "legend.frameon": False,
    "legend.fontsize": 8,
    "figure.autolayout": False,
})


def nice_log_yaxis(ax) -> None:
    """1-2-5 decade ticks with plain labels; matplotlib's default labels only the decades,
    which on a 10 ns to 700 ns range means a single tick."""
    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0), numticks=15))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))


def color_for(name: str) -> str:
    return COLORS.get(name, "#7f7f7f")


def si(n: float) -> str:
    for unit, div in (("M", 1e6), ("K", 1e3)):
        if n >= div:
            v = n / div
            return f"{v:.0f}{unit}" if v >= 10 else f"{v:.1f}{unit}"
    return f"{n:.0f}"


def read_csv(path: pathlib.Path) -> list[dict]:
    if not path.exists():
        print(f"  skip: {path} not found")
        return []
    with path.open() as fh:
        return list(csv.DictReader(fh))


def save(fig, out: pathlib.Path, name: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    path = out / name
    fig.savefig(path, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {path}")


def draw_cache_lines(ax, caches: dict[str, int], bytes_per_elem: float) -> None:
    """Mark where the table stops fitting in each cache level, in elements."""
    for label, size in caches.items():
        elems = size / bytes_per_elem
        ax.axvline(elems, color=MUTED, linestyle=":", linewidth=1, zorder=0)
        ax.annotate(label, xy=(elems, 1.005), xycoords=("data", "axes fraction"),
                    ha="center", va="bottom", fontsize=7.5, color=MUTED)


# --------------------------------------------------------------------------------------
# Google Benchmark JSON
# --------------------------------------------------------------------------------------
def load_bench(path: pathlib.Path) -> dict[tuple[str, str], list[tuple[float, float]]]:
    """-> {(workload, impl): [(size, ns), ...]} using the median aggregate."""
    if not path.exists():
        print(f"  skip: {path} not found (run scripts/run_bench.sh)")
        return {}
    with path.open() as fh:
        data = json.load(fh)

    series: dict[tuple[str, str], list[tuple[float, float]]] = defaultdict(list)
    for b in data.get("benchmarks", []):
        if b.get("run_type") == "aggregate" and b.get("aggregate_name") != "median":
            continue
        raw = b["name"]
        base = raw.split("_median")[0].split("_mean")[0].split("_stddev")[0]
        parts = base.split("/")
        if len(parts) < 2:
            continue
        workload, impl = parts[0], parts[1]
        size = None
        for p in reversed(parts[2:]):
            if p.isdigit():
                size = float(p)
                break
        if size is None:
            size = 0.0
        series[(workload, impl)].append((size, b["real_time"]))
    for key in series:
        series[key].sort()
    return series


def plot_cache_sweep(series, out, caches, bytes_per_elem) -> None:
    if not series:
        return
    for which, title in (("sweep_hit", "Successful lookup"),
                         ("sweep_miss", "Unsuccessful lookup")):
        rows = {impl: pts for (w, impl), pts in series.items() if w == which}
        if not rows:
            continue
        fig, ax = plt.subplots(figsize=(7.2, 4.2))
        for impl in sorted(rows, key=lambda k: (k != "maplab", k)):
            pts = rows[impl]
            ax.plot([p[0] for p in pts], [p[1] for p in pts],
                    marker="o", markersize=3.5, linewidth=1.8 if impl == "maplab" else 1.2,
                    color=color_for(impl), label=impl,
                    zorder=3 if impl == "maplab" else 2)
        draw_cache_lines(ax, caches, bytes_per_elem)
        ax.set_xscale("log", base=2)
        # Log-y: the interesting range spans 8 ns to 700 ns, and on a linear axis the
        # cache-resident half of the sweep is squashed into the baseline.
        ax.set_yscale("log")
        ax.set_xlabel("elements in table")
        ax.set_ylabel("ns per lookup (lower is better)")
        ax.set_title(f"{title}: cost vs table size")
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: si(v)))
        nice_log_yaxis(ax)
        ax.legend(loc="upper left")
        ax.annotate("run-to-run spread on this machine is 30-45%; see RESULTS.md",
                    xy=(0.5, -0.16), xycoords="axes fraction", ha="center",
                    fontsize=7, color=MUTED)
        save(fig, out, f"cache_{which.split('_')[1]}.png")


def plot_ops(series, out) -> None:
    if not series:
        return
    workloads = ["insert_reserved", "insert_no_reserve", "find_hit", "find_miss",
                 "churn", "mixed_90_9_1", "iterate"]
    present = [w for w in workloads if any(k[0] == w for k in series)]
    if not present:
        return
    sizes = sorted({s for (w, _), pts in series.items() if w in present for s, _ in pts})
    target = sizes[len(sizes) // 2] if sizes else 0

    impls = sorted({impl for (_, impl) in series}, key=lambda k: (k != "maplab", k))
    fig, ax = plt.subplots(figsize=(8.6, 4.2))
    width = 0.8 / max(len(impls), 1)
    for i, impl in enumerate(impls):
        xs, ys = [], []
        for j, w in enumerate(present):
            pts = dict(series.get((w, impl), []))
            if target in pts:
                xs.append(j + i * width - 0.4 + width / 2)
                ys.append(pts[target])
        ax.bar(xs, ys, width=width, color=color_for(impl), label=impl)
    ax.set_xticks(range(len(present)))
    ax.set_xticklabels(present, rotation=20, ha="right")
    ax.set_ylabel("ns per operation")
    ax.set_yscale("log")
    nice_log_yaxis(ax)
    ax.set_title(f"Core operations at {si(target)} elements (log scale, lower is better)")
    ax.legend(ncol=2)
    save(fig, out, "ops_summary.png")


# --------------------------------------------------------------------------------------
# Experiments
# --------------------------------------------------------------------------------------
def plot_exp1(res, out) -> None:
    rows = read_csv(res / "exp1_simd_vs_scalar.csv")
    if not rows:
        return
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9.4, 3.9))
    for probe in ("simd", "scalar"):
        for lookup, style in (("hit", "-"), ("miss", "--")):
            pts = sorted((int(r["size"]), float(r["ns_per_op"])) for r in rows
                         if r["probe"] == probe and r["lookup"] == lookup)
            if not pts:
                continue
            ax.plot([p[0] for p in pts], [p[1] for p in pts], style, marker="o",
                    markersize=3.5, color=color_for(probe), label=f"{probe} {lookup}",
                    linewidth=1.6)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    nice_log_yaxis(ax)
    ax.set_xlabel("elements")
    ax.set_ylabel("ns per lookup")
    ax.set_title("SIMD vs scalar control scan")
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: si(v)))
    ax.legend()

    for lookup in ("hit", "miss"):
        pts = sorted((int(r["size"]), float(r["speedup_vs_scalar"])) for r in rows
                     if r["probe"] == "simd" and r["lookup"] == lookup)
        ax2.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", markersize=3.5,
                 color=color_for(lookup), label=lookup, linewidth=1.8)
    ax2.axhline(1.0, color=MUTED, linewidth=1, linestyle=":")
    ax2.set_xscale("log", base=2)
    ax2.set_xlabel("elements")
    ax2.set_ylabel("speedup (x)")
    ax2.set_title("How much the SIMD scan buys")
    ax2.xaxis.set_major_formatter(FuncFormatter(lambda v, _: si(v)))
    ax2.legend()
    save(fig, out, "exp1_simd_vs_scalar.png")


def plot_exp2(res, out) -> None:
    rows = read_csv(res / "exp2_load_factor.csv")
    if not rows:
        return
    alpha = [float(r["load_factor"]) for r in rows]
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(10.4, 4.0))
    # The left panel carries a twin y-axis; without extra room its label lands on top
    # of the right panel's.
    fig.subplots_adjust(wspace=0.42)

    ax.plot(alpha, [float(r["hit_ns"]) for r in rows], marker="o", markersize=3.5,
            color=color_for("hit"), label="hit (ns)")
    ax.plot(alpha, [float(r["miss_ns"]) for r in rows], marker="s", markersize=3.5,
            color=color_for("miss"), label="miss (ns)")
    ax.set_xlabel("load factor")
    ax.set_ylabel("ns per lookup")
    axb = ax.twinx()
    axb.plot(alpha, [float(r["bytes_per_elem"]) for r in rows], marker="^", markersize=3.5,
             color=MUTED, linestyle="--", label="bytes/element")
    axb.set_ylabel("bytes per element")
    axb.grid(False)
    ax.set_title("The space/time frontier")
    ax.set_ylim(0, None)
    lines = ax.get_lines() + axb.get_lines()
    ax.legend(lines, [l.get_label() for l in lines], loc="upper left")

    ax2.plot(alpha, [float(r["groups_per_miss"]) for r in rows], marker="s", markersize=3.5,
             color=color_for("miss"), label="mean groups / miss")
    ax2.plot(alpha, [float(r["groups_p99_miss"]) for r in rows], marker="v", markersize=3.5,
             color="#c44e52", linestyle="--", label="p99 groups / miss")
    ax2.plot(alpha, [float(r["groups_per_hit"]) for r in rows], marker="o", markersize=3.5,
             color=color_for("hit"), label="mean groups / hit")
    ax2.set_yscale("log")
    nice_log_yaxis(ax2)
    ax2.set_xlabel("load factor")
    ax2.set_ylabel("groups probed")
    ax2.set_title("Why: probe length vs load")
    ax2.legend(loc="upper left")
    fig.savefig(out / "exp2_load_factor.png", bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"  wrote {out / 'exp2_load_factor.png'}")

    hist = read_csv(res / "exp2_probe_histogram.csv")
    if hist:
        fig, ax = plt.subplots(figsize=(7.2, 3.9))
        loads = [r["max_load"] for r in hist if r["lookup"] == "miss"]
        keep = []
        for lo in loads:
            if lo not in keep:
                keep.append(lo)
        keep = [keep[0], keep[len(keep) // 2], keep[-1]] if len(keep) >= 3 else keep
        cmap = plt.get_cmap("viridis")
        for i, lo in enumerate(keep):
            pts = sorted((int(r["groups"]), float(r["fraction"])) for r in hist
                         if r["lookup"] == "miss" and r["max_load"] == lo)
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", markersize=3,
                    color=cmap(i / max(len(keep) - 1, 1)), label=f"max load {lo}")
        ax.set_yscale("log")
        ax.set_xlabel("groups probed by one unsuccessful lookup")
        ax.set_ylabel("fraction of lookups")
        ax.set_title("Probe-length distribution on the miss path")
        ax.legend()
        save(fig, out, "exp2_probe_histogram.png")


def plot_exp3(res, out) -> None:
    rows = read_csv(res / "exp3_hash_quality.csv")
    if not rows:
        return
    hashes, patterns = [], []
    for r in rows:
        if r["hash"] not in hashes:
            hashes.append(r["hash"])
        if r["pattern"] not in patterns:
            patterns.append(r["pattern"])
    lookup = {(r["hash"], r["pattern"]): r for r in rows}

    # Three panels, because identity_hash fails in two different ways and the cost panel
    # alone cannot tell them apart: `sequential` blows up the number of groups probed
    # (H1 collapses), while `strided` leaves groups at 1.0 and blows up the number of key
    # comparisons instead (H2 collapses).
    fig, axes = plt.subplots(1, 3, figsize=(11.4, 3.9))
    palette = {"identity": "#d62728", "fmix64": "#1f77b4", "wyhash": "#2ca02c"}
    panels = [("hit_ns", "ns per lookup", "Cost"),
              ("groups_per_hit", "mean groups probed", "Mechanism 1: H1 clustering"),
              ("keycmp_per_hit", "mean key comparisons", "Mechanism 2: H2 defeated")]
    width = 0.8 / len(hashes)
    for ax, (field, ylab, title) in zip(axes, panels):
        for i, h in enumerate(hashes):
            xs = [j + i * width - 0.4 + width / 2 for j in range(len(patterns))]
            ys = [float(lookup[(h, p)][field]) for p in patterns]
            ax.bar(xs, ys, width=width, color=palette.get(h, ACCENT), label=h)
        ax.set_xticks(range(len(patterns)))
        ax.set_xticklabels(patterns)
        ax.set_ylabel(ylab)
        ax.set_title(title)
        ax.set_yscale("log")
        nice_log_yaxis(ax)
    axes[0].legend()
    fig.suptitle("What happens when you trust std::hash", fontweight="bold", y=1.02)
    save(fig, out, "exp3_hash_quality.png")


def plot_exp4(res, out) -> None:
    rows = read_csv(res / "exp4_h2_filter.csv")
    if not rows:
        return
    sizes = sorted({int(r["elements"]) for r in rows})
    target = sizes[-1]
    sel = [r for r in rows if int(r["elements"]) == target]
    labels = [r["variant"] for r in sel]
    x = range(len(sel))
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9.4, 3.9))
    colors = [ACCENT, "#d62728", "#ff7f0e"]
    ax.bar(x, [float(r["keycmp_per_miss"]) for r in sel], color=colors[: len(sel)])
    ax.set_ylabel("full key comparisons per miss")
    ax.set_title(f"What the 7 fingerprint bits buy ({si(target)} elements)")
    ax2.bar(x, [float(r["miss_ns"]) for r in sel], color=colors[: len(sel)])
    ax2.set_ylabel("ns per unsuccessful lookup")
    ax2.set_title("...and what that costs in time")
    for a in (ax, ax2):
        a.set_xticks(list(x))
        a.set_xticklabels(labels, rotation=15, ha="right")
    save(fig, out, "exp4_h2_filter.png")


def rolling_median(values: list[float], window: int) -> list[float]:
    half = window // 2
    out = []
    for i in range(len(values)):
        lo = max(0, i - half)
        hi = min(len(values), i + half + 1)
        chunk = sorted(values[lo:hi])
        out.append(chunk[len(chunk) // 2])
    return out


def plot_exp5(res, out) -> None:
    rows = read_csv(res / "exp5_tombstones.csv")
    if not rows:
        return
    variants = []
    for r in rows:
        if r["variant"] not in variants:
            variants.append(r["variant"])
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9.8, 4.0))
    palette = {"drain disabled": "#d62728", "drain enabled": ACCENT}
    for v in variants:
        sel = sorted((int(r["batch"]), r) for r in rows if r["variant"] == v)
        b = [s[0] for s in sel]
        ns = [float(s[1]["ns_per_op"]) for s in sel]
        c = palette.get(v, ACCENT)
        # Raw points faint, rolling median bold. A batch advances the table's state and so
        # cannot be repeated and minimised like every other measurement here; showing both
        # keeps the noise visible instead of hiding it behind the smoothing.
        ax.plot(b, ns, linewidth=0.7, color=c, alpha=0.30)
        ax.plot(b, rolling_median(ns, 21), linewidth=2.0, color=c, label=f"{v} (median of 21)")
        ax2.plot(b, [int(s[1]["bytes"]) / 1024 / 1024 for s in sel], linewidth=2.0, color=c,
                 label=v)
    ax.set_xlabel("batch (65536 insert+erase+lookup triples each)")
    ax.set_ylabel("ns per operation")
    ax.set_title("Churn at constant element count")
    ax.legend(loc="upper left")
    ax2.set_xlabel("batch")
    ax2.set_ylabel("table footprint (MiB)")
    ax2.set_ylim(0, None)
    ax2.set_title("...and what the table does about it")
    ax2.legend(loc="center right")
    save(fig, out, "exp5_tombstones.png")


def plot_exp6(res, out) -> None:
    rows = read_csv(res / "exp6_group_size.csv")
    if not rows:
        return
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(9.4, 3.9))
    for width in ("16", "8"):
        for probe, style in (("simd", "-"), ("scalar", "--")):
            pts = sorted((int(r["elements"]), float(r["miss_ns"])) for r in rows
                         if r["width"] == width and r["probe"] == probe)
            if not pts:
                continue
            ax.plot([p[0] for p in pts], [p[1] for p in pts], style, marker="o",
                    markersize=3.5, label=f"width {width}, {probe}",
                    color=ACCENT if width == "16" else "#ff7f0e")
        pts = sorted((int(r["elements"]), float(r["groups_per_miss"])) for r in rows
                     if r["width"] == width and r["probe"] == "simd")
        ax2.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", markersize=3.5,
                 label=f"width {width}", color=ACCENT if width == "16" else "#ff7f0e")
    for a, ylab, title in ((ax, "ns per unsuccessful lookup", "Group width 8 vs 16"),
                           (ax2, "mean groups per miss", "Why: a narrower group proves absence less often")):
        a.set_xscale("log", base=2)
        a.set_xlabel("elements")
        a.set_ylabel(ylab)
        a.set_title(title)
        a.xaxis.set_major_formatter(FuncFormatter(lambda v, _: si(v)))
        a.legend()
    save(fig, out, "exp6_group_size.png")


def plot_exp7(res, out) -> None:
    rows = read_csv(res / "exp7_memory.csv")
    if not rows:
        return
    # Rows arrive in blocks, one block per table size, with the same implementations in
    # the same order. Grouping by element count would not work: the "at ceiling" row
    # deliberately holds a different number of elements than the others in its block.
    impls = []
    for r in rows:
        if r["impl"] not in impls:
            impls.append(r["impl"])
        else:
            break
    blocks = [rows[i:i + len(impls)] for i in range(0, len(rows), len(impls))]

    fig, ax = plt.subplots(figsize=(7.6, 4.0))
    width = 0.8 / len(impls)
    for i, impl in enumerate(impls):
        xs = [j + i * width - 0.4 + width / 2 for j in range(len(blocks))]
        ys = [float(b[i]["bytes_per_elem"]) for b in blocks]
        bars = ax.bar(xs, ys, width=width, color=color_for(impl), label=impl)
        ax.bar_label(bars, fmt="%.1f", fontsize=7, padding=1)
    ax.set_xticks(range(len(blocks)))
    ax.set_xticklabels([si(float(b[0]["elements"])) for b in blocks])
    ax.set_xlabel("elements")
    ax.set_ylabel("bytes per element")
    ax.set_ylim(0, max(float(r["bytes_per_elem"]) for r in rows) * 1.18)
    ax.set_title("Memory per element (glibc chunk sizes, not requested sizes)")
    ax.legend(ncol=2, loc="upper right")
    save(fig, out, "exp7_memory.png")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", type=pathlib.Path, default=pathlib.Path("results"))
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("docs/img"))
    ap.add_argument("--l1", type=int, default=32 * 1024, help="L1d bytes per core")
    ap.add_argument("--l2", type=int, default=256 * 1024, help="L2 bytes per core")
    ap.add_argument("--l3", type=int, default=6 * 1024 * 1024, help="L3 bytes, shared")
    ap.add_argument("--bytes-per-elem", type=float, default=17.0,
                    help="slot + control byte, for placing the cache lines in element units")
    args = ap.parse_args()

    caches = {"L1d": args.l1, "L2": args.l2, "L3": args.l3}
    print(f"reading {args.results}/ -> writing {args.out}/")

    series = load_bench(args.results / "bench.json")
    plot_cache_sweep(series, args.out, caches, args.bytes_per_elem)
    plot_ops(series, args.out)

    for fn in (plot_exp1, plot_exp2, plot_exp3, plot_exp4, plot_exp5, plot_exp6, plot_exp7):
        fn(args.results, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
