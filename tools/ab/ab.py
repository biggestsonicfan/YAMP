#!/usr/bin/env python3
"""A/B performance harness for YAMP running Sonic the Fighters.

Builds two versions of YAMP (any git refs, or the working tree), runs each one several times
against the same StF module with the "-bench" recorder (source/Bench.h), interleaved A,B,A,B so
thermal and background drift hit both sides alike, and prints the per-metric medians side by side.

It also checks the SIMULATION, not just the clock: every run reports a hash of emulated work RAM
on its last frame, taken from a deterministic starting point (fixed RNG seed, pinned texture
budget, frames counted from the ROM's first frame - see LJHost's Run). All runs of both builds
must agree on it. An optimisation that changes the hash changed what the game does, and this
script says so and names the 64 KB chunks of work RAM that differ.

  python tools/ab/ab.py --a origin/master --b WORKTREE
  python tools/ab/ab.py --a HEAD~3 --b HEAD --reps 7 --frames 2400
  python tools/ab/ab.py --a HEAD --b WORKTREE --fpscap 0      # uncapped: limiter out of the picture

Requires: the module folder (the directory holding stf-pxd-w64-d3d12_retail.dll and rom/) as the
run CWD - by default build/bin/Win64/Debug/m2ftg, override with --game-dir. Runs pass -noownership
(the ownership check starts a Steam helper process, which is noise here); the module checksum is
still enforced, so both sides provably run the same DLL.

Both refs must contain the -bench recorder, i.e. be at or after the commit that added this script.
"""

import argparse
import math
import os
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MSBUILD = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe")
PREMAKE = REPO / "premake5.exe"
PROFILE = Path(__file__).resolve().parent / "settings.ini"

# (key, label, lower_is_better). Only these are tabulated; the full report is kept per run.
METRICS = [
    ("fps", "fps", False),
    ("cpu_mainthread_pct", "main thread CPU %", True),
    ("cpu_process_pct", "process CPU %", True),
    ("frame_ms_avg", "frame ms avg", True),
    ("frame_ms_p99", "frame ms p99", True),
    ("frame_ms_max", "frame ms max", True),
    ("work_ms_avg", "work ms avg", True),
    ("work_ms_p95", "work ms p95", True),
    ("work_ms_p99", "work ms p99", True),
    ("module_ms_avg", "module_main ms avg", True),
    ("host_ms_avg", "host ms avg", True),
    ("host_ms_p99", "host ms p99", True),
    ("submit_ms_avg", "submit ms avg", True),
    ("present_ms_avg", "present ms avg", True),
    ("boot_s", "boot s", True),
    ("anchor_s", "to ROM start s", True),
    ("peak_working_set_mb", "peak working set MB", True),
    ("peak_private_mb", "peak private MB", True),
    ("handles", "handles", True),
]


def run(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def git(*args, cwd=REPO):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def msbuild(vcxproj, config):
    run([str(MSBUILD), str(vcxproj), f"-p:Configuration={config} Win64", "-p:Platform=x64",
         "-m", "-v:m", "-clp:ErrorsOnly;Summary"])


def build(ref, config, cache):
    """Returns the path of a built YAMP.exe for `ref` ("WORKTREE" = the current checkout)."""
    if ref == "WORKTREE":
        print(f"[build] working tree ({config})")
        run([str(PREMAKE), "vs2022"], cwd=REPO, stdout=subprocess.DEVNULL)
        msbuild(REPO / "build" / "YAMP.vcxproj", config)
        return REPO / "build" / "bin" / "Win64" / config / "YAMP.exe"

    sha = git("rev-parse", "--verify", f"{ref}^{{commit}}")
    wt = cache / sha[:12]
    exe = wt / "build" / "bin" / "Win64" / config / "YAMP.exe"
    if exe.exists():
        print(f"[build] {ref} = {sha[:12]} (cached)")
        return exe
    print(f"[build] {ref} = {sha[:12]} -> {wt}")
    if not (wt / ".git").exists():
        cache.mkdir(parents=True, exist_ok=True)
        run(["git", "worktree", "add", "--detach", str(wt), sha], cwd=REPO)
        # Submodules from the main checkout's own clones: no network, and the same objects.
        for name in git("config", "--file", ".gitmodules", "--name-only", "--get-regexp", r"\.path$").splitlines():
            sub = name[len("submodule."):-len(".path")]
            local = REPO / ".git" / "modules" / sub
            if local.exists():
                git("config", f"submodule.{sub}.url", str(local), cwd=wt)
        run(["git", "-c", "protocol.file.allow=always", "submodule", "update", "--init"], cwd=wt)
    run([str(PREMAKE), f"--file={wt / 'premake5.lua'}", "vs2022"], cwd=wt, stdout=subprocess.DEVNULL)
    msbuild(wt / "build" / "YAMP.vcxproj", config)
    return exe


def parse_report(path):
    out = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


def ram_diff_chunks(a, b):
    da, db = a.read_bytes(), b.read_bytes()
    chunks = {}
    for i in range(0, min(len(da), len(db)), 4):
        if da[i:i + 4] != db[i:i + 4]:
            c = 0x500000 + (i & ~0xFFFF)
            chunks[c] = chunks.get(c, 0) + 1
    return chunks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", default="HEAD", help="baseline: git ref or WORKTREE (default HEAD)")
    ap.add_argument("--b", default="WORKTREE", help="candidate: git ref or WORKTREE (default WORKTREE)")
    ap.add_argument("--config", default="Release", choices=["Debug", "Release", "Master"])
    ap.add_argument("--frames", type=int, default=1500, help="frames per run, counted from the ROM's first frame")
    ap.add_argument("--warmup", type=int, default=120, help="frames after the ROM starts left out of the stats")
    ap.add_argument("--reps", type=int, default=5, help="runs per side")
    ap.add_argument("--fpscap", type=int, default=1, choices=[0, 1], help="YAMP's 60 Hz limiter (settings FPSCap)")
    ap.add_argument("--game-dir", default=str(REPO / "build" / "bin" / "Win64" / "Debug" / "m2ftg"),
                    help="the folder holding the StF module DLL and rom/ (used as the run CWD)")
    ap.add_argument("--out", default=None, help="results folder (default: %%TEMP%%/yamp-ab/results/<a>_vs_<b>)")
    ap.add_argument("--cache", default=os.path.join(os.environ.get("TEMP", "."), "yamp-ab", "trees"),
                    help="where git refs are checked out and built")
    ap.add_argument("--timeout", type=int, default=0, help="per-run timeout in seconds (default: from --frames)")
    args = ap.parse_args()

    game_dir = Path(args.game_dir)
    if not (game_dir / "stf-pxd-w64-d3d12_retail.dll").exists():
        sys.exit(f"no stf-pxd-w64-d3d12_retail.dll in {game_dir} (pass --game-dir)")

    def slug(ref):
        return "".join(c if c.isalnum() else "-" for c in ref)
    out = Path(args.out) if args.out else Path(os.environ.get("TEMP", ".")) / "yamp-ab" / "results" / f"{slug(args.a)}_vs_{slug(args.b)}"
    out.mkdir(parents=True, exist_ok=True)

    # Build both first (B last when it is the working tree, so A's build cannot clobber it), then
    # copy each exe into its own folder: settings.ini is read from beside YAMP.exe.
    sides = {}
    for label, ref in (("A", args.a), ("B", args.b)):
        exe = build(ref, args.config, Path(args.cache))
        d = out / label
        d.mkdir(exist_ok=True)
        shutil.copy2(exe, d / "YAMP.exe")
        sides[label] = (ref, d)

    profile = PROFILE.read_text().replace("FPSCap=1", f"FPSCap={args.fpscap}")
    timeout = args.timeout or max(60, int(args.frames / 30) + 60)
    results = {"A": [], "B": []}
    for rep in range(args.reps):
        for label in ("A", "B"):
            ref, d = sides[label]
            (d / "settings.ini").write_text(profile)
            report = d / f"run{rep}.txt"
            for stale in d.glob(f"run{rep}.txt*"):
                stale.unlink()
            cmd = [str(d / "YAMP.exe"), "-stf", "-noownership", "-frames", str(args.frames),
                   "-bench", str(report), "-bench-warmup", str(args.warmup)]
            print(f"[run] {label} rep {rep + 1}/{args.reps}", flush=True)
            try:
                subprocess.run(cmd, cwd=game_dir, timeout=timeout, check=False)
            except subprocess.TimeoutExpired:
                print(f"  TIMEOUT after {timeout}s")
            if not report.exists():
                print("  no report written (crash, timeout, or the ROM never started)")
                continue
            results[label].append((report, parse_report(report)))

    # ---- report ---------------------------------------------------------------------------
    print()
    print(f"A = {args.a}   B = {args.b}   ({args.config}, {args.frames} frames, warmup {args.warmup}, "
          f"FPSCap={args.fpscap}, {len(results['A'])}+{len(results['B'])} runs)")
    print()
    print(f"{'metric':<22}{'A median':>12}{'A range':>20}{'B median':>12}{'B range':>20}{'delta':>9}")
    for key, name, lower in METRICS:
        va = [float(r[key]) for _, r in results["A"] if key in r]
        vb = [float(r[key]) for _, r in results["B"] if key in r]
        if not va or not vb:
            continue
        ma, mb = statistics.median(va), statistics.median(vb)
        delta = (mb - ma) / ma * 100 if ma else 0.0
        # Only call it a change when the two sides' ranges do not overlap at all.
        separated = max(va) < min(vb) or max(vb) < min(va)
        better = (mb < ma) == lower
        tag = ("  better" if better else "  WORSE") if separated else ""
        print(f"{name:<22}{ma:>12.3f}{f'[{min(va):.3f}..{max(va):.3f}]':>20}{mb:>12.3f}"
              f"{f'[{min(vb):.3f}..{max(vb):.3f}]':>20}{delta:>+8.1f}%{tag}")

    na, nb = len(results["A"]), len(results["B"])
    if na and nb:
        # Non-overlapping ranges is the most extreme Mann-Whitney outcome; this is its p-value.
        p = 2 / math.comb(na + nb, na)
        print(f"\n'better'/'WORSE' = every run of one side beat every run of the other "
              f"(two-sided p = {p:.3f} at {na}+{nb} runs; an A/A run at 3+3 flagged sub-ms metrics by ~10%).")

    print()
    hashes = {label: sorted({r.get("state_hash") for _, r in results[label]}) for label in results}
    frames = {label: sorted({r.get("rom_frame") for _, r in results[label]}) for label in results}
    print(f"state_hash  A: {hashes['A']}  B: {hashes['B']}")
    print(f"rom_frame   A: {frames['A']}  B: {frames['B']}")
    all_runs = results["A"] + results["B"]
    if len({r.get("state_hash") for _, r in all_runs}) == 1 and all_runs:
        print("SIMULATION IDENTICAL across every run of both builds.")
        return 0
    print("SIMULATION DIFFERS - the builds (or runs) did not compute the same game state.")
    ref_report = all_runs[0][0]
    for report, r in all_runs[1:]:
        if r.get("state_hash") != all_runs[0][1].get("state_hash"):
            ra, rb = Path(str(ref_report) + ".ram"), Path(str(report) + ".ram")
            if ra.exists() and rb.exists():
                chunks = ram_diff_chunks(ra, rb)
                desc = ", ".join(f"0x{c:06X}:{n}" for c, n in sorted(chunks.items()))
                print(f"  {ref_report.parent.name}/{ref_report.name} vs {report.parent.name}/{report.name}: "
                      f"differing dwords per 64 KB chunk: {desc}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
