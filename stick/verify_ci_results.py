#!/usr/bin/env python3
"""Verify the complete firmware suite and assemble its margin icon previews."""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
from pathlib import Path

from check import ROOT, SCENARIOS, digest, render_margin_atlas, shard_scenarios

ATLAS_SCENARIOS = ("diode-presence", "diode", "diode-ready", "connect", "connection-details")
PROVENANCE = (
    "application_sha256",
    "merged_sha256",
    "firmware_sources_sha256",
    "verification_sources_sha256",
    "simulator_revision",
    "simulator_patch_sha256",
    "simulator_binary_sha256",
    "simulator_rom_sha256",
)


def verify_results(root: Path, count: int, output: Path) -> None:
    reference = None
    locations = {}
    folders = {}
    for folder in root.glob("firmware-summary-*/shard-*"):
        index = int(folder.name.removeprefix("shard-"))
        if index in folders:
            raise RuntimeError(f"Duplicate shard {index}")
        folders[index] = folder
    if set(folders) != set(range(1, count + 1)):
        raise RuntimeError("Scenario summaries are missing or include unexpected shards")
    for index in range(1, count + 1):
        folder = folders[index]
        result = json.loads((folder / "results.json").read_text())
        expected = set(shard_scenarios(index, count))
        if (
            not result["complete"]
            or set(result["scenarios"]) != expected
            or set(result["scenario_actions_sha256"]) != expected
        ):
            raise RuntimeError(f"Shard {index} did not complete its assigned scenarios")
        provenance = {name: result[name] for name in PROVENANCE}
        if reference is not None and provenance != reference:
            raise RuntimeError(f"Shard {index} used different firmware or test sources")
        reference = provenance
        locations.update({name: folder / name for name in expected})

    for key in ("firmware_sources_sha256", "verification_sources_sha256"):
        for name, expected in reference[key].items():
            if digest(ROOT / name) != expected:
                raise RuntimeError(f"Results do not match {name}")

    with tempfile.TemporaryDirectory() as temporary:
        atlas = Path(temporary)
        for name in ATLAS_SCENARIOS:
            (atlas / name).symlink_to(locations[name].resolve(), target_is_directory=True)
        render_margin_atlas(atlas)
        image = atlas / "margin-icons.png"
        if not image.exists():
            raise RuntimeError("Results are missing margin icon screenshots")
        shutil.copyfile(image, output / "margin-icons.png")
    print(f"All {len(SCENARIOS)} scenarios passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--shards", type=int, required=True)
    args = parser.parse_args()
    if len(set(SCENARIOS)) != len(SCENARIOS):
        raise RuntimeError("Scenario names must be unique")
    args.output.mkdir(parents=True, exist_ok=True)
    verify_results(args.results, args.shards, args.output)


if __name__ == "__main__":
    main()
