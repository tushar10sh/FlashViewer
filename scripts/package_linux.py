#!/usr/bin/env python3
import subprocess
import os
import shutil
import sys

def gather_deps(bin_path, plugins, out_lib):
    os.makedirs(out_lib, exist_ok=True)
    seen = set()
    to_scan = [bin_path] + [p for p in plugins if os.path.exists(p)]
    
    while to_scan:
        current = to_scan.pop(0)
        if not os.path.exists(current):
            continue
        try:
            res = subprocess.check_output(["ldd", current], stderr=subprocess.DEVNULL).decode("utf-8")
        except Exception:
            continue
        for line in res.splitlines():
            parts = line.strip().split(" => ")
            if len(parts) == 2:
                target = parts[1].split(" ")[0].strip()
                if os.path.exists(target):
                    fn = os.path.basename(target)
                    # Exclude low-level glibc and libgcc system libraries provided by host OS
                    excluded_prefixes = ("ld-linux", "libc.", "libm.", "libdl.", "libpthread.", "libresolv.", "librt.", "libgcc_s.")
                    if any(fn.startswith(p) for p in excluded_prefixes):
                        continue
                    if fn not in seen:
                        seen.add(fn)
                        dest = os.path.join(out_lib, fn)
                        try:
                            shutil.copy(target, dest)
                            to_scan.append(dest)
                        except Exception as e:
                            print(f"Warning: could not copy {target}: {e}")

if __name__ == "__main__":
    bundle_dir = sys.argv[1] if len(sys.argv) > 1 else "/dist/FlashViewer-bundle"
    bin_file = os.path.join(bundle_dir, "bin", "FlashViewer")
    plugins = [
        os.path.join(bundle_dir, "plugins", "platforms", "libqxcb.so"),
        os.path.join(bundle_dir, "plugins", "platforms", "libqoffscreen.so"),
    ]
    lib_dir = os.path.join(bundle_dir, "lib")
    gather_deps(bin_file, plugins, lib_dir)
    print(f"Successfully gathered dependencies into {lib_dir}")
