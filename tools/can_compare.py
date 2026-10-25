#!/usr/bin/env python3
"""
Compare CAN recordings from the CYD's SD card to find which IDs and bytes carry a value.

The CYD's sniffer page records every frame to CANnnnn.LOG (candump format) and each MARK
tap to CANnnnn.TXT. This script finds what changed between two recordings, or between
the seconds before and after a MARK, e.g. which byte switches when the transfer case
goes from 2H to 4H.

Usage:
    python can_compare.py CAN0001.LOG                    summary: every ID, what each byte does
    python can_compare.py CAN0001.LOG CAN0002.LOG        compare two recordings (A -> B)
    python can_compare.py CAN0001.LOG --mark 3           compare 5 s before / after MARK 3
    python can_compare.py CAN0001.LOG --id 4B0           every change of one ID's data

Options:
    --window 5        seconds before / after the mark to compare (default 5)
    --gap 0.5         seconds right around the mark to skip (default 0.5)
    --from 10 --to 60 only use frames in this time range (seconds, as in the log)
    --top 40          how many findings to show (default 40)
    --all             also list IDs with no differences

Only reads files; needs Python 3.6 or newer and nothing else.
"""

import argparse
import os
import re
import sys
from collections import defaultdict

LINE_RE = re.compile(r"^\((\d+(?:\.\d+)?)\)\s+\S+\s+([0-9A-Fa-f]{1,8})#(R|[0-9A-Fa-f]*)")
MARK_RE = re.compile(r"^\((\d+(?:\.\d+)?)\)\s+MARK\s+(\d+)")


# ---- Reading files -----------------------------------------------------------------

def read_log(path):
    """Return a list of (time, id, data bytes) from a candump log."""
    frames = []
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line.strip())
            if not m:
                continue
            t, can_id, data = m.groups()
            data = b"" if data == "R" else bytes.fromhex(data[: len(data) // 2 * 2])
            frames.append((float(t), can_id.upper().zfill(3), data))
    return frames


def read_marks(log_path):
    """Return {mark number: time} from the .TXT notes file next to the log."""
    base = os.path.splitext(log_path)[0]
    for ext in (".TXT", ".txt"):
        if os.path.exists(base + ext):
            marks = {}
            with open(base + ext, encoding="ascii", errors="replace") as f:
                for line in f:
                    m = MARK_RE.match(line.strip())
                    if m:
                        marks[int(m.group(2))] = float(m.group(1))
            return marks
    return {}


def time_slice(frames, start=None, end=None):
    return [fr for fr in frames
            if (start is None or fr[0] >= start) and (end is None or fr[0] <= end)]


# ---- Describing one recording ------------------------------------------------------

class IdStats:
    """Everything seen for one CAN ID: frame times and the values at each byte position."""

    def __init__(self):
        self.times = []
        self.dlcs = set()
        self.values = defaultdict(list)   # byte index -> list of values in order

    def add(self, t, data):
        self.times.append(t)
        self.dlcs.add(len(data))
        for i, b in enumerate(data):
            self.values[i].append(b)

    @property
    def count(self):
        return len(self.times)

    def rate(self):
        span = self.times[-1] - self.times[0] if self.count > 1 else 0
        return (self.count - 1) / span if span > 0 else 0.0


def collect(frames):
    stats = defaultdict(IdStats)
    for t, can_id, data in frames:
        stats[can_id].add(t, data)
    return stats


def is_counter(vals):
    """A rolling counter steps by the same amount (mod 256, or mod 16 in a nibble) most of the time."""
    if len(set(vals)) < 4 or len(vals) < 8:
        return False
    steps = defaultdict(int)
    for a, b in zip(vals, vals[1:]):
        steps[(b - a) % 256] += 1
    step, n = max(steps.items(), key=lambda kv: kv[1])
    return step != 0 and n >= 0.8 * (len(vals) - 1)


def describe_byte(vals):
    unique = sorted(set(vals))
    if len(unique) == 1:
        return "constant %02X" % unique[0]
    if is_counter(vals):
        return "counter"
    changes = sum(1 for a, b in zip(vals, vals[1:]) if a != b)
    if len(unique) <= 4:
        return "flag/state: " + " ".join("%02X" % v for v in unique) + " (%d changes)" % changes
    return "varies %02X-%02X (%d values, %d changes)" % (unique[0], unique[-1], len(unique), changes)


def id_sort_key(can_id):
    return (len(can_id), int(can_id, 16))


def print_summary(frames, title):
    stats = collect(frames)
    if not frames:
        print("No frames.")
        return
    print("%s: %d frames, %d IDs, %.1f-%.1f s" % (title, len(frames), len(stats),
                                                   frames[0][0], frames[-1][0]))
    for can_id in sorted(stats, key=id_sort_key):
        s = stats[can_id]
        dlc = "/".join(str(d) for d in sorted(s.dlcs))
        print("\n%s  [%s]  %d frames, %.1f/s" % (can_id, dlc, s.count, s.rate()))
        for i in sorted(s.values):
            print("    byte %d: %s" % (i, describe_byte(s.values[i])))


# ---- Comparing two sets of frames --------------------------------------------------

def median(vals):
    v = sorted(vals)
    return v[len(v) // 2]


def fmt_values(vals, limit=6):
    unique = sorted(set(vals))
    if len(unique) == 1:
        return "%02X" % unique[0]
    if len(unique) <= limit:
        return "{" + " ".join("%02X" % v for v in unique) + "}"
    return "%02X-%02X" % (unique[0], unique[-1])


def compare(a_frames, b_frames):
    """Return findings as (score, id, text), higher score = stronger candidate."""
    a, b = collect(a_frames), collect(b_frames)
    findings = []

    for can_id in set(a) | set(b):
        if can_id not in b:
            findings.append((60, can_id, "only in A (%d frames, %.1f/s): stops being sent"
                             % (a[can_id].count, a[can_id].rate())))
            continue
        if can_id not in a:
            findings.append((60, can_id, "only in B (%d frames, %.1f/s): starts being sent"
                             % (b[can_id].count, b[can_id].rate())))
            continue

        sa, sb = a[can_id], b[can_id]
        ra, rb = sa.rate(), sb.rate()
        if ra > 0 and rb > 0 and max(ra, rb) / min(ra, rb) > 2:
            findings.append((20, can_id, "frame rate %.1f/s -> %.1f/s" % (ra, rb)))
        if sa.dlcs != sb.dlcs:
            findings.append((40, can_id, "length %s -> %s" % (sorted(sa.dlcs), sorted(sb.dlcs))))

        for i in sorted(set(sa.values) & set(sb.values)):
            va, vb = sa.values[i], sb.values[i]
            if is_counter(va) or is_counter(vb):
                continue
            set_a, set_b = set(va), set(vb)
            label = "byte %d" % i

            if len(set_a) == 1 and len(set_b) == 1:
                if set_a != set_b:
                    if abs(va[0] - vb[0]) == 1:   # one step: just as likely a slowly drifting value
                        findings.append((70, can_id, "%s: %02X -> %02X (steady in both, but only one step:"
                                         " may be a slow value like a temperature)" % (label, va[0], vb[0])))
                    else:
                        findings.append((100, can_id, "%s: %02X -> %02X (steady in both)"
                                         % (label, va[0], vb[0])))
                continue

            # Bits that are steady in both but flip between A and B (a flag inside a busy byte)
            flipped = False
            for bit in range(8):
                fa = sum((v >> bit) & 1 for v in va) / len(va)
                fb = sum((v >> bit) & 1 for v in vb) / len(vb)
                if (fa == 0 and fb == 1) or (fa == 1 and fb == 0):
                    flipped = True
                    findings.append((90, can_id, "%s bit %d (mask %02X): %d -> %d (steady in both,"
                                     " rest of the byte changes)" % (label, bit, 1 << bit, int(fa), int(fb))))
            if flipped:
                continue

            if len(set_a) == 1 or len(set_b) == 1:   # e.g. wheel speed 0 while parked, then moving
                findings.append((75, can_id, "%s: %s -> %s (%s)" % (
                    label, fmt_values(va), fmt_values(vb),
                    "starts changing" if len(set_a) == 1 else "stops changing")))
                continue

            if set_a.isdisjoint(set_b):   # both moving, e.g. a speed that went up
                findings.append((50, can_id, "%s: %s -> %s (changing in both, no values in common)"
                                 % (label, fmt_values(va), fmt_values(vb))))
                continue

            ma, mb = median(va), median(vb)
            spread = max(max(va + vb) - min(va + vb), 1)
            if abs(ma - mb) >= max(0.25 * spread, 3):
                findings.append((30, can_id, "%s: typical value %02X -> %02X (range %s -> %s)"
                                 % (label, ma, mb, fmt_values(va), fmt_values(vb))))

    findings.sort(key=lambda f: (-f[0], id_sort_key(f[1]), f[2]))
    return findings, a, b


STRENGTH = {100: "STRONG", 90: "STRONG", 75: "likely", 70: "likely", 60: "likely",
            50: "maybe", 40: "maybe", 30: "maybe", 20: "info"}


def print_comparison(a_frames, b_frames, a_name, b_name, top, show_all):
    if not a_frames or not b_frames:
        print("Not enough frames to compare (A: %d, B: %d)." % (len(a_frames), len(b_frames)))
        return
    findings, a, b = compare(a_frames, b_frames)
    print("A = %s: %d frames, %d IDs" % (a_name, len(a_frames), len(a)))
    print("B = %s: %d frames, %d IDs" % (b_name, len(b_frames), len(b)))
    print()
    if not findings:
        print("No differences found (counters are ignored).")
    else:
        print("Differences, strongest first (counters ignored):")
        print()
        for score, can_id, text in findings[:top]:
            print("  %-7s %-4s %s" % (STRENGTH.get(score, ""), can_id, text))
        if len(findings) > top:
            print("\n  ... %d more (use --top %d to see all)" % (len(findings) - top, len(findings)))
    if show_all:
        changed = {f[1] for f in findings}
        same = sorted((set(a) | set(b)) - changed, key=id_sort_key)
        print("\nNo differences: " + (" ".join(same) if same else "(none)"))
    print("\nSTRONG = a byte or bit that holds one value in A and another in B: the best"
          "\ncandidates for a switch or state (transfer case, brake, gear). Check them with --id.")


# ---- One ID over time ----------------------------------------------------------------

def print_timeline(frames, can_id, marks):
    can_id = can_id.upper().zfill(3)
    rows = [(t, d) for t, i, d in frames if i == can_id]
    if not rows:
        print("ID %s is not in this recording." % can_id)
        return
    print("%s: %d frames; showing each change (^ = byte changed)" % (can_id, len(rows)))
    mark_times = sorted((t, n) for n, t in marks.items())
    last = None
    for t, data in rows:
        while mark_times and mark_times[0][0] <= t:
            mt, n = mark_times.pop(0)
            print("%12.3f  ---- MARK %d ----" % (mt, n))
        if data == last:
            continue
        hexed = " ".join("%02X" % v for v in data)
        print("%12.3f  %s" % (t, hexed))
        if last is not None and len(last) == len(data):
            print("              " + " ".join("^^" if x != y else "  " for x, y in zip(last, data)).rstrip())
        last = data
    for mt, n in mark_times:
        print("%12.3f  ---- MARK %d ----" % (mt, n))


# ---- Main ------------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="Compare CAN recordings (candump .LOG) from the CYD.")
    p.add_argument("log_a", help="recording (CANnnnn.LOG)")
    p.add_argument("log_b", nargs="?", help="second recording to compare against")
    p.add_argument("--mark", type=int, help="compare before / after this MARK number")
    p.add_argument("--window", type=float, default=5.0, help="seconds each side of the mark (default 5)")
    p.add_argument("--gap", type=float, default=0.5, help="seconds right at the mark to skip (default 0.5)")
    p.add_argument("--id", help="print every change of this ID's data")
    p.add_argument("--from", dest="start", type=float, help="ignore frames before this time (s)")
    p.add_argument("--to", dest="end", type=float, help="ignore frames after this time (s)")
    p.add_argument("--top", type=int, default=40, help="findings to show (default 40)")
    p.add_argument("--all", action="store_true", help="also list IDs with no differences")
    args = p.parse_args()

    a = time_slice(read_log(args.log_a), args.start, args.end)
    if not a:
        sys.exit("No candump frames found in %s" % args.log_a)
    marks = read_marks(args.log_a)

    if args.id:
        print_timeline(a, args.id, marks)
    elif args.mark is not None:
        if args.mark not in marks:
            sys.exit("MARK %d not found. Marks in this recording: %s"
                     % (args.mark, ", ".join(str(n) for n in sorted(marks)) or "none"))
        t = marks[args.mark]
        before = time_slice(a, t - args.gap - args.window, t - args.gap)
        after = time_slice(a, t + args.gap, t + args.gap + args.window)
        print("MARK %d at %.3f s: comparing %.1f s before with %.1f s after\n"
              % (args.mark, t, args.window, args.window))
        print_comparison(before, after, "before MARK %d" % args.mark, "after MARK %d" % args.mark,
                         args.top, args.all)
    elif args.log_b:
        b = time_slice(read_log(args.log_b), args.start, args.end)
        print_comparison(a, b, os.path.basename(args.log_a), os.path.basename(args.log_b),
                         args.top, args.all)
    else:
        print_summary(a, os.path.basename(args.log_a))
        if marks:
            print("\nMarks: " + ", ".join("%d at %.1f s" % (n, t) for n, t in sorted(marks.items())))


if __name__ == "__main__":
    main()
