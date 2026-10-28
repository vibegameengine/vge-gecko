import glob
import os
import subprocess
import sys

NM = os.path.expanduser("~/.mozbuild/clang/bin/llvm-nm.exe")
OBJDIR = "C:/Projects/gecko-obj-bedrock/vge/bedrock/jsapi"
CONTRACTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "contract")


def defined_symbols():
    objs = glob.glob(os.path.join(OBJDIR, "**", "*.obj"), recursive=True)
    out = subprocess.run([NM, "--defined-only", "-C"] + objs, capture_output=True, text=True, errors="replace").stdout
    symbols = set()
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and len(parts[1]) == 1:
            symbols.add(parts[2].strip())
    return symbols


def main():
    groups = sys.argv[1:] or [os.path.basename(p)[9:-4] for p in glob.glob(os.path.join(CONTRACTS, "contract_*.txt"))]
    defined = defined_symbols()
    total_missing = 0
    for group in groups:
        lines = [l.rstrip("\n").split("\t", 1) for l in open(os.path.join(CONTRACTS, "contract_%s.txt" % group), encoding="utf-8") if l.strip()]
        missing = [(h, s) for h, s in lines if s not in defined]
        total_missing += len(missing)
        print("%s: %d of %d defined, %d missing" % (group, len(lines) - len(missing), len(lines), len(missing)))
        if len(groups) == 1:
            for h, s in missing:
                print("  %s\t%s" % (h, s))
    return 1 if total_missing else 0


if __name__ == "__main__":
    sys.exit(main())
