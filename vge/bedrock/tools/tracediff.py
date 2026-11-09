import re
import sys
from collections import Counter


def load(path):
    groups = Counter()
    total = None
    for line in open(path, encoding="utf-8", errors="replace"):
        match = re.match(r"resident ([\d.]+) committed [\d.]+ MB x\d+ (.*)", line)
        if match:
            frames = [f for f in match[2].split(" < ") if "LazyInstanceImpl" not in f]
            groups[" < ".join(f[:60] for f in frames[:5])] += float(match[1])
        match = re.match(r"untracked_private_resident_mb ([\d.]+)", line)
        if match:
            total = float(match[1])
    return groups, total


def main():
    before, before_unknown = load(sys.argv[1])
    after, after_unknown = load(sys.argv[2])
    print("tracked", round(sum(before.values())), round(sum(after.values())))
    print("unknown", before_unknown, after_unknown)
    delta = Counter({k: after[k] - before[k] for k in set(before) | set(after)})
    ranked = sorted(delta.items(), key=lambda item: -abs(item[1]))
    for key, value in ranked[:int(sys.argv[3]) if len(sys.argv) > 3 else 25]:
        print(f"{value:+.1f} {before[key]:.1f} {after[key]:.1f} {key[:280]}")


if __name__ == "__main__":
    main()
