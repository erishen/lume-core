#!/usr/bin/env python3
"""Write .vscode/c_cpp_properties.json for the VS Code C/C++ extension.

The compiler needs `llvm-config --cflags`, and the extension reads its include
path from this file instead of from the Makefile — so without it
`src/backend_llvm.h` / `src/llvm_codegen.c` report

    cannot find source file "llvm-c/Core.h" ... update includePath

The path is version-stamped (`/opt/homebrew/opt/llvm/include` is a symlink,
but `llvm-config --includedir` resolves it through a Cellar version directory),
so it is generated rather than committed: run `make vscode-cpp` after a brew
upgrade of llvm, or after cloning on a machine that has LLVM somewhere else.

Usage: scripts/gen_c_cpp_properties.py [llvm-config]
"""

import json
import os
import platform
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, ".vscode", "c_cpp_properties.json")

# The libLLVM C API headers need these to compile as C (the Makefile passes the
# same list via `llvm-config --cflags`); without them the extension reports the
# same "cannot find" error even though the include dir is right.
DEFINES_C = [
    "__STDC_CONSTANT_MACROS",
    "__STDC_FORMAT_MACROS",
    "__STDC_LIMIT_MACROS",
]

# The build sets _DARWIN_C_SOURCE on macOS and _GNU_SOURCE + _FORTIFY_SOURCE=2
# elsewhere; only one of the two can be on at a time.
DEFINES_DARWIN = ["_DARWIN_C_SOURCE"]
DEFINES_LINUX = ["_GNU_SOURCE", "_FORTIFY_SOURCE=2"]


def llvm_includedir(llvm_config):
    if not llvm_config:
        return None
    try:
        out = subprocess.run(
            [llvm_config, "--includedir"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip() or None


def main():
    llvm_config = sys.argv[1] if len(sys.argv) > 1 else None
    if not llvm_config:
        llvm_config = (
            subprocess.run(
                ["command", "-v", "llvm-config"],
                capture_output=True, text=True, shell=True,
            ).stdout.strip()
            or None
        )

    includedir = llvm_includedir(llvm_config)
    defines = DEFINES_C + (DEFINES_DARWIN if platform.system() == "Darwin"
                           else DEFINES_LINUX)
    if includedir:
        defines.append("HAVE_LIBLLVM=1")

    config = {
        "name": os.uname().machine if hasattr(os, "uname") else platform.machine(),
        "includePath": [
            "${workspaceFolder}/src",
            "${workspaceFolder}/**",
        ] + ([includedir] if includedir else []),
        "defines": defines,
        "cStandard": "c11",
        "cppStandard": "gnu++17",
        # Apple clang on macOS, the cc the project actually builds with.
        "compilerPath": "/usr/bin/clang" if platform.system() == "Darwin"
                        else "/usr/bin/cc",
        "intelliSenseMode": "${default}",
    }

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        json.dump({"configurations": [config], "version": 4}, fh, indent=2)
        fh.write("\n")

    print("wrote {}".format(os.path.relpath(OUT, ROOT)))
    print("  includePath: {}".format(", ".join(config["includePath"])))
    print("  defines:     {}".format(", ".join(config["defines"])))


if __name__ == "__main__":
    main()
