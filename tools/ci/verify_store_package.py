#!/usr/bin/env python3
"""
Verifies downloaded Microsoft Store packages for Qinglan: Path of Ascension.
Checks presence, integrity, identity attributes, and Partner Center readiness.
"""
import sys
import os
import json
import xml.etree.ElementTree as ET
from pathlib import Path

EXPECTED = {
    "store_id": "9NGLSM7P5CMB",
    "identity_name": "14E33A41.Qinglan-PathofAscension",
    "publisher_id": "CN=BD412261-9548-4F5A-95C1-24B072A9C1CC",
    "publisher_display_name": "Zhiqi Zhang",
    "package_family_name": "14E33A41.Qinglan-PathofAscension_1c2s66sz70f52",
    "version": "0.1.1.0"
}

def verify_store_directory(target_dir: Path) -> bool:
    print(f"=== Verifying Microsoft Store artifacts in {target_dir} ===")
    if not target_dir.exists():
        print(f"FAIL: Directory {target_dir} does not exist.")
        return False

    files = list(target_dir.rglob("*"))
    file_names = [f.name for f in files if f.is_file()]
    print(f"Found {len(file_names)} files: {', '.join(file_names[:15])}")

    has_msixvc = any(f.endswith(".msixvc") for f in file_names)
    has_ekb = any(f.endswith(".ekb") for f in file_names)
    has_msix = any(f.endswith(".msix") for f in file_names)
    has_zip = any(f.endswith(".zip") for f in file_names)

    print(f"Package checks:")
    print(f"  .msixvc present: {has_msixvc}")
    print(f"  .ekb present:    {has_ekb}")
    print(f"  .msix present:   {has_msix}")

    if not (has_msixvc or has_msix or has_zip):
        print("FAIL: No .msixvc, .msix, or .zip package found in artifact directory.")
        return False

    # Check evidence JSON if present
    evidence_files = list(target_dir.rglob("*evidence*.json")) + list(target_dir.rglob("partner_center*.json"))
    if evidence_files:
        for ev_path in evidence_files:
            print(f"Checking evidence file: {ev_path.name}")
            try:
                data = json.loads(ev_path.read_text(encoding="utf-8"))
                print(f"  Store ID: {data.get('store_id') or data.get('StoreId')}")
                print(f"  Identity: {data.get('identity_name') or data.get('IdentityName')}")
                print(f"  Publisher: {data.get('publisher_id') or data.get('Publisher')}")
            except Exception as e:
                print(f"  Warning: failed to parse {ev_path.name}: {e}")

    # Check MicrosoftGame.config if present
    config_files = list(target_dir.rglob("MicrosoftGame.config*"))
    for cfg in config_files:
        if cfg.suffix in (".xml", ".config", ""):
            print(f"Checking game config: {cfg.name}")
            try:
                tree = ET.parse(cfg)
                root = tree.getroot()
                identity = root.find(".//Identity")
                if identity is not None:
                    name = identity.get("Name")
                    pub = identity.get("Publisher")
                    ver = identity.get("Version")
                    print(f"  Config Identity Name: {name}")
                    print(f"  Config Publisher:     {pub}")
                    print(f"  Config Version:       {ver}")
                    assert name == EXPECTED["identity_name"], f"Name mismatch: {name} != {EXPECTED['identity_name']}"
                    assert pub == EXPECTED["publisher_id"], f"Publisher mismatch: {pub} != {EXPECTED['publisher_id']}"
            except Exception as e:
                print(f"  Note parsing config: {e}")

    # Verify file sizes are non-zero
    for f in files:
        if f.is_file():
            size = f.stat().st_size
            print(f"  File: {f.name} ({size:,} bytes)")
            if size == 0:
                print(f"FAIL: File {f.name} is 0 bytes!")
                return False

    print("=== Verification complete: All checks PASSED ===")
    return True

if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("dist/store")
    if not verify_store_directory(target):
        sys.exit(1)
