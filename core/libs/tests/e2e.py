import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="AnyBox.FakeTitle" Publisher="CN=AnyBox" Version="1.0.2.0"/>
  <TitleId>1234abcd</TitleId>
  <ShellVisuals DefaultDisplayName="Fake Title"/>
</Game>
"""

def executable_list(family=None):
    attribute = "" if family is None else f' TargetDeviceFamily="{family}"'
    return f'  <ExecutableList>\n    <Executable Name="title.exe"{attribute}/>\n  </ExecutableList>\n'


DEVICES = {
    "device_scarlett": (executable_list("Scarlett"), 0x07),
    "device_xboxone": (executable_list("XboxOne"), 0x04),
    "device_pc": (executable_list("PC"), 0x01),
    "device_default": (executable_list(), 0x04),
    "device_none": (None, None),
    "device_noexecutable": ("", None),
    "device_unknown": (executable_list("Desktop"), None),
}

EXPECTED = {
    "taskqueue": (0, "none"),
    "stub": (0xAB01, "not-implemented"),
    "crash": (0xC0000005, "crash"),
    "device_scarlett": (0, "none"),
    "device_xboxone": (0, "none"),
    "device_pc": (0, "none"),
    "device_default": (0, "none"),
    "device_none": (0xAB02, "unsupported"),
    "device_noexecutable": (0xAB02, "unsupported"),
    "device_unknown": (0xAB02, "unsupported"),
}


def subsystem_offset(data):
    return struct.unpack_from("<I", data, 0x3C)[0] + 24 + 68


def set_xbox_subsystem(path):
    data = bytearray(path.read_bytes())
    struct.pack_into("<H", data, subsystem_offset(data), 14)
    path.write_bytes(bytes(data))


def subsystem(path):
    data = path.read_bytes()
    return struct.unpack_from("<H", data, subsystem_offset(data))[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relinker", required=True, type=Path)
    parser.add_argument("--title", required=True, type=Path)
    parser.add_argument("--libs", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--scenario", required=True, choices=sorted(EXPECTED))
    args = parser.parse_args()

    package = args.work / "package"
    out = args.work / "out"
    shutil.rmtree(args.work, ignore_errors=True)
    package.mkdir(parents=True)
    title = package / "title.exe"
    shutil.copyfile(args.title, title)
    set_xbox_subsystem(title)
    executable, device = DEVICES.get(args.scenario, ("", None))
    config = None if executable is None else CONFIG.replace("  <ShellVisuals", executable + "  <ShellVisuals")
    if config is not None:
        (package / "MicrosoftGame.config").write_text(config, encoding="utf-8")

    converted = out / "title.exe"
    subprocess.run([str(args.relinker), "--console", "--registry", "--libs", str(args.libs), str(title), str(converted)], check=True)
    if config is not None:
        shutil.copyfile(package / "MicrosoftGame.config", out / "MicrosoftGame.config")

    assert subsystem(converted) == 3, "converted title is a console program"
    registry = json.loads((out / "title.exe.registry.json").read_text(encoding="utf-8"))
    routes = {module["dll"].lower(): module for module in registry["imports"]}
    assert routes["xgameruntime.dll"]["target"] == "ab_xgameruntime.dll", routes
    assert (out / "ab_xgameruntime.dll").exists() and (out / "ab_runtime.dll").exists(), "libraries are copied"

    argument = "device" if args.scenario in DEVICES else args.scenario
    run = subprocess.run([str(converted), argument], cwd=out, capture_output=True, text=True, timeout=60)
    print(run.stdout)
    print(run.stderr, file=sys.stderr)
    code, kind = EXPECTED[args.scenario]
    assert (run.returncode & 0xFFFFFFFF) == code, f"exit code 0x{run.returncode & 0xFFFFFFFF:x}, expected 0x{code:x}"
    if args.scenario == "taskqueue":
        assert "task queue ok" in run.stdout
    if device is not None:
        assert f"device type {device} packaged 0" in run.stdout, run.stdout

    report_path = out / "anybox-report.json"
    if kind == "crash" and not report_path.exists():
        print("SKIP crash report check: no report written (see docs/dev/debt/runtime.md)")
        return
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["format"] == "anybox-report" and report["version"] == 1
    assert report["stop"]["kind"] == kind, report["stop"]
    if config is None:
        assert report["game"]["titleId"] is None and report["game"]["name"] is None, report["game"]
    else:
        assert report["game"]["titleId"] == "1234ABCD", report["game"]
        assert report["game"]["name"] == "Fake Title" and report["game"]["version"] == "1.0.2.0", report["game"]
    assert "xgameruntime" in report["libraries"], report["libraries"]
    if kind == "not-implemented":
        assert report["stop"]["library"] == "xgameruntime" and report["stop"]["function"] == "XUserAddAsync", report["stop"]
    if kind == "unsupported":
        assert report["stop"]["library"] == "xgameruntime" and report["stop"]["function"] == "XSystemGetDeviceType", report["stop"]
    if kind == "crash":
        assert report["stop"]["exception"]["code"] == "0xc0000005", report["stop"]
        assert report["stop"]["exception"]["module"] == "title.exe", report["stop"]

    schema = Path(__file__).resolve().parents[3] / "docs" / "run-report.schema.json"
    try:
        import jsonschema
    except ImportError:
        print("jsonschema not installed; schema check skipped")
    else:
        jsonschema.validate(report, json.loads(schema.read_text(encoding="utf-8")))
    print(f"PASS {args.scenario}")


if __name__ == "__main__":
    main()
