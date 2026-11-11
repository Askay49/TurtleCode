#!/usr/bin/env python3
"""Linux-only, opt-in end-to-end memory regression runner. No third-party modules."""
import argparse
import csv
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

MIB = 1024 * 1024


def process_ids(pid):
    result = [pid]
    for current in result:
        try:
            result.extend(int(x) for x in Path(f"/proc/{current}/task/{current}/children").read_text().split())
        except (OSError, ValueError):
            pass
    return result


def memory(pid):
    try:
        values = {}
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            parts = line.split()
            if parts[0] in ("Rss:", "Pss:"):
                values[parts[0][:-1].lower()] = int(parts[1]) * 1024
        return values if len(values) == 2 else None
    except (OSError, ValueError):
        return None


def disk_bytes(root, pids):
    # Include unlinked backing files still held open; deduplicate file descriptors.
    seen = set()
    total = 0
    paths = list(root.rglob("*"))
    for pid in pids:
        try:
            paths.extend(Path(f"/proc/{pid}/fd").iterdir())
        except OSError:
            pass
    for path in paths:
        try:
            if str(path).startswith("/proc/") and not os.readlink(path).startswith(str(root)):
                continue
            stat = path.stat()
            key = (stat.st_dev, stat.st_ino)
            if key not in seen and not path.is_dir():
                seen.add(key)
                total += stat.st_blocks * 512
        except OSError:
            pass
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--size-mib", type=int, default=10)
    parser.add_argument("--cycles", type=int, default=6)
    parser.add_argument("--max-growth-mib", type=float, default=64,
                        help="Allowed post-close RSS/PSS growth after the first warmup cycle")
    parser.add_argument("--timeout", type=float, default=1200)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    if args.size_mib < 1 or not 3 <= args.cycles <= 100 or args.max_growth_mib < 0 or args.timeout <= 0:
        parser.error("Invalid size, cycles, growth threshold or timeout")
    if memory(os.getpid()) is None:
        parser.error("Readable Linux /proc/PID/smaps_rollup is required")
    binary = args.binary.resolve()
    args.report.mkdir(parents=True, exist_ok=True)
    rows, stages, errors = [], [], []
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="blink-memory-") as directory:
        root = Path(directory)
        scratch = root / "scratch"
        scratch.mkdir()
        fixture = root / "fixture.txt"
        # Bounded generator: no resident copy of the full input in the runner.
        line = b"int value = 123; // memory fixture\n"
        block = (line * (MIB // len(line) + 1))[:MIB]
        with fixture.open("wb") as stream:
            for _ in range(args.size_mib):
                stream.write(block)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", TMPDIR=str(scratch))
        current = {"cycle": 0, "stage": "startup"}
        with (args.report / "stderr.log").open("w") as stderr, (args.report / "samples.csv").open("w", newline="") as csv_stream:
            writer = None
            proc = subprocess.Popen([str(binary), "--memory-workload", str(fixture), str(args.cycles)],
                                    env=env, stdout=subprocess.PIPE, stderr=stderr)
            selector = selectors.DefaultSelector()
            selector.register(proc.stdout, selectors.EVENT_READ)
            pending = b""
            try:
                while proc.poll() is None:
                    if time.monotonic() - started > args.timeout:
                        errors.append("Workload timeout")
                        break
                    for key, _ in selector.select(timeout=0.05):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        pending += chunk
                        while b"\n" in pending:
                            line, pending = pending.split(b"\n", 1)
                            event = json.loads(line)
                            current = event
                            stages.append(dict(event, seconds=round(time.monotonic() - started, 3)))
                            (args.report / "progress.json").write_text(json.dumps(stages[-1]) + "\n")
                    pids = process_ids(proc.pid)
                    editor = memory(proc.pid)
                    if editor is None:
                        continue
                    children = [m for pid in pids[1:] if (m := memory(pid)) is not None]
                    row = dict(seconds=round(time.monotonic()-started, 3), **current,
                               rss=editor["rss"], pss=editor["pss"],
                               children_rss=sum(m["rss"] for m in children),
                               children_pss=sum(m["pss"] for m in children),
                               temporary_disk=disk_bytes(scratch, pids))
                    rows.append(row)
                    if writer is None:
                        writer = csv.DictWriter(csv_stream, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    csv_stream.flush()
                    time.sleep(0.02)
            finally:
                selector.close()
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                proc.stdout.close()
            if proc.returncode:
                errors.append(f"Workload exited with {proc.returncode}; see stderr.log")
    closed = []
    expected = ["opened", "pasted", "deleted", "undo", "redo", "closed"]
    actual = [(s["cycle"], s["stage"]) for s in stages]
    if actual != [(0, "baseline")] + [(c, s) for c in range(1, args.cycles+1) for s in expected]:
        errors.append("Incomplete or out-of-order workload stages")
    for cycle in range(1, args.cycles+1):
        samples = [r for r in rows if r["cycle"] == cycle and r["stage"] == "closed"]
        if not samples:
            errors.append(f"Missing post-close measurement for cycle {cycle}")
        else:
            # Use the last observation after the idle wait, not an operation peak.
            closed.append(samples[-1])
    growth = {}
    if len(closed) == args.cycles:
        for metric in ("rss", "pss"):
            growth[metric] = max(r[metric] for r in closed[1:]) - closed[0][metric]
            if growth[metric] > args.max_growth_mib * MIB:
                errors.append(f"Post-close {metric.upper()} growth {growth[metric]/MIB:.1f} MiB exceeds {args.max_growth_mib:g} MiB")
    summary = dict(size_mib=args.size_mib, cycles=args.cycles, max_growth_mib=args.max_growth_mib,
                   duration_seconds=round(time.monotonic()-started, 3), sample_interval_ms="approximately 50-100",
                   peak_sampled_bytes={k: max((r[k] for r in rows), default=0) for k in
                                       ("rss", "pss", "children_rss", "children_pss", "temporary_disk")},
                   post_close=closed, growth_bytes=growth, stages=stages, errors=errors,
                   passed=not errors, profile="MainWindow; plain text; LSP, Tree-sitter and recovery disabled")
    (args.report / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{'PASS' if not errors else 'FAIL'}: {args.size_mib} MiB, {args.cycles} cycles; "
          f"peak RSS {summary['peak_sampled_bytes']['rss']/MIB:.1f} MiB; "
          f"post-close growth {growth.get('rss', 0)/MIB:.1f} MiB; report: {args.report.resolve()}")
    for error in errors:
        print(error)
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
