import struct
import unittest
from pathlib import Path

import helpers

import launch_title

CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1"><ExecutableList>
<Executable Name="game.exe" IsDevOnly="false" />
<Executable Name="dev.exe" IsDevOnly="true" />
</ExecutableList></Game>"""


def image(machine=launch_title.MACHINE_AMD64, magic=launch_title.PE32_PLUS):
    data = bytearray(0x200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    struct.pack_into("<H", data, 0x98, magic)
    return bytes(data)


class LaunchTitleTest(unittest.TestCase):
    def test_executable_names_skip_dev_only(self):
        self.assertEqual(launch_title.executable_names(CONFIG), ["game.exe"])
        with self.assertRaises(launch_title.LaunchError):
            launch_title.executable_names("<Game")

    def test_pe_problem(self):
        self.assertIsNone(launch_title.pe_problem(image()))
        self.assertIn("MZ", launch_title.pe_problem(b"\x00" * 0x200))
        self.assertIn("PE32+ x64", launch_title.pe_problem(image(machine=0x14C)))
        self.assertIn("PE32+ x64", launch_title.pe_problem(image(magic=0x10B)))
        broken = bytearray(image())
        struct.pack_into("<I", broken, 0x3C, 0x7FFFFFFF)
        self.assertIn("outside", launch_title.pe_problem(bytes(broken)))
        self.assertIn("MZ", launch_title.pe_problem(b"MZ"))

    def test_module_dirs(self):
        with helpers.temp_root() as root:
            root = Path(root)
            for name in ("a.dll", "bin/b.dll", "data/c.bin"):
                helpers.write(root, name, "x")
            self.assertEqual(launch_title.module_dirs(root / "game.exe"), [".", "bin"])
            self.assertEqual(launch_title.module_dirs(root / "bin" / "game.exe"), ["."])
            self.assertEqual(launch_title.module_dirs(root / "data" / "game.exe"), [])

    def test_relinker_command(self):
        command = launch_title.relinker_command(Path("r.exe"), Path("libs"), Path("in.exe"), Path("out.exe"), [".", "bin"], True)
        self.assertEqual(command, ["r.exe", "--libs", "libs", "--registry", "--module-dir", ".", "--module-dir", "bin", "--console", "in.exe", "out.exe"])

    def test_missing_executable_names_the_container(self):
        with helpers.temp_root() as root:
            root = Path(root)
            helpers.write(root, "gameos.xvd", "x")
            with self.assertRaises(launch_title.LaunchError) as raised:
                launch_title.check_executable(root / "game.exe", root)
            self.assertIn("gameos.xvd", str(raised.exception))

    def test_place_remaining_skips_containers_and_converted(self):
        with helpers.temp_root() as package, helpers.temp_root() as out:
            package, out = Path(package), Path(out)
            helpers.write(package, "data/a.rpf", "a")
            helpers.write(package, "gameos.xvd", "x")
            helpers.write(package, "done.dll", "new")
            helpers.write(out, "done.dll", "converted")
            launch_title.place_remaining(package, out)
            self.assertTrue((out / "data" / "a.rpf").is_file())
            self.assertFalse((out / "gameos.xvd").exists())
            self.assertEqual((out / "done.dll").read_text(encoding="utf-8"), "converted")


if __name__ == "__main__":
    unittest.main()
