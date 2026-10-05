#!/usr/bin/env python3
"""Build CameraToolsReShadeBridge.addon64, the ReShade add-on CameraTools drives ReShade through.

The add-on is one C++ file compiled with LLVM's clang-cl and lld-link against ReShade's API headers, which this script
downloads at the ReShade version the add-on is written for. The Windows SDK and C runtime come from --sdk, a folder made
by xwin, which is how macOS and Linux build it. On Windows, leave out --sdk and run from a Visual Studio developer prompt,
whose INCLUDE and LIB the two tools read.
"""

import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
# ReShade 6.8.0, whose add-on API version 20 the add-on is built and tested against.
RESHADE_COMMIT = "18deaa52de0c425a78b329e9cb3c497281cd00ec"
RESHADE_HEADERS = [
    "reshade.hpp", "reshade_api.hpp", "reshade_api_device.hpp", "reshade_api_format.hpp", "reshade_api_pipeline.hpp",
    "reshade_api_resource.hpp", "reshade_events.hpp", "reshade_overlay.hpp",
]
DEPS = HERE / ".deps"
# Where Homebrew's llvm and lld formulas put the tools, for a PATH that does not have them.
HOMEBREW = {"clang-cl": Path("/opt/homebrew/opt/llvm/bin/clang-cl"), "lld-link": Path("/opt/homebrew/bin/lld-link")}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", type=Path, help="a Windows SDK and CRT made by 'xwin splat' (not needed on Windows)")
    parser.add_argument("--out", type=Path, default=HERE / "bin/CameraToolsReShadeBridge.addon64", help="the add-on to write")
    parser.add_argument("--reshade-include", type=Path, help="ReShade's include folder, instead of downloading it")
    parser.add_argument("--clang-cl", type=Path, help="clang-cl, if it is not on PATH")
    parser.add_argument("--lld-link", type=Path, help="lld-link, if it is not on PATH")
    parser.add_argument("--work", type=Path, default=HERE / "obj", help="where the object file goes")
    args = parser.parse_args()

    clang_cl = tool("clang-cl", args.clang_cl)
    lld_link = tool("lld-link", args.lld_link)
    reshade = args.reshade_include or download_headers()
    includes, libpaths = sdk_paths(args.sdk)

    args.work.mkdir(parents=True, exist_ok=True)
    obj = args.work / "bridge.obj"
    run(clang_cl, "--target=x86_64-pc-windows-msvc", "/c", "/std:c++17", "/MT", "/O2", "/EHsc", "/W4", "/WX",
        "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", *[f"/imsvc{path}" for path in includes], f"/imsvc{reshade}",
        f"/Fo{obj}", "--", HERE / "bridge.cpp")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    run(lld_link, "/dll", "/nologo", "/MANIFEST:NO", "/OPT:REF", "/OPT:ICF", f"/out:{args.out}",
        f"/implib:{args.work / 'bridge.lib'}", *[f"/libpath:{path}" for path in libpaths], obj)
    print(f"{args.out}: ReShade add-on for API version {api_version(reshade)}, {args.out.stat().st_size} bytes")


def tool(name, given):
    found = given or shutil.which(name) or (HOMEBREW[name] if HOMEBREW[name].exists() else None)
    if not found:
        sys.exit(f"{name} not found. Install LLVM (on macOS: brew install llvm lld), put it on PATH, or pass --{name}.")
    return Path(found)


# Only the headers the add-on includes, from the pinned commit, so no ReShade checkout is needed.
def download_headers():
    include = DEPS / f"reshade-{RESHADE_COMMIT[:12]}/include"
    include.mkdir(parents=True, exist_ok=True)
    for name in RESHADE_HEADERS:
        path = include / name
        if path.exists():
            continue
        url = f"https://raw.githubusercontent.com/crosire/reshade/{RESHADE_COMMIT}/include/{name}"
        print(f"Downloading {url}")
        with urllib.request.urlopen(url) as response:
            data = response.read()
        path.with_suffix(".part").write_bytes(data)
        path.with_suffix(".part").replace(path)
    return include


def sdk_paths(sdk):
    if sdk is None:
        if "INCLUDE" not in os.environ or "LIB" not in os.environ:
            sys.exit("No Windows SDK. Pass --sdk with a folder made by 'xwin --accept-license splat --output <folder>', "
                     "or on Windows run from a Visual Studio developer prompt.")
        return [], []
    if not (sdk / "sdk/include/um/Windows.h").exists():
        sys.exit(f"{sdk} is not an xwin SDK folder: it has no sdk/include/um/Windows.h.")
    includes = [sdk / "crt/include", sdk / "sdk/include/ucrt", sdk / "sdk/include/um", sdk / "sdk/include/shared"]
    # xwin names the folders x86_64, or x64 with --preserve-ms-arch-notation.
    arch = "x64" if (sdk / "crt/lib/x64").exists() else "x86_64"
    libpaths = [sdk / f"crt/lib/{arch}", sdk / f"sdk/lib/um/{arch}", sdk / f"sdk/lib/ucrt/{arch}"]
    return includes, libpaths


def api_version(include):
    for line in (include / "reshade.hpp").read_text().splitlines():
        if line.startswith("#define RESHADE_API_VERSION "):
            return line.split()[2]
    sys.exit(f"{include / 'reshade.hpp'} does not define RESHADE_API_VERSION.")


def run(*command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{Path(str(command[0])).name} failed:\n{result.stdout}{result.stderr}")


if __name__ == "__main__":
    main()
