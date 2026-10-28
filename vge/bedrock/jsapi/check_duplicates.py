import collections
import glob
import os
import subprocess
import sys

NM = os.path.expanduser("~/.mozbuild/clang/bin/llvm-nm.exe")
OBJDIR = "C:/Projects/gecko-obj-bedrock/vge/bedrock/jsapi"
STRONG = set("TDBR")


def main():
    owners = collections.defaultdict(list)
    for obj in sorted(glob.glob(os.path.join(OBJDIR, "**", "*.obj"), recursive=True)):
        out = subprocess.run([NM, "--defined-only", obj], capture_output=True, text=True, errors="replace").stdout
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1] in STRONG and not parts[2].startswith(("??_C@", "__real@", "__xmm@", "??_7", "??_R", "$")):
                owners[parts[2]].append(os.path.relpath(obj, OBJDIR))
    dups = {s: o for s, o in owners.items() if len(set(o)) > 1}
    for symbol, objs in sorted(dups.items()):
        demangled = subprocess.run([os.path.join(os.path.dirname(NM), "llvm-undname.exe"), symbol], capture_output=True, text=True, errors="replace").stdout.splitlines()
        print("%s\t%s" % (demangled[1] if len(demangled) > 1 else symbol, " ".join(sorted(set(objs)))))
    print("duplicates: %d" % len(dups))
    return 1 if dups else 0


if __name__ == "__main__":
    sys.exit(main())
