import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anybox_scan
import import_audit

ARCHIVE_MAGIC = b"!<arch>\n"
NAME_TYPES = {0: "ordinal", 1: "name", 2: "noprefix", 3: "undecorate", 4: "exportas"}


class InventoryError(Exception):
    pass


def export_name(symbol, name_type, hint, extra):
    if name_type in (0, 1):
        return symbol
    stripped = symbol[1:] if symbol[:1] in ("?", "@", "_") else symbol
    if name_type == 2:
        return stripped
    if name_type == 3:
        return stripped.split("@", 1)[0]
    if name_type == 4:
        return extra or symbol
    raise InventoryError(f"unknown import name type {name_type}")


def read_import_library(path):
    data = path.read_bytes()
    if not data.startswith(ARCHIVE_MAGIC):
        raise InventoryError(f"{path}: not an archive (.lib)")
    entries = {}
    objects = 0
    offset = len(ARCHIVE_MAGIC)
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        if header[58:60] != b"`\n":
            raise InventoryError(f"{path}: bad archive member header at 0x{offset:x}")
        name = header[:16].decode("ascii", errors="replace").strip()
        size = int(header[48:58].decode("ascii").strip())
        body = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
        if name in ("/", "//", "/<ECSYMBOLS>/"):
            continue
        if len(body) < 20:
            continue
        sig1, sig2, version, machine, _time, length, hint, bits = struct.unpack_from("<HHHHIIHH", body, 0)
        if sig1 != 0 or sig2 != 0xFFFF or version != 0:
            objects += 1
            continue
        strings = body[20:20 + length].split(b"\0")
        symbol = strings[0].decode("ascii", errors="replace")
        dll = strings[1].decode("ascii", errors="replace").lower()
        extra = strings[2].decode("ascii", errors="replace") if len(strings) > 2 and strings[2] else None
        name_type = (bits >> 2) & 7
        entries.setdefault(dll, set()).add(export_name(symbol, name_type, hint, extra))
    if not entries and objects:
        raise InventoryError(f"{path}: no short-format import entries (a static library, or a GNU dlltool import library); read the DLL instead")
    return entries


def read_dll(path):
    pe = import_audit.Pe(path)
    dll, functions = pe.exports()
    return {(dll or path.name).lower(): set(functions)}


def collect(paths):
    inventory = {}
    for path in paths:
        files = sorted(p for p in path.rglob("*") if p.suffix.lower() in (".lib", ".dll")) if path.is_dir() else [path]
        for file in files:
            try:
                found = read_import_library(file) if file.suffix.lower() == ".lib" else read_dll(file)
            except (InventoryError, import_audit.AuditError, struct.error, ValueError) as error:
                print(f"skipped {file}: {error}", file=sys.stderr)
                continue
            for dll, functions in found.items():
                inventory.setdefault(dll, set()).update(functions)
    return {dll: sorted(functions) for dll, functions in sorted(inventory.items())}


def rename(inventory, renames):
    result = {}
    for dll, functions in inventory.items():
        target = renames.get(dll, dll)
        result[target] = sorted(set(result.get(target, [])) | set(functions))
    return dict(sorted(result.items()))


def parse_rename(value):
    source, separator, target = value.partition("=")
    if not separator or not source or not target:
        raise argparse.ArgumentTypeError(f"expected FROM=TO, got {value!r}")
    return source.lower(), target.lower()


def coverage(inventory, libraries):
    rows = []
    for dll, functions in inventory.items():
        library = anybox_scan.route(libraries, dll)
        statuses = [library.status(function) if library else "unrouted" for function in functions]
        rows.append({
            "dll": dll,
            "library": library.name if library else None,
            "exports": len(functions),
            "declared": sum(status != "absent" and status != "unrouted" for status in statuses),
            "done": sum(status in ("implemented", "forwarded") for status in statuses),
        })
    return rows


def main():
    parser = argparse.ArgumentParser(
        description="List every export of DLLs or import libraries (.lib), for example an installed GDK, and compare them with what AnyBox declares.")
    parser.add_argument("paths", nargs="+", type=Path, help=".lib or .dll files, or folders searched recursively")
    parser.add_argument("--json", type=Path, help="write the inventory (docs/inventory/<name>.json) for tools/needs.py")
    parser.add_argument("--label", default="", help="where the inventory came from, for example 'GDK 2410 GRDK'")
    parser.add_argument("--rename", action="append", type=parse_rename, default=[], metavar="FROM=TO",
                        help="record the exports of DLL FROM under DLL name TO, applied before --only; repeatable")
    parser.add_argument("--only", action="append", help="keep only this DLL name; repeatable")
    args = parser.parse_args()

    inventory = rename(collect(args.paths), dict(args.rename))
    if args.only:
        wanted = {name.lower() for name in args.only}
        inventory = {dll: functions for dll, functions in inventory.items() if dll in wanted}
    if not inventory:
        print("no exports found", file=sys.stderr)
        return 1
    for row in coverage(inventory, anybox_scan.scan()):
        served = f"-> {row['library']}" if row["library"] else "-> unrouted"
        print(f"{row['dll']:<40} {row['exports']:>5} exports  {row['declared']:>5} declared  {row['done']:>5} done  {served}")
    if args.json:
        record = {"format": "anybox-inventory", "version": 1, "label": args.label or None, "dlls": inventory}
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
