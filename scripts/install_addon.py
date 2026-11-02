#!/usr/bin/env python3
"""Adds the compiled add-on to a copy of YARG.app so that the game loads it at startup."""
import json
import shutil
import sys
from pathlib import Path

ASSEMBLY = "WiimoteGuitarBridge"
TEST_BUNDLE_ID = "com.shininchi.yarg-wiimote-test"


def main(app: Path, dll: Path) -> None:
    data = app / "Contents/Resources/Data"
    shutil.copy2(dll, data / "Managed" / f"{ASSEMBLY}.dll")

    assemblies_path = data / "ScriptingAssemblies.json"
    assemblies = json.loads(assemblies_path.read_text())
    if f"{ASSEMBLY}.dll" not in assemblies["names"]:
        assemblies["names"].append(f"{ASSEMBLY}.dll")
        assemblies["types"].append(16)
        assemblies_path.write_text(json.dumps(assemblies))

    startup_path = data / "RuntimeInitializeOnLoads.json"
    startup = json.loads(startup_path.read_text())
    if not any(entry["assemblyName"] == ASSEMBLY for entry in startup["root"]):
        startup["root"].append({
            "assemblyName": ASSEMBLY,
            "nameSpace": "WiimoteGuitar",
            "className": "Bridge",
            "methodName": "Initialize",
            "loadTypes": 0,
            "isUnityClass": False,
        })
        startup_path.write_text(json.dumps(startup))

    # A separate identity keeps this copy's settings and scores apart from the real game.
    info_path = app / "Contents/Info.plist"
    info = info_path.read_text()
    info_path.write_text(info.replace("<string>in.yarg.game</string>", f"<string>{TEST_BUNDLE_ID}</string>"))


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
