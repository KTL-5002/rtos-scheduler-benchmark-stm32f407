#!/usr/bin/env python3
"""
analyze.py — turn the sweep's UART log(s) into tables and figures.

Usage:
    python analyze.py sweep_exp2.log sweep_exp1.log sweep_exp3.log [-o results]
        [--table-set A] [--table-u 90] [--exclude exclusions.csv]

Reads the machine-readable lines the firmware prints (anything else in the
PuTTY log is ignored):  META, CSV, HIST, OVH, UHIST, each preceded by a header
line (METAH, CSVH, ...), so logs from older firmware versions still parse.
If the same run point (experiment, scheduler, set, U, trial, logging) appears
more than once, the LAST occurrence wins: re-run single points and append.

--exclude takes a CSV with columns experiment,run,reason. Those runs are
dropped and listed, with the reason, in coverage.md.

Output (default ./results):
  coverage.md                 Section A: matrix, trials, durations, releases,
                              completeness, exclusions, provenance, clocks
  exp1_task_sets/             experiment 1 (fixed 5-task sets x U)
      table3_paired.csv/.md   Table III at --table-set/--table-u, trial stats
      table4_overhead.csv/.md Table IV at the same point (+ every U in csv)
      figT1_response_distribution.png   Fig. 1
      figT2_miss_fraction.png           Fig. 2 (error bars = min..max over trials)
      figT3_update_delay.png            Fig. 3
      fig_motor_miss.png, fig_first_miss.png, fig_norm_response.png,
      fig_cpu_util.png, fig_edf_overhead.png, summary.csv, results.csv
  exp2_task_count/            experiment 2 (task count N x U)
      paper_tables.md, table1/2/3 csv   tables in the Zhang & Li (2025) format
      figN1..figN6                      figures vs N
  exp3_observer/              experiment 3 (deferred vs intrusive logging)
      figT4_observer_effect.png         Fig. 4
      observer_effect.csv/.md
  exp4_repeatability/         experiment 4 (identical repeats of critical points)
      repeatability.csv/.md, fig_repeatability.png

Requires: pandas, matplotlib   (pip install pandas matplotlib)
"""
import argparse
import os
import re
import sys

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402

# ===================================================================== style
# Fixed colours per scheduler (never per rank) plus a second encoding (line
# style + marker) so DMS / EDF stay distinct in grayscale print.
STYLE = {
    "DMS": dict(color="#2a78d6", linestyle="-", marker="o"),
    "EDF": dict(color="#eb6834", linestyle="--", marker="s"),
}
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({
    "figure.dpi": 150, "savefig.dpi": 150, "font.size": 9,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "axes.titlesize": 10,
    "axes.titleweight": "bold", "axes.titlecolor": INK,
    "xtick.color": INK2, "ytick.color": INK2,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "lines.linewidth": 2, "lines.markersize": 4.5,
})

CPU_HZ = 168_000_000
CYC_PER_US = 168.0
CYC_PER_TICK = 16_800

SET_LABEL = {
    "A": "A · harmonic, D < T",
    "B": "B · non-harmonic, D = T",
    "C": "C · non-harmonic, tight D",
    "D": "D · harmonic, D = T (control)",
}

# Column layouts of the first firmware version (logs without header lines).
LEGACY = {
    "CSV": ["run", "sched", "set", "u_permille", "task", "T_ms", "D_ms", "jobs", "misses",
            "max_us", "min_us", "due", "done", "unfinished", "total_miss", "idle", "window_ms"],
    "OVH": ["run", "sched", "set", "u_permille", "hook_n", "hook_avg_cyc", "hook_max_cyc",
            "recomp_n", "recomp_avg_cyc", "recomp_max_cyc", "releases", "prio_writes"],
}
TEXT_COLS = {"sched", "set", "task", "fw_version", "git_commit", "build_id",
             "build_date", "build_time"}
POINT = ["exp", "sched", "set", "u_permille", "trial", "obs", "rep"]


def set_label(s, multiline=False):
    if str(s).startswith("GEN"):
        return str(s)
    lab = SET_LABEL.get(str(s)[:1], str(s).replace("_", " "))
    return lab.replace(" · ", "\n") if multiline else lab


def fmt_ms(x):
    return "–" if pd.isna(x) else f"{x:.3f}"


# ===================================================================== parse
TAG_RE = re.compile(r"(?<![A-Z])(METAH|META|CSVH|CSV|HISTH|HIST|OVHH|OVH|UHISTH|UHIST),([^\r\n]*)")


def parse_logs(paths):
    """Header-driven: every data line is read with the columns of the most
    recent header line of its kind."""
    cols = {"CSV": list(LEGACY["CSV"]), "OVH": list(LEGACY["OVH"])}
    data = {k: [] for k in ("META", "CSV", "HIST", "OVH", "UHIST")}
    for path in paths:
        cur_exp = np.nan        # experiment of the run block being read
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = TAG_RE.search(line)
                if not m:
                    continue
                tag, parts = m.group(1), m.group(2).strip().split(",")
                if tag.endswith("H") and tag[:-1] in data:
                    cols[tag[:-1]] = parts
                elif tag in data and tag in cols and len(parts) == len(cols[tag]):
                    row = dict(zip(cols[tag], parts))
                    # META and CSV carry the experiment; the HIST / OVH / UHIST
                    # lines that follow in the same run block inherit it
                    if tag in ("META", "CSV") and "experiment" in row:
                        try:
                            cur_exp = float(row["experiment"])
                        except ValueError:
                            pass
                    row["_exp"] = cur_exp
                    data[tag].append(row)
    if not data["CSV"]:
        sys.exit("No CSV lines found. Is this the sweep's UART log?")

    frames = {}
    for k, rows in data.items():
        d = pd.DataFrame(rows)
        for c in d.columns:
            if c not in TEXT_COLS:
                d[c] = pd.to_numeric(d[c], errors="coerce")
        if not d.empty:
            for c, default in (("trial", 0), ("obs", 0), ("rep", 0)):
                if c not in d.columns:
                    d[c] = default
            # experiment: from the run block; manual runs (0) and old logs
            # without it are classified by task-set name
            guess = np.where(d["set"].astype(str).str.startswith("GEN"), 2, 1)
            e = pd.to_numeric(d.pop("_exp"), errors="coerce")
            d["exp"] = np.where(e.notna() & (e > 0), e, guess).astype(int)
        frames[k] = d
    c = frames["CSV"]

    # last occurrence of a point wins
    frames["CSV"] = c.drop_duplicates(POINT + ["task"], keep="last")
    for k, extra in (("HIST", ["task"]), ("META", []), ("OVH", []), ("UHIST", [])):
        if not frames[k].empty:
            frames[k] = frames[k].drop_duplicates(POINT + extra, keep="last")

    df = frames["CSV"]
    df["U"] = df["u_permille"] / 10.0
    df["miss_pct"] = 100.0 * df["total_miss"] / df["due"].where(df["due"] > 0)
    df["norm_resp"] = df["max_us"] / (df["D_ms"] * 1000.0)
    df["cpu_pct"] = df["cpu_x100"] / 100.0 if "cpu_x100" in df else np.nan
    for k in ("OVH", "UHIST", "META", "HIST"):
        if not frames[k].empty:
            frames[k]["U"] = frames[k]["u_permille"] / 10.0
    return frames


def apply_exclusions(frames, path):
    if not path:
        return frames, pd.DataFrame(columns=["experiment", "run", "reason"])
    ex = pd.read_csv(path, encoding="utf-8")
    ex.columns = [c.strip() for c in ex.columns]
    drop = set(zip(ex.experiment.astype(int), ex.run.astype(int)))
    for k, d in frames.items():
        if not d.empty and "run" in d:
            frames[k] = d[~d.apply(lambda r: (int(r.exp), int(r.run)) in drop, axis=1)]
    return frames, ex


# ===================================================================== plots
def panels(data, panel_col, panel_vals, title_fn, x, y, ylabel, xlabel, title, path, *,
           ref=None, ref_fn=None, logy=False, ylim=None, xticks=None, ncols=2,
           scheds=("DMS", "EDF"), err=None):
    """Small multiples: one panel per value of panel_col, DMS vs EDF lines.
    err = (lo_col, hi_col) draws error bars."""
    n = len(panel_vals)
    cols = min(ncols, n)
    rows = (n + cols - 1) // cols
    size = (6.8, 3.6) if n == 1 else (4.0 * cols, 2.9 * rows)
    fig, axes = plt.subplots(rows, cols, figsize=size, sharex=True, sharey=not logy, squeeze=False)
    xs_all = sorted(data[x].dropna().unique())
    span = (xs_all[-1] - xs_all[0]) or 1
    for ax, pv in zip(axes.flat, panel_vals):
        d = data[data[panel_col] == pv] if panel_col else data
        for sch in scheds:
            e = d[d.sched == sch].sort_values(x)
            e = e[e[y].notna()]
            if e.empty:
                continue
            if err:
                ax.errorbar(e[x], e[y], yerr=[(e[y] - e[err[0]]).clip(lower=0), (e[err[1]] - e[y]).clip(lower=0)],
                            capsize=2, elinewidth=1, label=sch, **STYLE[sch])
            else:
                ax.plot(e[x], e[y], label=sch, **STYLE[sch])
            if len(scheds) > 1:
                ax.annotate(sch, (e[x].iloc[-1], e[y].iloc[-1]), xytext=(5, 6 if sch == "DMS" else -6),
                            textcoords="offset points", va="center", fontsize=8, color=INK2)
        if ref is not None:
            ax.axhline(ref, color=INK2, linewidth=0.8, linestyle=":")
        if ref_fn is not None:
            ax.axhline(ref_fn(pv), color=INK2, linewidth=0.8, linestyle=":")
        if logy:
            ax.set_yscale("log")
            ax.yaxis.set_major_locator(matplotlib.ticker.LogLocator(subs=(1, 2, 5)))
            ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
            ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
        if ylim:
            ax.set_ylim(*ylim)
        if xticks is not None:
            ax.set_xticks(xticks)
        ax.set_xlim(xs_all[0] - 0.04 * span, xs_all[-1] + 0.12 * span)
        ax.set_title(title_fn(pv), loc="left")
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
    for ax in list(axes.flat)[n:]:
        ax.set_visible(False)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    if n == 1:
        axes.flat[0].set_title(title, loc="left")
        if len(labels) > 1:
            axes.flat[0].legend(loc="best", ncol=2)
        fig.tight_layout()
    else:
        # title on its own line, legend on the line below it
        fig.suptitle(title, x=0.01, y=0.995, ha="left", va="top", fontweight="bold", color=INK)
        top = 1 - (0.11 if rows > 1 else 0.20)
        if len(labels) > 1:
            fig.legend(handles, labels, loc="upper left", ncol=len(labels),
                       bbox_to_anchor=(0.005, 1 - (0.035 if rows > 1 else 0.075)))
        fig.tight_layout(rect=(0, 0, 1, top))
    fig.savefig(path)
    plt.close(fig)


def first_miss_summary(df, key):
    """First U with any miss in ANY trial (conservative), per (key, sched)."""
    out = []
    for (k, sch), d in df.groupby([key, "sched"]):
        runs = d.groupby("U").agg(miss=("total_miss", "sum")).reset_index()
        failing = runs[runs.miss > 0]
        first_u = failing.U.min() if not failing.empty else np.nan
        passing = runs[runs.U < first_u] if not failing.empty else runs
        first_tasks = ""
        if not failing.empty:
            at = d[(d.U == first_u) & (d.total_miss > 0)]
            first_tasks = " ".join(sorted(set(at.task.astype(str))))
        motor = d[d.task == "motor"]
        m110 = motor[motor.U == 110].miss_pct
        out.append({key: k, "sched": sch,
                    "last_U_all_deadlines_met": passing.U.max() if not passing.empty else np.nan,
                    "first_U_with_miss": first_u, "first_tasks_to_miss": first_tasks,
                    "motor_miss_pct_at_110_mean": float(m110.mean()) if len(m110) else np.nan,
                    "trials": int(d.trial.nunique()), "max_U_tested": d.U.max()})
    return pd.DataFrame(out)


def run_level(df, keys):
    """One row per run (all trials kept), then mean / min / max over trials."""
    runs = (df.groupby(keys + ["trial"])
              .agg(total_miss=("total_miss", "sum"), due=("due", "sum"),
                   worst_norm_resp=("norm_resp", "max"), cpu_pct=("cpu_pct", "first"),
                   idle=("idle", "first"))
              .reset_index())
    runs["miss_pct"] = 100.0 * runs.total_miss / runs.due
    agg = (runs.groupby(keys)
               .agg(miss_pct=("miss_pct", "mean"), miss_lo=("miss_pct", "min"), miss_hi=("miss_pct", "max"),
                    worst_norm_resp=("worst_norm_resp", "mean"), wr_lo=("worst_norm_resp", "min"),
                    wr_hi=("worst_norm_resp", "max"), cpu_pct=("cpu_pct", "mean"),
                    cpu_lo=("cpu_pct", "min"), cpu_hi=("cpu_pct", "max"),
                    trials=("trial", "nunique"))
               .reset_index())
    return runs, agg


def edf_overhead(od, window_s):
    e = od[od.sched == "EDF"].copy()
    if e.empty:
        return e
    e["hook_pct"] = 100.0 * e.hook_avg_cyc / CYC_PER_TICK
    if "mgr_cpu_cyc" in e and e.mgr_cpu_cyc.notna().any():
        e["mgr_pct"] = 100.0 * e.mgr_cpu_cyc / (CPU_HZ * window_s)
    else:   # older logs: elapsed time as an upper bound
        e["mgr_pct"] = 100.0 * e.recomp_avg_cyc * e.recomp_n / (CPU_HZ * window_s)
    e["edf_overhead_pct"] = e.hook_pct + e.mgr_pct
    return e


# ================================================================ coverage (A)
def coverage(frames, ex, out):
    df, meta = frames["CSV"], frames["META"]
    lines = ["# A. Experiment coverage and data integrity\n",
             "Generated by analyze.py from the UART logs. One run = one fresh boot of the "
             "board and one measurement window.\n"]
    names = {1: "Experiment 1 — fixed task sets × U", 2: "Experiment 2 — task count × U",
             3: "Experiment 3 — observer effect (deferred vs intrusive logging)",
             4: "Experiment 4 — repeatability (identical repeats of critical points)"}
    for exp, d in df.groupby("exp"):
        runs = d.drop_duplicates(POINT)
        pts = runs.groupby(["sched", "set", "u_permille", "obs"]).trial.nunique()
        tmax = int(pts.max())
        short = pts[pts < tmax]
        nrep = int(runs.rep.max()) + 1 if "rep" in runs else 1
        win = sorted(d.window_ms.dropna().unique())
        lines += [f"\n## {names.get(exp, f'Experiment {exp}')}\n",
                  f"- Runs received: **{len(runs)}** "
                  f"({runs['set'].nunique()} task set(s) / N, {runs.u_permille.nunique()} U levels, "
                  f"{runs.sched.nunique()} schedulers, {runs.obs.nunique()} logging mode(s), "
                  f"up to {tmax} trial(s) per point"
                  + (f", {nrep} identical repeats per point)" if nrep > 1 else ")"),
                  f"- Measurement window per run: {', '.join(f'{w/1000:g} s' for w in win)}; "
                  f"total measured time {len(runs) * (win[0] if win else 0) / 1000 / 60:.1f} min",
                  f"- Releases with a deadline inside the window: **{int(d.due.sum()):,}**; "
                  f"completed jobs: **{int(d.done.sum()):,}**; never completed "
                  f"(counted as misses): **{int(d.unfinished.sum()):,}**",
                  f"- Points with fewer than {tmax} trials: **{len(short)}**"
                  + ("" if short.empty else " → " + "; ".join(
                      f"{s} {st} U={u/10:g}% obs={o}: {n}" for (s, st, u, o), n in short.items()))]
        # counter-freeze check: an EDF run's hook must fire exactly once per
        # window tick (v0.5.1+). Older firmware kept counting while printing.
        ovh = frames["OVH"]
        if not ovh.empty and "hook_n" in ovh:
            e = ovh[(ovh.exp == exp) & (ovh.sched == "EDF")]
            if not e.empty and win:
                want = int(win[0]) * 10            # 10 kHz tick
                ok = (e.hook_n - want).abs() <= 1
                lines.append(f"- EDF counter freeze check (hook calls = {want:,} window ticks): "
                             + ("**PASS** in all %d EDF runs" % len(e) if ok.all() else
                                f"**FAIL** in {int((~ok).sum())} of {len(e)} EDF runs "
                                f"(hook calls {int(e.hook_n.min()):,}–{int(e.hook_n.max()):,}; "
                                "counters kept running after the window - firmware before v0.5.1)"))
        hist = frames["HIST"]
        if not hist.empty:
            h = hist[hist.exp == exp]
            nh = len(h.drop_duplicates(POINT))
            lines.append(f"- Runs with response-time histograms: {nh} / {len(runs)}; "
                         f"runs with META provenance: {len(meta[meta.exp == exp].drop_duplicates(POINT)) if not meta.empty else 0} / {len(runs)}")
    lines.append("\n## Exclusions\n")
    if ex.empty:
        lines.append("No runs excluded.")
    else:
        lines.append("| Experiment | Run | Reason |\n|---:|---:|---|")
        lines += [f"| {int(r.experiment)} | {int(r.run)} | {r.reason} |" for r in ex.itertuples()]
    lines.append("\n## Provenance and clock / tick consistency\n")
    if meta.empty:
        lines.append("No META lines found (log from older firmware): provenance not recorded.")
    else:
        for c in ("fw_version", "git_commit", "build_id", "build_date", "build_time"):
            if c in meta:
                lines.append(f"- {c}: {', '.join(sorted(set(meta[c].astype(str))))}")
        commits = set(meta["git_commit"].astype(str)) if "git_commit" in meta else {"unknown"}
        dirty = meta["git_dirty"].dropna().astype(int).max() if "git_dirty" in meta and meta["git_dirty"].notna().any() else None
        if commits == {"unknown"}:
            lines.append("- Source traceability: **NOT TRACEABLE** — git commit not recorded")
        elif dirty:
            lines.append("- Source traceability: **PARTIAL** — built from the commit above PLUS uncommitted changes (dirty)")
        else:
            lines.append("- Source traceability: **OK** — built from the clean commit above")
        ok = True
        for c, want in (("sysclk_hz", 168_000_000), ("tick_hz", 10_000), ("cyc_per_tick", 16_800)):
            if c in meta:
                vals = sorted(set(meta[c].dropna().astype(int)))
                good = vals == [want]
                ok &= good
                lines.append(f"- {c}: {', '.join(map(str, vals))} {'✓' if good else '✗ UNEXPECTED'}")
        lines.append(f"- Clock/tick consistency: {'consistent in every run' if ok else 'CHECK the values above'}")
        lines.append(f"- Trial 0 = synchronous release (critical instant); trial t ≥ 1 = random "
                     f"per-task release offsets, seed t, identical for DMS and EDF (paired).")
    with open(os.path.join(out, "coverage.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# ====================================================== experiment 1 (Tables III, IV; Figs 1-3)
def experiment1(frames, out, table_set, table_u):
    os.makedirs(out, exist_ok=True)
    df = frames["CSV"][frames["CSV"].exp == 1]
    od = frames["OVH"][frames["OVH"].exp == 1] if not frames["OVH"].empty else frames["OVH"]
    hist = frames["HIST"][frames["HIST"].exp == 1] if not frames["HIST"].empty else frames["HIST"]
    uh = frames["UHIST"][frames["UHIST"].exp == 1] if not frames["UHIST"].empty else frames["UHIST"]
    sets = sorted(df.set.unique())
    tset = next((s for s in sets if str(s).startswith(table_set)), sets[0])
    tu = table_u if (df.U == table_u).any() else sorted(df.U.unique())[len(df.U.unique()) // 2]
    window_s = df.window_ms.iloc[0] / 1000.0

    df.to_csv(os.path.join(out, "results.csv"), index=False)
    runs, agg = run_level(df, ["set", "sched", "U"])
    summ = first_miss_summary(df, "set")
    summ.to_csv(os.path.join(out, "summary.csv"), index=False)

    # ---------------- Table III: paired per-task results at one workload
    p = df[(df.set == tset) & (df.U == tu)]
    t3 = []
    for task in list(dict.fromkeys(p.task)):
        row = {"task": task}
        for sch in ("DMS", "EDF"):
            q = p[(p.task == task) & (p.sched == sch)]
            if q.empty:
                continue
            row[f"{sch}_maxR_us_mean"] = q.max_us.mean()
            row[f"{sch}_maxR_us_sd"] = q.max_us.std(ddof=1) if len(q) > 1 else 0.0
            row[f"{sch}_maxR_us_trial0"] = q[q.trial == 0].max_us.mean()
            row[f"{sch}_misses"] = int(q.total_miss.sum())
            row[f"{sch}_releases"] = int(q.due.sum())
            row[f"{sch}_trials"] = q.trial.nunique()
            row["D_ms"] = q.D_ms.iloc[0]
        t3.append(row)
    t3 = pd.DataFrame(t3)
    t3.to_csv(os.path.join(out, "table3_paired.csv"), index=False)
    with open(os.path.join(out, "table3_paired.md"), "w", encoding="utf-8") as f:
        f.write(f"**Table III — Paired results, set {set_label(tset)}, U = {tu:g}%, "
                f"{window_s:g} s window per trial**\n\n"
                "Max R = largest response time in a trial, shown as mean ± SD over trials "
                "(trial 0 = synchronous release in brackets). Misses / releases are summed over "
                "trials; releases count jobs whose deadline fell inside the window, and jobs never "
                "completed count as misses. A dash means unavailable, not zero.\n\n"
                "| Task | D (ms) | Max R DMS (µs) | Max R EDF (µs) | Misses / releases DMS | Misses / releases EDF |\n"
                "|---|---:|---:|---:|---:|---:|\n")
        for r in t3.itertuples():
            def mr(s):
                m = getattr(r, f"{s}_maxR_us_mean", np.nan)
                return "–" if pd.isna(m) else (f"{m:,.0f} ± {getattr(r, f'{s}_maxR_us_sd'):,.0f} "
                                              f"[{getattr(r, f'{s}_maxR_us_trial0'):,.0f}]")
            def mm(s):
                v = getattr(r, f"{s}_misses", np.nan)
                return "–" if pd.isna(v) else f"{int(v):,} / {int(getattr(r, f'{s}_releases')):,}"
            f.write(f"| {r.task} | {r.D_ms:g} | {mr('DMS')} | {mr('EDF')} | {mm('DMS')} | {mm('EDF')} |\n")

    # ---------------- Fig. 1: per-task response-time distributions at that point
    if not hist.empty:
        h = hist[(hist.set == tset) & (hist.U == tu)]
        bcols = [c for c in h.columns if re.fullmatch(r"(lt|ge)\d+", str(c))]
        tasks = list(dict.fromkeys(h.task))
        if tasks and bcols:
            fig, axes = plt.subplots(1, len(tasks), figsize=(2.6 * len(tasks), 3.2), sharey=True, squeeze=False)
            x = np.arange(len(bcols))
            labels = [("<" + c[2:]) if c.startswith("lt") else ("≥" + c[2:]) for c in bcols]
            for ax, task in zip(axes.flat, tasks):
                for i, sch in enumerate(("DMS", "EDF")):
                    q = h[(h.task == task) & (h.sched == sch)]
                    if q.empty:
                        continue
                    counts = q[bcols].sum().values.astype(float)
                    n = counts.sum()
                    frac = counts / n if n else counts
                    ax.bar(x + (i - 0.5) * 0.4, 100 * frac, width=0.38, color=STYLE[sch]["color"],
                           hatch=None if sch == "DMS" else "//", edgecolor="white", linewidth=0.4,
                           label=f"{sch} (n={int(n):,})")
                miss_edge = next((j for j, c in enumerate(bcols) if c == "lt125"), 10)
                ax.axvline(miss_edge - 0.5, color=INK2, linestyle=":", linewidth=0.9)
                d_ms = h[h.task == task].D_ms.iloc[0]
                ax.set_title(f"{task}  (D = {d_ms:g} ms)", loc="left", fontsize=9)
                ax.set_xticks(x[::3], labels[::3], fontsize=7)
                ax.set_xlabel("Response / deadline (%)")
                ax.legend(fontsize=7, loc="best")
                ax.grid(axis="x", visible=False)
            axes.flat[0].set_ylabel("Jobs (% of completed)")
            fig.suptitle(f"Response-time distributions, set {set_label(tset)}, U = {tu:g}% "
                         f"(all trials pooled; right of the dotted line = deadline missed)",
                         x=0.01, ha="left", fontweight="bold", color=INK, fontsize=10)
            fig.tight_layout(rect=(0, 0, 1, 0.9))
            fig.savefig(os.path.join(out, "figT1_response_distribution.png"))
            plt.close(fig)

    # ---------------- Fig. 2: miss fraction vs demand, error bars over trials
    common = dict(xlabel="Target utilization (%)", title_fn=set_label, xticks=list(range(50, 131, 10)))
    panels(agg, "set", sets, x="U", y="miss_pct", err=("miss_lo", "miss_hi"),
           ylabel="Deadline misses (% of releases)",
           title="Deadline-miss fraction vs utilization (unfinished jobs count as misses; bars = min–max over trials)",
           path=os.path.join(out, "figT2_miss_fraction.png"), ylim=(-3, 103), **common)
    motor = df[df.task == "motor"].groupby(["set", "sched", "U"]).agg(
        miss_pct=("miss_pct", "mean"), lo=("miss_pct", "min"), hi=("miss_pct", "max")).reset_index()
    panels(motor, "set", sets, x="U", y="miss_pct", err=("lo", "hi"), ylabel="Motor misses (% of releases)",
           title="Critical task (motor) deadline misses vs utilization",
           path=os.path.join(out, "fig_motor_miss.png"), ylim=(-3, 103), **common)
    panels(agg, "set", sets, x="U", y="worst_norm_resp", err=("wr_lo", "wr_hi"),
           ylabel="Worst max response / deadline",
           title="Worst normalized response time (above the dotted line = deadline missed)",
           path=os.path.join(out, "fig_norm_response.png"), ref=1.0, logy=True, **common)
    if agg.cpu_pct.notna().any():
        panels(agg, "set", sets, x="U", y="cpu_pct", err=("cpu_lo", "cpu_hi"),
               ylabel="Measured CPU utilization (%)", title="Measured CPU utilization vs target (dotted: 100%)",
               path=os.path.join(out, "fig_cpu_util.png"), ref=100.0, **common)

    # first-miss bars
    fig, ax = plt.subplots(figsize=(7, 3.2))
    for i, sch in enumerate(("DMS", "EDF")):
        d = summ[summ.sched == sch].set_index("set").reindex(sets)
        xs = [j + (i - 0.5) * 0.36 for j in range(len(sets))]
        ax.bar(xs, d.first_U_with_miss, width=0.32, label=sch, color=STYLE[sch]["color"],
               hatch=None if sch == "DMS" else "//", edgecolor="white", linewidth=0.5)
        for xi, v in zip(xs, d.first_U_with_miss):
            if pd.notna(v):
                ax.annotate(f"{v:g}", (xi, v), xytext=(0, 2), textcoords="offset points",
                            ha="center", fontsize=8, color=INK)
    ax.set_xticks(range(len(sets)), [set_label(s, True) for s in sets])
    if summ.first_U_with_miss.notna().any():
        ax.set_ylim(max(0, summ.first_U_with_miss.min() - 10), summ.first_U_with_miss.max() + 9)
    ax.set_ylabel("Utilization at first miss (%)")
    ax.set_title("Utilization at which each scheduler first misses a deadline (any trial)", loc="left")
    ax.legend(loc="upper left", ncol=2)
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "fig_first_miss.png"))
    plt.close(fig)

    # ---------------- Table IV + Fig. 3: overhead and update latency
    if not od.empty:
        e = edf_overhead(od, window_s)
        e.to_csv(os.path.join(out, "overhead_all.csv"), index=False)
        panels(e.groupby(["set", "U"]).agg(edf_overhead_pct=("edf_overhead_pct", "mean")).reset_index().assign(sched="EDF"),
               "set", sets, x="U", y="edf_overhead_pct", ylabel="EDF overhead (% CPU)",
               title="EDF scheduling overhead (tick hook + manager CPU time) vs utilization",
               path=os.path.join(out, "fig_edf_overhead.png"), scheds=("EDF",),
               ylim=(0, max(3.0, e.edf_overhead_pct.max() * 1.2)), **common)

        q = e[(e.set == tset) & (e.U == tu)]
        dms_cpu = df[(df.set == tset) & (df.U == tu) & (df.sched == "DMS")].drop_duplicates("trial").cpu_pct.mean()
        edf_cpu = df[(df.set == tset) & (df.U == tu) & (df.sched == "EDF")].drop_duplicates("trial").cpu_pct.mean()
        rows = []
        if not q.empty:
            hn = q.hook_n.sum()
            rows.append(("Hook CPU time (µs)", hn, (q.hook_avg_cyc * q.hook_n).sum() / hn / CYC_PER_US if hn else np.nan,
                         q.hook_max_cyc.max() / CYC_PER_US))
            rn = q.recomp_n.sum()
            has_mgr = "mgr_cpu_cyc" in q and q.mgr_cpu_cyc.notna().any()
            rows.append(("Manager CPU time per update (µs)", rn,
                         q.mgr_cpu_cyc.sum() / rn / CYC_PER_US if (has_mgr and rn) else np.nan, np.nan))
            rows.append(("Manager elapsed time per update (µs)", rn,
                         (q.recomp_avg_cyc * q.recomp_n).sum() / rn / CYC_PER_US if rn else np.nan,
                         q.recomp_max_cyc.max() / CYC_PER_US))
            if "upd_n" in q and q.upd_n.notna().any():
                un = q.upd_n.sum()
                rows.append(("Update delay, detection → priorities written (µs)", un,
                             (q.upd_avg_cyc * q.upd_n).sum() / un / CYC_PER_US if un else np.nan,
                             q.upd_max_cyc.max() / CYC_PER_US))
            rows.append(("Priority writes per update", q.prio_writes.sum(),
                         q.prio_writes.sum() / rn if rn else np.nan, np.nan))
        t4 = pd.DataFrame(rows, columns=["quantity", "count", "mean", "obs_max"])
        t4.to_csv(os.path.join(out, "table4_overhead.csv"), index=False)
        with open(os.path.join(out, "table4_overhead.md"), "w", encoding="utf-8") as f:
            f.write(f"**Table IV — EDF management cost and priority-update latency, set {set_label(tset)}, "
                    f"U = {tu:g}%, {q.trial.nunique() if not q.empty else 0} trial(s) × {window_s:g} s**\n\n"
                    "Hook = tick-hook ISR, measured with DWT around its body (CPU time). Manager CPU "
                    "time = FreeRTOS run-time stats for the manager task (excludes interrupts and "
                    "preempting tasks). Manager elapsed time = DWT from wake-up to priorities written "
                    "(includes interrupts). Update delay = event detection (release in the hook, or a "
                    "backlogged job at completion) → priorities written; events arriving while one is "
                    "pending are folded into it. Counters cover exactly the measurement window.\n\n"
                    "| Quantity | Count | Mean | Obs. max |\n|---|---:|---:|---:|\n")
            for r in t4.itertuples():
                f.write(f"| {r.quantity} | {int(r.count):,} | "
                        f"{'–' if pd.isna(r.mean) else f'{r.mean:.2f}'} | "
                        f"{'–' if pd.isna(r.obs_max) else f'{r.obs_max:.2f}'} |\n")
            if not q.empty:
                f.write(f"\nProcessor fraction (mean over trials): hook {q.hook_pct.mean():.2f} %, "
                        f"manager {q.mgr_pct.mean():.2f} %, total {q.edf_overhead_pct.mean():.2f} %. "
                        f"DMS baseline: no scheduler-specific runtime work (FreeRTOS's own fixed-priority "
                        f"scheduler; one compare per tick in the dispatcher). Measured CPU utilization at "
                        f"this point: DMS {dms_cpu:.2f} %, EDF {edf_cpu:.2f} % (difference "
                        f"{edf_cpu - dms_cpu:+.2f} points).\n")

        if not uh.empty:
            u = uh[(uh.set == tset) & (uh.U == tu) & (uh.sched == "EDF")]
            bcols = [c for c in uh.columns if re.fullmatch(r"(lt|ge)\d+us", str(c))]
            if not u.empty and bcols:
                counts = u[bcols].sum().values.astype(float)
                n = counts.sum()
                fig, ax = plt.subplots(figsize=(6.8, 3.4))
                lab = [("<" + c[2:-2]) if c.startswith("lt") else ("≥" + c[2:-2]) for c in bcols]
                ax.bar(range(len(bcols)), 100 * counts / n if n else counts, width=0.7,
                       color=STYLE["EDF"]["color"], edgecolor="white")
                for j, v in enumerate(counts):
                    if v:
                        ax.annotate(f"{int(v):,}", (j, 100 * v / n), xytext=(0, 2), textcoords="offset points",
                                    ha="center", fontsize=7, color=INK)
                ax.set_xticks(range(len(bcols)), lab)
                ax.set_xlabel("Update delay (µs)")
                ax.set_ylabel("Updates (%)")
                ax.set_title(f"EDF priority-update delay, set {set_label(tset)}, U = {tu:g}% "
                             f"(n = {int(n):,}, all trials)", loc="left")
                ax.grid(axis="x", visible=False)
                fig.tight_layout()
                fig.savefig(os.path.join(out, "figT3_update_delay.png"))
                plt.close(fig)

    print(f"\n=== Experiment 1 (task sets): {len(runs)} runs, {len(sets)} task sets, "
          f"tables at set {tset} U={tu:g}%")
    print(summ[["set", "sched", "last_U_all_deadlines_met", "first_U_with_miss",
                "first_tasks_to_miss", "motor_miss_pct_at_110_mean", "trials"]].to_string(index=False))


# ====================================================== experiment 2 (paper tables)
def per_run_exp2(df):
    out = []
    for (n, sch, u, tr), d in df.groupby(["n_tasks", "sched", "U", "trial"]):
        jobs = d.jobs.sum()
        starts = d.starts.sum() if "starts" in d else jobs
        w_start = d.starts if "starts" in d else d.jobs
        mean_resp = (d.mean_us * d.jobs).sum() / jobs if jobs else np.nan
        ex2 = ((d.sd_us ** 2 + d.mean_us ** 2) * d.jobs).sum() / jobs if jobs else np.nan
        sd_resp = np.sqrt(max(ex2 - mean_resp ** 2, 0.0)) if jobs else np.nan
        mean_start = (d.start_mean_us * w_start).sum() / starts if starts else np.nan
        due, miss = d.due.sum(), d.total_miss.sum()
        motor = d[d.task == "motor"]
        out.append(dict(n_tasks=int(n), sched=sch, U=u, trial=tr,
                        mean_start_ms=mean_start / 1000.0, mean_resp_ms=mean_resp / 1000.0,
                        sd_resp_ms=sd_resp / 1000.0, max_resp_ms=d.max_us.max() / 1000.0,
                        adherence_pct=100.0 * (due - miss) / due if due else np.nan,
                        motor_miss_pct=float(motor.miss_pct.iloc[0]) if not motor.empty else np.nan,
                        cpu_pct=d.cpu_pct.iloc[0]))
    pr = pd.DataFrame(out)
    g = pr.groupby(["n_tasks", "sched", "U"])
    agg = g.agg(**{c: (c, "mean") for c in ("mean_start_ms", "mean_resp_ms", "sd_resp_ms", "max_resp_ms",
                                            "adherence_pct", "motor_miss_pct", "cpu_pct")},
                adherence_sd=("adherence_pct", "std"), cpu_sd=("cpu_pct", "std"),
                adh_lo=("adherence_pct", "min"), adh_hi=("adherence_pct", "max"),
                trials=("trial", "nunique")).reset_index()
    return pr, agg.fillna({"adherence_sd": 0.0, "cpu_sd": 0.0})


def experiment2(frames, out):
    os.makedirs(out, exist_ok=True)
    df = frames["CSV"][frames["CSV"].exp == 2]
    od = frames["OVH"][frames["OVH"].exp == 2] if not frames["OVH"].empty else frames["OVH"]
    pr, agg = per_run_exp2(df)
    ns, us = sorted(agg.n_tasks.unique()), sorted(agg.U.unique())
    summ = first_miss_summary(df, "n_tasks")
    df.to_csv(os.path.join(out, "results.csv"), index=False)
    pr.to_csv(os.path.join(out, "per_trial.csv"), index=False)
    agg.to_csv(os.path.join(out, "per_point.csv"), index=False)
    summ.to_csv(os.path.join(out, "summary.csv"), index=False)

    t1 = agg.pivot_table(index=["U", "n_tasks"], columns="sched",
                         values=["mean_start_ms", "mean_resp_ms", "sd_resp_ms"]).round(3)
    t1.columns = [f"{s}_{v}" for v, s in t1.columns]
    t1.reset_index().to_csv(os.path.join(out, "table1_response.csv"), index=False)
    for v, name in (("adherence_pct", "table2_adherence.csv"), ("cpu_pct", "table3_cpu.csv")):
        t = agg.pivot_table(index=["U", "n_tasks"], columns="sched", values=v).round(2).reset_index()
        t.columns.name = None
        t.to_csv(os.path.join(out, name), index=False)

    with open(os.path.join(out, "paper_tables.md"), "w", encoding="utf-8") as f:
        f.write("# Task-count experiment — tables in the format of Zhang & Li (AICCONF 2025)\n\n"
                "Start latency = release → first instruction of the job (the paper's \"response "
                "time\"). Response time = release → completion. Values are means over trials; "
                "± is the SD over trials (Tables II, III) or the pooled SD over all completed "
                "jobs (Table I).\n")
        for u in us:
            p = agg[agg.U == u]
            g = {s: p[p.sched == s].set_index("n_tasks") for s in ("DMS", "EDF")}
            get = lambda s, n: g[s].loc[n] if n in g[s].index else None
            f.write(f"\n## Target utilization U = {u:g}%\n\n**Table I — Task responsiveness (ms)**\n\n"
                    "| Tasks | DMS start latency | EDF start latency | DMS response (mean ± SD) | EDF response (mean ± SD) |\n"
                    "|---:|---:|---:|---:|---:|\n")
            for n in ns:
                a, b = get("DMS", n), get("EDF", n)
                resp = lambda r: "–" if r is None else f"{r.mean_resp_ms:.3f} ± {r.sd_resp_ms:.3f}"
                f.write(f"| {n} | {fmt_ms(a.mean_start_ms) if a is not None else '–'} | "
                        f"{fmt_ms(b.mean_start_ms) if b is not None else '–'} | {resp(a)} | {resp(b)} |\n")
            f.write("\n**Table II — Deadline adherence (%)**   ·   **Table III — CPU utilization (%)**\n\n"
                    "| Tasks | DMS adherence | EDF adherence | DMS CPU | EDF CPU | trials |\n"
                    "|---:|---:|---:|---:|---:|---:|\n")
            for n in ns:
                a, b = get("DMS", n), get("EDF", n)
                pm = lambda r, k, s: "–" if r is None else f"{r[k]:.1f} ± {r[s]:.1f}"
                tr = int(max(a.trials if a is not None else 0, b.trials if b is not None else 0))
                f.write(f"| {n} | {pm(a,'adherence_pct','adherence_sd')} | {pm(b,'adherence_pct','adherence_sd')} | "
                        f"{pm(a,'cpu_pct','cpu_sd')} | {pm(b,'cpu_pct','cpu_sd')} | {tr} |\n")

    utitle = lambda u: f"U = {u:g}%"
    common = dict(xlabel="Number of tasks", title_fn=utitle, xticks=ns, ncols=4)
    panels(agg, "U", us, x="n_tasks", y="adherence_pct", err=("adh_lo", "adh_hi"),
           ylabel="Deadline adherence (%)", title="Deadline adherence vs number of tasks (bars = min–max over trials)",
           path=os.path.join(out, "figN1_adherence.png"), ylim=(-3, 103), **common)
    panels(agg, "U", us, x="n_tasks", y="mean_start_ms", ylabel="Mean start latency (ms)",
           title="Mean start latency (release → start) vs number of tasks",
           path=os.path.join(out, "figN2_start_latency.png"), logy=True, **common)
    panels(agg, "U", us, x="n_tasks", y="mean_resp_ms", ylabel="Mean response time (ms)",
           title="Mean response time (release → completion) vs number of tasks",
           path=os.path.join(out, "figN3_response.png"), logy=True, **common)
    if agg.cpu_pct.notna().any():
        panels(agg, "U", us, x="n_tasks", y="cpu_pct", ylabel="Measured CPU utilization (%)",
               title="Measured CPU utilization vs number of tasks (dotted: target U)",
               path=os.path.join(out, "figN4_cpu.png"), ref_fn=lambda u: u, **common)
    one = dict(xlabel="Number of tasks", title_fn=lambda _: "", xticks=ns, ncols=1)
    panels(summ.rename(columns={"first_U_with_miss": "fm"}), None, [None], x="n_tasks", y="fm",
           ylabel="Utilization at first miss (%)",
           title="Utilization at which each scheduler first misses a deadline (any trial)",
           path=os.path.join(out, "figN5_first_miss.png"), **one)
    if not od.empty:
        e = edf_overhead(od, df.window_ms.iloc[0] / 1000.0)
        e.to_csv(os.path.join(out, "overhead.csv"), index=False)
        eo = e.groupby("n_tasks").agg(edf_overhead_pct=("edf_overhead_pct", "mean")).reset_index().assign(sched="EDF")
        panels(eo, None, [None], x="n_tasks", y="edf_overhead_pct", ylabel="EDF overhead (% CPU)",
               title="EDF scheduling overhead vs number of tasks (mean over U levels and trials)",
               path=os.path.join(out, "figN6_edf_overhead.png"), scheds=("EDF",),
               ylim=(0, max(3.0, eo.edf_overhead_pct.max() * 1.2)), **one)

    print(f"\n=== Experiment 2 (task count): {len(pr)} runs, N = {', '.join(map(str, ns))}")
    print(summ[["n_tasks", "sched", "last_U_all_deadlines_met", "first_U_with_miss",
                "motor_miss_pct_at_110_mean", "trials"]].to_string(index=False))


# ====================================================== experiment 3 (Fig. 4)
def experiment3(frames, out):
    os.makedirs(out, exist_ok=True)
    df = frames["CSV"][frames["CSV"].exp == 3]
    meta = frames["META"][frames["META"].exp == 3] if not frames["META"].empty else frames["META"]
    runs = (df.groupby(["sched", "U", "obs", "trial"])
              .agg(total_miss=("total_miss", "sum"), due=("due", "sum"), cpu_pct=("cpu_pct", "first"))
              .reset_index())
    runs["miss_pct"] = 100 * runs.total_miss / runs.due
    motor = df[df.task == "motor"][["sched", "U", "obs", "trial", "max_us", "miss_pct"]].rename(
        columns={"max_us": "motor_max_us", "miss_pct": "motor_miss_pct"})
    runs = runs.merge(motor, on=["sched", "U", "obs", "trial"], how="left")
    if not meta.empty and "log_bytes" in meta:
        runs = runs.merge(meta[["sched", "U", "obs", "trial", "log_bytes", "log_records"]],
                          on=["sched", "U", "obs", "trial"], how="left")
    agg = runs.groupby(["sched", "U", "obs"]).agg(
        miss_pct=("miss_pct", "mean"), miss_lo=("miss_pct", "min"), miss_hi=("miss_pct", "max"),
        motor_max_ms=("motor_max_us", lambda s: s.mean() / 1000), motor_lo=("motor_max_us", lambda s: s.min() / 1000),
        motor_hi=("motor_max_us", lambda s: s.max() / 1000), cpu_pct=("cpu_pct", "mean"),
        log_bytes=("log_bytes", "mean") if "log_bytes" in runs else ("cpu_pct", "size"),
        trials=("trial", "nunique")).reset_index()
    agg.to_csv(os.path.join(out, "observer_effect.csv"), index=False)

    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.6))
    for ax, (y, lo, hi, lab) in zip(axes, (("miss_pct", "miss_lo", "miss_hi", "Deadline misses (% of releases)"),
                                           ("motor_max_ms", "motor_lo", "motor_hi", "Motor max response (ms)"))):
        for sch in ("DMS", "EDF"):
            for obs, ls, mk, tag in ((0, "-", "o", "deferred"), (1, ":", "^", "intrusive")):
                e = agg[(agg.sched == sch) & (agg.obs == obs)].sort_values("U")
                if e.empty:
                    continue
                ax.errorbar(e.U, e[y], yerr=[(e[y] - e[lo]).clip(lower=0), (e[hi] - e[y]).clip(lower=0)],
                            color=STYLE[sch]["color"], linestyle=ls, marker=mk, capsize=2, elinewidth=1,
                            label=f"{sch}, {tag} logging")
        if y == "motor_max_ms":
            ax.axhline(8, color=INK2, linestyle=":", linewidth=0.9)
            ax.annotate("motor deadline 8 ms", (agg.U.min(), 8), xytext=(2, 3), textcoords="offset points",
                        fontsize=7, color=INK2)
        ax.set_xlabel("Target utilization (%)")
        ax.set_ylabel(lab)
        ax.set_xticks(sorted(agg.U.unique()))
    axes[0].legend(fontsize=7, loc="upper left")
    fig.suptitle("Observer effect: deferred reporting vs intrusive periodic logging (set A, matched workloads)",
                 x=0.01, ha="left", fontweight="bold", color=INK, fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(os.path.join(out, "figT4_observer_effect.png"))
    plt.close(fig)

    with open(os.path.join(out, "observer_effect.md"), "w", encoding="utf-8") as f:
        f.write("**Observer effect — set A, deferred vs intrusive logging**\n\n"
                "Deferred: the reporter (priority 55) sleeps through the whole window and prints "
                "only after suspending the scheduler; nothing is transmitted during measurement. "
                "Intrusive: the same reporter additionally prints a status line per task every "
                "1 s during the window, over the blocking (polled, no interrupt, no DMA) USART2 at "
                "115200 baud, while the scheduler runs. No buffering, so no records are lost; each "
                "print blocks every lower-priority task for its full transmission time.\n\n"
                "| Scheduler | U (%) | Logging | Misses (% of releases) | Motor max R (ms) | CPU (%) | Bytes logged | Trials |\n"
                "|---|---:|---|---:|---:|---:|---:|---:|\n")
        for r in agg.sort_values(["sched", "U", "obs"]).itertuples():
            f.write(f"| {r.sched} | {r.U:g} | {'intrusive' if r.obs else 'deferred'} | {r.miss_pct:.2f} | "
                    f"{r.motor_max_ms:.3f} | {r.cpu_pct:.2f} | {r.log_bytes:,.0f} | {r.trials} |\n")
    print(f"\n=== Experiment 3 (observer effect): {len(runs)} runs")
    print(agg[["sched", "U", "obs", "miss_pct", "motor_max_ms", "trials"]].round(2).to_string(index=False))


# ====================================================== experiment 4 (repeatability)
def experiment4(frames, out):
    os.makedirs(out, exist_ok=True)
    df = frames["CSV"][frames["CSV"].exp == 4]
    od = frames["OVH"][frames["OVH"].exp == 4] if not frames["OVH"].empty else frames["OVH"]
    window_s = df.window_ms.iloc[0] / 1000.0
    runs = (df.groupby(["n_tasks", "U", "trial", "sched", "rep"])
              .agg(due=("due", "sum"), miss=("total_miss", "sum"), cpu_pct=("cpu_pct", "first"),
                   max_norm=("norm_resp", "max"))
              .reset_index())
    runs["adherence_pct"] = 100 * (runs.due - runs.miss) / runs.due
    motor = df[df.task == "motor"][["n_tasks", "U", "trial", "sched", "rep", "miss_pct"]].rename(
        columns={"miss_pct": "motor_miss_pct"})
    runs = runs.merge(motor, on=["n_tasks", "U", "trial", "sched", "rep"], how="left")
    if not od.empty:
        e = edf_overhead(od, window_s)
        if not e.empty:
            runs = runs.merge(e[["n_tasks", "U", "trial", "rep", "edf_overhead_pct"]].assign(sched="EDF"),
                              on=["n_tasks", "U", "trial", "sched", "rep"], how="left")
    runs.to_csv(os.path.join(out, "repeatability_runs.csv"), index=False)

    agg = (runs.groupby(["n_tasks", "U", "trial", "sched"])
               .agg(repeats=("rep", "nunique"),
                    adh_mean=("adherence_pct", "mean"), adh_sd=("adherence_pct", "std"),
                    adh_min=("adherence_pct", "min"), adh_max=("adherence_pct", "max"),
                    collapsed=("adherence_pct", lambda s: int((s < 50).sum())),
                    cpu_mean=("cpu_pct", "mean"), cpu_sd=("cpu_pct", "std"),
                    motor_miss_mean=("motor_miss_pct", "mean"))
               .reset_index().fillna({"adh_sd": 0.0, "cpu_sd": 0.0}))
    if "edf_overhead_pct" in runs:
        ov = runs.groupby(["n_tasks", "U", "trial", "sched"]).edf_overhead_pct.agg(["mean", "std"]).reset_index()
        agg = agg.merge(ov.rename(columns={"mean": "ovh_mean", "std": "ovh_sd"}),
                        on=["n_tasks", "U", "trial", "sched"], how="left")
    agg.to_csv(os.path.join(out, "repeatability.csv"), index=False)

    with open(os.path.join(out, "repeatability.md"), "w", encoding="utf-8") as f:
        f.write("**Repeatability — identical conditions (same generated task set, U, offset seed and "
                f"scheduler), {window_s:g} s window per repeat**\n\n"
                "Adherence = jobs meeting their deadline / jobs due (unfinished jobs count as misses). "
                "Collapsed = repeats with adherence below 50 %.\n\n"
                "| Tasks | U (%) | Seed | Scheduler | Repeats | Adherence mean ± SD (%) | Min–max (%) | Collapsed | CPU mean ± SD (%) | EDF overhead (%) |\n"
                "|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|\n")
        for r in agg.itertuples():
            ov = getattr(r, "ovh_mean", np.nan)
            f.write(f"| {r.n_tasks} | {r.U:g} | {r.trial} | {r.sched} | {r.repeats} | "
                    f"{r.adh_mean:.2f} ± {r.adh_sd:.2f} | {r.adh_min:.2f}–{r.adh_max:.2f} | "
                    f"{r.collapsed}/{r.repeats} | {r.cpu_mean:.2f} ± {r.cpu_sd:.2f} | "
                    f"{'–' if pd.isna(ov) else f'{ov:.2f}'} |\n")

    pts = list(agg[["n_tasks", "U", "trial"]].drop_duplicates().itertuples(index=False, name=None))
    fig, ax = plt.subplots(figsize=(6.8, 3.6))
    for j, (n, u, sd) in enumerate(pts):
        for i, sch in enumerate(("DMS", "EDF")):
            q = runs[(runs.n_tasks == n) & (runs.U == u) & (runs.trial == sd) & (runs.sched == sch)]
            if q.empty:
                continue
            xs = j + (i - 0.5) * 0.36 + np.linspace(-0.08, 0.08, len(q))
            ax.scatter(xs, q.adherence_pct, s=22, color=STYLE[sch]["color"],
                       marker=STYLE[sch]["marker"], label=sch if j == 0 else None, zorder=3)
    ax.set_xticks(range(len(pts)), [f"{n} tasks, U={u:g}%\nseed {sd}" for n, u, sd in pts])
    ax.set_ylim(-5, 105)
    ax.set_ylabel("Deadline adherence (%)")
    ax.set_title("Repeatability: each dot is one identical repeat", loc="left")
    ax.legend(loc="center right")
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "fig_repeatability.png"))
    plt.close(fig)

    print(f"\n=== Experiment 4 (repeatability): {len(runs)} runs")
    cols = ["n_tasks", "U", "trial", "sched", "repeats", "adh_mean", "adh_sd", "adh_min", "adh_max", "collapsed"]
    print(agg[cols].round(2).to_string(index=False))


# ====================================================================== main
def main():
    # Windows consoles default to cp1252; make printing never crash on symbols
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="PuTTY log file(s) from the sweeps")
    ap.add_argument("-o", "--out", default="results", help="output folder")
    ap.add_argument("--table-set", default="A", help="task set for Table III / IV and Figs 1, 3 (default A)")
    ap.add_argument("--table-u", type=float, default=90.0, help="utilization (%%) for Table III / IV (default 90)")
    ap.add_argument("--exclude", help="CSV with columns experiment,run,reason")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    frames = parse_logs(a.logs)
    frames, ex = apply_exclusions(frames, a.exclude)
    coverage(frames, ex, a.out)
    exps = set(frames["CSV"].exp.unique())
    if 1 in exps:
        experiment1(frames, os.path.join(a.out, "exp1_task_sets"), a.table_set, a.table_u)
    if 2 in exps:
        experiment2(frames, os.path.join(a.out, "exp2_task_count"))
    if 3 in exps:
        experiment3(frames, os.path.join(a.out, "exp3_observer"))
    if 4 in exps:
        experiment4(frames, os.path.join(a.out, "exp4_repeatability"))
    print(f"\nWrote tables and figures to {os.path.abspath(a.out)}")


if __name__ == "__main__":
    main()
