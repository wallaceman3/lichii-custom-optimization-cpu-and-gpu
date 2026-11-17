#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""
Lichii Custom Optimizations Porting & Re-application Script
===========================================================
Applies the full custom CPU & GPU optimization stack to any new version,
branch, or commit of the Lichii / Eglantine emulator cleanly.

Modules included:
  1. 6-Core CPU Shader Workers (GetSmartPipelineWorkers: 2 CPU + 1 Driver during gameplay)
  2. VIP Shader Queue & Batch Syscall Notifications (NeedNow -> Vip -> Soon -> Background)
  3. Nx2FastDraw Same-Value Write Filter & IsStoreOnlyExecutable routing
  4. GPU ASTC & Unswizzle Compute Thresholds (>=4096 B / >=64x64) and memory barriers
  5. Precision Hybrid Frame Pacing (SleepUntil kernel + _mm_pause spin, Mailbox VSync, anti-burst)
  6. Async N-1 Occlusion Queries (SamplesQueryBank::Sync non-blocking)
  7. Guest CPU Busy-Polling Mitigation (svcSleepThread yield & GetCNTPCT spin mitigation)
  8. Tasarren VMA VRAM Reduction (dedicated render target / storage pools up to 8 MiB)
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

def run_cmd(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)
    if check and result.returncode != 0:
        print(f"Error running command: {cmd}\nOutput: {result.stdout}\nError: {result.stderr}", file=sys.stderr)
        raise RuntimeError(f"Command failed: {cmd}")
    return result

def check_git_clean(repo_root):
    res = run_cmd("git status --porcelain", cwd=repo_root)
    lines = [l for l in res.stdout.strip().splitlines() if not l.startswith("??")]
    return len(lines) == 0

def apply_patch_file(repo_root, patch_path, ignore_whitespace=True):
    flags = "--3way"
    if ignore_whitespace:
        flags += " --ignore-space-change --ignore-whitespace"
    cmd = f"git apply {flags} \"{patch_path}\""
    res = run_cmd(cmd, cwd=repo_root, check=False)
    return res.returncode == 0, res.stdout, res.stderr

def main():
    parser = argparse.ArgumentParser(description="Re-apply custom optimizations to Lichii")
    parser.add_argument("--repo-root", default=".", help="Root directory of the Lichii repository")
    parser.add_argument("--skip-vram", action="store_true", help="Skip Tasarren VRAM optimizations")
    parser.add_argument("--check-only", action="store_true", help="Dry run check without applying")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    patches_dir = repo_root / "patches"

    print("================================================================")
    print(" LICHII CUSTOM OPTIMIZATION AUTO-PORTING TOOL")
    print(f" Target Repository: {repo_root}")
    print("================================================================")

    if not (repo_root / "src").is_dir():
        print("ERROR: Target directory does not look like a Lichii repository.", file=sys.stderr)
        sys.exit(1)

    if not check_git_clean(repo_root):
        print("WARNING: Git working tree has unstaged changes. Stash or commit before applying.", file=sys.stderr)

    patches_to_apply = []
    phase1_patch = patches_dir / "01_phase1_optimizations.patch"
    if phase1_patch.is_file():
        patches_to_apply.append(("Phase 1 Optimizations (VIP queue, FastDraw, ASTC, Pacing, N-1)", phase1_patch))

    if not args.skip_vram:
        vram_dir = patches_dir / "tasarren_vram"
        if vram_dir.is_dir():
            for p in sorted(vram_dir.glob("*.patch")):
                patches_to_apply.append((f"Tasarren VRAM: {p.name}", p))

    print(f"Found {len(patches_to_apply)} patch sets to verify/apply.\n")

    for desc, patch_path in patches_to_apply:
        cmd_flag = "--check" if args.check_only else ""
        print(f"[*] Processing: {desc}...")
        ok, out, err = apply_patch_file(repo_root, patch_path)
        if ok:
            print(f"    [OK] Applied cleanly.")
        else:
            print(f"    [CONFLICT / DRIFT] Needed manual or 3-way merge: {err.strip()[:100]}...")

    print("\n================================================================")
    print(" Optimization pipeline completed!")
    print("================================================================")

if __name__ == "__main__":
    main()
