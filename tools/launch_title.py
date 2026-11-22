import argparse
import os
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import run_report_summary

ROOT = Path(__file__).resolve().parent.parent
MACHINE_AMD64 = 0x8664
PE32_PLUS = 0x20B
NOT_COPIED_SUFFIXES = {".xvd"}


class LaunchError(Exception):
    pass


def executable_names(config_text):
    try:
        root = ET.fromstring(config_text)
    except ET.ParseError as error:
        raise LaunchError(f"MicrosoftGame.Config is not valid XML: {error}")
    names = []
    for element in root.iter("Executable"):
        name = element.get("Name")
        if name and element.get("IsDevOnly", "false").lower() != "true":
            names.append(name)
    return names


def pe_problem(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        return "does not start with an MZ header"
    (pe_offset,) = struct.unpack_from("<I", data, 0x3C)
    if pe_offset > len(data) - 0x1A:
        return "PE header offset lies outside the file"
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return "has no PE signature"
    (machine,) = struct.unpack_from("<H", data, pe_offset + 4)
    (magic,) = struct.unpack_from("<H", data, pe_offset + 0x18)
    if machine != MACHINE_AMD64 or magic != PE32_PLUS:
        return f"is not a PE32+ x64 image (machine 0x{machine:04X}, magic 0x{magic:04X})"
    return None


def read_head(path, size=0x10000):
    with open(path, "rb") as handle:
        return handle.read(size)


def module_dirs(executable):
    base = executable.parent
    return sorted({dll.parent.relative_to(base).as_posix() for dll in base.rglob("*.dll")})


def relinker_command(relinker, libs, executable, output, dirs, console):
    command = [str(relinker), "--libs", str(libs), "--registry"]
    for directory in dirs:
        command += ["--module-dir", directory]
    if console:
        command.append("--console")
    return command + [str(executable), str(output)]


def place(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def place_remaining(package, out):
    placed = 0
    for source in package.rglob("*"):
        if not source.is_file() or source.suffix.lower() in NOT_COPIED_SUFFIXES:
            continue
        destination = out / source.relative_to(package)
        if destination.exists():
            continue
        place(source, destination)
        placed += 1
    return placed


def pick_executable(package, requested):
    config = next((p for p in package.iterdir() if p.name.lower() == "microsoftgame.config"), None)
    if requested:
        return package / requested
    if config is None:
        raise LaunchError(f"{package} has no MicrosoftGame.Config; pass --exe")
    names = executable_names(config.read_text(encoding="utf-8-sig"))
    if len(names) != 1:
        raise LaunchError(f"MicrosoftGame.Config lists {len(names)} executables ({', '.join(names) or 'none'}); pass --exe")
    return package / names[0]


def check_executable(executable, package):
    if not executable.is_file():
        containers = sorted(p.name for p in package.glob("*.xvd"))
        hint = f" The package holds {', '.join(containers)}; AnyBox does not decrypt or unpack it, so put the decrypted executable beside the package files." if containers else ""
        raise LaunchError(f"{executable} not found.{hint}")
    problem = pe_problem(read_head(executable))
    if problem:
        raise LaunchError(f"{executable} {problem}; AnyBox needs the decrypted PE32+ executable")


def convert(args):
    package = args.package.resolve()
    executable = pick_executable(package, args.exe)
    check_executable(executable, package)
    relinker = args.build_dir / "anybox-relinker.exe"
    libs = args.build_dir / "libs"
    for needed in (relinker, libs):
        if not needed.exists():
            raise LaunchError(f"{needed} not found; build AnyBox first (docs/dev/BUILD.md)")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    output = out / executable.relative_to(package)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = relinker_command(relinker, libs, executable, output, module_dirs(executable), args.console)
    print("[launch] " + " ".join(command))
    result = subprocess.run(command, cwd=package)
    if result.returncode != 0:
        raise LaunchError(f"the relinker exited with {result.returncode}")
    print(f"[launch] placed {place_remaining(package, out)} remaining package files in {out}")
    return output


def run(output):
    report = output.parent / "anybox-report.json"
    report.unlink(missing_ok=True)
    env = dict(os.environ)
    env.setdefault("ANYBOX_LOG_FILE", str(output.parent / "anybox.log"))
    code = subprocess.run([str(output)], cwd=output.parent, env=env).returncode
    print(f"[launch] exit=0x{code & 0xFFFFFFFF:X}")
    if report.is_file():
        import json
        data = json.loads(report.read_text(encoding="utf-8"))
        problems = run_report_summary.validate(data)
        print(run_report_summary.summary(data) if not problems else "[launch] report problems: " + "; ".join(problems))
    return code


def main():
    parser = argparse.ArgumentParser(description="Convert a decrypted GDK package into a Windows program and start it.")
    parser.add_argument("package", type=Path, help="folder holding the decrypted executable, its DLLs and MicrosoftGame.Config")
    parser.add_argument("out", type=Path, help="folder to write the converted program into")
    parser.add_argument("--exe", help="executable path relative to the package (default: the one in MicrosoftGame.Config)")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build", help="AnyBox build directory holding anybox-relinker.exe and libs")
    parser.add_argument("--console", action="store_true", help="use the Windows console subsystem")
    parser.add_argument("--no-run", action="store_true", help="convert only")
    args = parser.parse_args()
    try:
        output = convert(args)
    except LaunchError as error:
        print(f"[launch] {error}", file=sys.stderr)
        return 2
    return 0 if args.no_run else run(output)


if __name__ == "__main__":
    sys.exit(main())
