"""Share one external staging directory between Unreal and the native cooker."""
import os
from pathlib import Path


def import_work(group):
    location = os.environ.get("VCR_IMPORT_WORK")
    if not location:
        raise RuntimeError("Set VCR_IMPORT_WORK to the temporary directory for this import.")
    root = Path(location).resolve()
    workspace = (root / group).resolve()
    engine = Path(__file__).resolve().parents[1]
    projects = engine.parent / "projects"
    if workspace.parent != root or any(p == workspace or p in workspace.parents for p in (engine, projects)):
        raise ValueError("Use a temporary import workspace outside the engine and projects directories.")
    workspace.mkdir(parents=True, exist_ok=True)
    return workspace
