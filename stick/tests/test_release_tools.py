"""Release boundaries reject diagnostic, local, stale and corrupt inputs."""

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from stick.build_port import CORE, FQBN, LIBRARIES, source_inputs, stage
from stick.package_release import REQUIRED, validate_build
from tools.common import InputError, digest, read_json, write_json


class ReleaseBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="stickwalker-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        for name in REQUIRED:
            (self.folder / name).write_bytes(b"candidate fixture: " + name.encode())
        inputs = source_inputs() + [
            ROOT / "assets/placeholders" / a["file"]
            for a in read_json(ROOT / "config/artwork.json")
        ]
        self.manifest = {
            "bench_control": False,
            "artwork": "placeholders",
            "fqbn": FQBN,
            "core": CORE,
            "libraries": LIBRARIES,
            "source_sha256": {
                p.relative_to(ROOT).as_posix(): digest(p) for p in inputs
            },
            "outputs": {n: digest(self.folder / n) for n in REQUIRED},
        }
        baseline = read_json(ROOT / "stick/ir-timing-baseline.json")
        self.timing = {
            "elf_sha256": self.manifest["outputs"]["PwStick.ino.elf"],
            "reference_app_sha256": baseline["reference_app_sha256"],
            "functions": baseline["functions"],
            "passed": True,
        }
        self.write()

    def write(self):
        write_json(self.folder / "build-manifest.json", self.manifest)
        write_json(self.folder / "ir-timing.json", self.timing)

    def test_production_inventory_is_accepted(self):
        validate_build(self.folder)

    def test_local_and_diagnostic_builds_are_rejected(self):
        for change in ({"bench_control": True}, {"artwork": "local"}):
            with self.subTest(change=change):
                original = dict(self.manifest)
                self.manifest.update(change)
                self.write()
                with self.assertRaises(InputError):
                    validate_build(self.folder)
                self.manifest = original

    def test_changed_firmware_is_rejected(self):
        (self.folder / "PwStick.ino.bin").write_bytes(b"changed after compilation")
        with self.assertRaises(InputError):
            validate_build(self.folder)

    def test_stale_and_incomplete_inputs_are_rejected(self):
        name = "stick/build_port.py"
        self.manifest["source_sha256"][name] = "0" * 64
        self.write()
        with self.assertRaises(InputError):
            validate_build(self.folder)
        del self.manifest["source_sha256"][name]
        self.write()
        with self.assertRaises(InputError):
            validate_build(self.folder)

    def test_timing_report_must_match_elf(self):
        self.timing["elf_sha256"] = "0" * 64
        self.write()
        with self.assertRaises(InputError):
            validate_build(self.folder)

    def test_stage_is_independent_of_local_artwork_and_h8_outputs(self):
        sketch = self.folder / "PwStick"
        manifest = stage(sketch, "placeholders")
        self.assertTrue((sketch / "rom_assets.h").is_file())
        self.assertFalse(
            any(
                "assets/local/" in p or "build/6.02.02" in p
                for p in manifest["source_sha256"]
            )
        )
        self.assertTrue((sketch / "src/stick/board_runtime.cpp").is_file())


if __name__ == "__main__":
    unittest.main()
