
import unreal
# Run both builders in one session
import importlib.util
from pathlib import Path

def run(path):
    spec = importlib.util.spec_from_file_location("mod", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if hasattr(mod, "main"):
        mod.main()

root = Path(r"/Users/harrison/PycharmProjects/SailSimUE/Scripts")
run(root / "create_navt_materials.py")
run(root / "create_navt_terrain_material.py")
unreal.log("Season materials rebuild complete")
