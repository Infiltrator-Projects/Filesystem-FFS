#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce the independent FFS shared-object source and package boundary."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
cfg = json.loads((root / "standalone/config.json").read_text())
assert cfg["repository"] == "Filesystem-FFS"
assert cfg["module"] == "FFS.so"
assert cfg["pdf"] == "docs/FFS.pdf"
assert cfg["abi"] == "infiltrator_filesystem_ffs_defrag_v1"
assert "backend_revision" not in cfg
cmake = (root / "CMakeLists.txt").read_text()
assert len(re.findall(r"\badd_library\([^\n]*\bSHARED\b", cmake)) == 1
assert 'OUTPUT_NAME "FFS"' in cmake
assert "install(TARGETS ffs-unified LIBRARY DESTINATION /System/Filesystems)" in cmake
assert not (root / "native/filesystems/ofs").exists()
assert not (root / "native/filesystems/exfat").exists()
assert not (root / "native/filesystems/fat").exists()
builder = (root / "standalone/build.py").read_text()
assert "backend-ext4" not in builder
assert "Filesystem-EXT4" not in builder
assert "backend_revision" not in builder
# Every repository owns exactly one format implementation directory; shared
# portable support may live elsewhere, but foreign format trees may not return.
filesystem_root = root / "native/filesystems"
assert {p.name for p in filesystem_root.iterdir() if p.is_dir()} == {"ffs"}
assert not (root / "native/providers").exists()
# Installation builds exactly one corresponding filesystem SO. Additional
# test executables and freestanding regression SOs must remain noninstallable.
assert len(re.findall(r"\binstall\(TARGETS\s+[^)]+\)", cmake)) == 1
assert (root / "standalone/config.json").is_file()
release_source = (root / "standalone/release.py").read_text()
assert 'expected = {Path(f"System/Filesystems/{MODULE}"), Path(f"System/Resources/Documentation/{REPO}.pdf")}' in release_source
assert 'if actual != expected:' in release_source
print("FFS-ISOLATED-SO-SOURCE-OWNER-PASS")
