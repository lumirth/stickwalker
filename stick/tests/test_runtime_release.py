"""Release relinking preserves ordered inputs and rejects mixed/corrupt kits."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from stick.package_release import validate_runtime
from stick.package_runtime import portable_arguments
from tools.common import InputError, digest, write_json


class RuntimeReleaseTests(unittest.TestCase):
    def test_relink_works_after_move_and_replaces_core(self):
        with tempfile.TemporaryDirectory(prefix="relink test ") as temp:
            root = Path(temp).resolve()
            kit = root / "moved kit"
            kit.mkdir()
            (kit / "relink.py").write_bytes((ROOT / "stick/relink.py").read_bytes())
            toolchain = root / "compiler with spaces"
            (toolchain / "bin").mkdir(parents=True)
            compiler = toolchain / "bin/xtensa-esp32s3-elf-g++"
            compiler.write_text(
                "#!/usr/bin/env python3\nimport json,sys\nfrom pathlib import Path\nPath('output/arguments.json').write_text(json.dumps(sys.argv[1:]))\n"
            )
            compiler.chmod(0o755)
            arguments = [
                "/old/compiler/bin/xtensa-esp32s3-elf-g++",
                "-Wl,--Map=/old/build/image.map",
                "-L/old/sdk/lib",
                "/old/build/sketch/application.o",
                "/old/build/core/core.a",
                "@/old/sdk/flags/ld_libs",
                "-o",
                "/old/build/PwStick.ino.elf",
            ]
            portable = portable_arguments(arguments, "/old/build", "/old/sdk", "/old/compiler")
            write_json(kit / "link.json", {"arguments": portable})
            replacement = root / "modified core.a"
            replacement.write_bytes(b"replacement core fixture")
            subprocess.run(
                [
                    sys.executable,
                    str(kit / "relink.py"),
                    "--toolchain",
                    str(toolchain),
                    "--core",
                    str(replacement),
                ],
                check=True,
                capture_output=True,
            )
            received = json.loads((kit / "output/arguments.json").read_text())
            self.assertEqual(
                received[2:5],
                [
                    str(kit / "objects/sketch/application.o"),
                    str(replacement),
                    "@" + str(kit / "sdk/flags/ld_libs"),
                ],
            )
            self.assertEqual(received[-1], str(kit / "output/PwStick.ino.elf"))
            self.assertNotIn("/old/", " ".join(received))

    def test_unbundled_link_input_is_rejected(self):
        for argument in ("/missing/file.o", "-L/missing/lib", "@/missing/flags"):
            with self.subTest(argument=argument), self.assertRaises(InputError):
                portable_arguments([argument], "/build", "/sdk", "/compiler")

    def test_runtime_binding_and_inventory(self):
        with tempfile.TemporaryDirectory() as temp:
            kit = Path(temp)
            for name in (
                "relink.py",
                "link.json",
                "objects/core/core.a",
                "sdk/flags/ld_libs",
                "licenses/Arduino-LGPL-2.1.txt",
                "sources/lib.zip",
            ):
                path = kit / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"relink fixture")
            sources = {"archives": {"lib.zip": {"sha256": digest(kit / "sources/lib.zip")}}}
            write_json(kit / "sources.json", sources)
            runtime = {
                "relink_verified": True,
                "elf_sha256": "elf",
                "app_sha256": "app",
                "files": {
                    p.relative_to(kit).as_posix(): digest(p) for p in kit.rglob("*") if p.is_file()
                },
            }
            write_json(kit / "runtime-manifest.json", runtime)
            build = {"outputs": {"PwStick.ino.elf": "elf", "PwStick.ino.bin": "app"}}
            import stick.package_release as release

            original = release.read_json
            with patch.object(
                release,
                "read_json",
                side_effect=lambda p: (
                    sources if p == ROOT / "stick/runtime-sources.json" else original(p)
                ),
            ):
                validate_runtime(kit, build)
                build["outputs"]["PwStick.ino.bin"] = "other app"
                with self.assertRaises(InputError):
                    validate_runtime(kit, build)
                build["outputs"]["PwStick.ino.bin"] = "app"
                (kit / "unexpected.o").write_bytes(b"extra input")
                with self.assertRaises(InputError):
                    validate_runtime(kit, build)
                (kit / "unexpected.o").unlink()
                (kit / "objects/core/core.a").write_bytes(b"changed after verification")
                with self.assertRaises(InputError):
                    validate_runtime(kit, build)


if __name__ == "__main__":
    unittest.main()
