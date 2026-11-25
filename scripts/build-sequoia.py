#!/usr/bin/env python3
"""Build AirportItlwm-Sequoia with the installed Clang toolchain."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import plistlib
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SDK_REVISION = "3f750085caa17ec3a7880f11c11bf4f48cd6a164"
VERSION = "2.5.0"


def project_sources():
    # Follow the Xcode groups instead of maintaining a second source list.
    # Based on scripts/sequoia/build_common.py in
    # hjgyuhuk/AirportItlwm15-intel-8265 (GPL-2.0).
    project = json.loads(subprocess.check_output([
        "plutil", "-convert", "json", "-o", "-",
        str(ROOT / "itlwm.xcodeproj/project.pbxproj")]))
    objects = project["objects"]
    paths = {}

    def visit(key, parent):
        obj = objects[key]
        path = (ROOT if obj.get("sourceTree") == "SOURCE_ROOT" else parent)
        path = path / obj.get("path", "")
        paths[key] = path
        for child in obj.get("children", []):
            visit(child, path)

    visit(objects[project["rootObject"]]["mainGroup"], ROOT)
    target = next(obj for obj in objects.values()
                  if obj.get("isa") == "PBXNativeTarget"
                  and obj.get("name") == "AirportItlwm-Sequoia")
    sources = []
    for phase_id in target["buildPhases"]:
        phase = objects[phase_id]
        if phase["isa"] != "PBXSourcesBuildPhase":
            continue
        for file_id in phase["files"]:
            entry = objects[file_id]
            if "fileRef" not in entry:
                continue
            if entry.get("settings", {}).get("COMPILER_FLAGS"):
                raise ValueError("Per-file flags need to be included: " + file_id)
            ref = entry["fileRef"]
            if ref in paths:
                sources.append(paths[ref])
            elif objects[ref].get("sourceTree") == "SOURCE_ROOT":
                sources.append(ROOT / objects[ref]["path"])
            else:
                raise ValueError("Source has no project group: " + ref)
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--check-shared", action="store_true",
                        help="Also compile shared IWX, itlwm and legacy Airport scan callers")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    sdk = ROOT / "MacKernelSDK"
    revision = subprocess.check_output(
        ["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if revision != SDK_REVISION:
        raise ValueError("MacKernelSDK must be checked out at " + SDK_REVISION)
    out = ROOT / "build" / ("Sequoia-" + args.configuration)
    objects = out / "objects"
    logs = out / "logs"
    objects.mkdir(parents=True, exist_ok=True)
    logs.mkdir(exist_ok=True)

    def run(command, name):
        with (logs / (name + ".log")).open("w") as log:
            result = subprocess.run(list(map(str, command)), cwd=ROOT,
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError("Failed: " + str(logs / (name + ".log")))

    firmware = objects / "FwBinary.cpp"
    run(["python3", ROOT / "scripts/zlib_compress_fw.py", firmware,
         ROOT / "itlwm/firmware"], "firmware")
    sources = [firmware if p.name == "FwBinary.cpp" else p for p in project_sources()]
    compiler = subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip()
    flags = ["-x", "c++", "-std=gnu++14", "-arch", "x86_64", "-O1",
             "-g" if args.configuration == "Debug" else "-g0",
             "-mmacosx-version-min=15.2", "-mkernel", "-nostdinc++", "-fapple-kext",
             "-fno-exceptions", "-fno-rtti", "-fno-common", "-fno-builtin",
             "-fno-stack-protector", "-mno-red-zone", "-Wno-deprecated-register",
             "-Werror=return-type", "-DKERNEL", "-DKERNEL_PRIVATE", "-D__KERNEL__",
             "-DAIRPORT", "-DAIRPORT_WCL", "-DAIRPORT_VTD", "-D__PRIVATE_SPI__",
             "-DIO80211FAMILY_V2", "-DIEEE80211_STA_ONLY", "-D__IO80211_TARGET=__MAC_15_2",
             '-DITLWM_VERSION="' + VERSION + '"', '-DGIT_COMMIT="_sequoia-9462"',
             "-include", str(ROOT / "itlwm/PrivateSPI.pch")]
    if args.configuration == "Debug":
        flags.append("-DDEBUG=1")
    for directory in ("MacKernelSDK/Headers", "itl80211/openbsd", "itl80211/linux",
                      "itl80211", "include", "include/Airport", "include/HAL",
                      "AirportItlwm", "itlwm/hal_iwm", "itlwm/hal_iwx", "itlwm/hal_iwn"):
        flags.append("-I" + str(ROOT / directory))
    for directory in sorted({p.parent for tree in ("AirportItlwm", "include", "itl80211", "itlwm")
                             for p in (ROOT / tree).rglob("*")
                             if p.suffix in (".h", ".hpp")}):
        flags.extend(["-iquote", str(directory)])
    module = objects / "module.cpp"
    module.write_text('''#include <mach/kmod.h>
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(com.zxystd.AirportItlwm, "VERSION", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nullptr;
__private_extern__ kmod_stop_func_t *_antimain = nullptr;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
'''.replace('"VERSION"', '"' + VERSION + '"'))
    sources.append(module)

    def compile_one(item):
        index, source = item
        obj = objects / (str(index) + ".o")
        run([compiler, *flags, "-c", source, "-o", obj], "compile-" + str(index))
        print(f"[{index + 1}/{len(sources)}] {source.name}", flush=True)
        return obj

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        compiled = list(pool.map(compile_one, enumerate(sources)))
    if args.check_shared:
        common = [flag for flag in flags if flag not in
                  ("-DAIRPORT_WCL", "-DAIRPORT_VTD", "-DIO80211FAMILY_V2",
                   "-DAIRPORT", "-D__IO80211_TARGET=__MAC_15_2")]
        run([compiler, *common, "-c", ROOT / "itlwm/ItlNetworkUserClient.cpp",
             "-o", objects / "userclient.o"], "shared-itlwm-userclient")
        run([compiler, *common, "-c", ROOT / "itlwm/hal_iwx/ItlIwx.cpp",
             "-o", objects / "shared-iwx.o"], "shared-itlwm-iwx")
        for target in ("__MAC_11_0", "__MAC_12_0"):
            run([compiler, *common, "-DAIRPORT", "-D__IO80211_TARGET=" + target,
                 "-c", ROOT / "AirportItlwm/AirportSTAIOCTL.cpp", "-o",
                 objects / (target + "-scan.o")], "shared-airport-" + target)
            run([compiler, *common, "-DAIRPORT", "-D__IO80211_TARGET=" + target,
                 "-c", ROOT / "itlwm/hal_iwx/ItlIwx.cpp", "-o",
                 objects / (target + "-iwx.o")], "shared-iwx-" + target)
    bundle = out / "AirportItlwm_Sequoia.kext"
    binary = bundle / "Contents/MacOS/AirportItlwm"
    binary.parent.mkdir(parents=True, exist_ok=True)
    run(["xcrun", "ld", "-kext", "-arch", "x86_64", "-macos_version_min", "15.2",
         "-o", binary, *compiled, sdk / "Library/x86_64/libkmod.a"], "link")
    info = plistlib.loads((ROOT / "AirportItlwm/WCL/Sequoia-Info.plist").read_bytes())
    info.update(CFBundleDevelopmentRegion="en", CFBundleExecutable="AirportItlwm",
                CFBundleIdentifier="com.zxystd.AirportItlwm", CFBundleName="AirportItlwm",
                CFBundlePackageType="KEXT", CFBundleShortVersionString="2.5.0",
                CFBundleVersion=VERSION)
    (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps(info, sort_keys=False))
    print(bundle)


if __name__ == "__main__":
    main()
