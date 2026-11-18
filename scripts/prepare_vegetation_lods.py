"""Prepare generated vegetation LODs for the scenes of a Windows engine project."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="Project directory containing project.vcrproject.json")
    parser.add_argument("--scene", action="append", help="Scene relative to assets (default: all project scenes)")
    args = parser.parse_args()
    project = args.project.resolve()
    assets = project / "assets"
    manifest = json.loads((project / "project.vcrproject.json").read_text(encoding="utf-8-sig"))
    scenes = [assets / scene for scene in args.scene] if args.scene else sorted(
        {assets / manifest["startup_scene"], *(assets / "scenes").rglob("*.json")})
    models = set()
    for scene in scenes:
        document = json.loads(scene.read_text(encoding="utf-8-sig"))
        for node in document.get("nodes", []):
            if node.get("kind") == "model" and node.get("asset_path"):
                models.add(node["asset_path"])
    cooker = root / "build" / "windows-release" / "bin" / "vcr_asset_lod_cooker.exe"
    command = [str(cooker), "cook-vegetation", str(assets)]
    # Bound the UTF-16 command line on Windows even for deeply nested imports.
    base_bytes = len(subprocess.list2cmdline(command).encode("utf-16-le"))
    batch_bytes = base_bytes
    batch = []
    for model in sorted(models):
        argument_bytes = 2 + len(subprocess.list2cmdline([model]).encode("utf-16-le"))
        if batch and batch_bytes + argument_bytes > 60000:
            subprocess.run(command + batch, check=True)
            batch.clear()
            batch_bytes = base_bytes
        batch.append(model)
        batch_bytes += argument_bytes
    if batch:
        subprocess.run(command + batch, check=True)


if __name__ == "__main__":
    main()
