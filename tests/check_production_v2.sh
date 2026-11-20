#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
binary="${1:?FFS.so required}"
test -s "$binary"
nm -D --defined-only "$binary" | grep -E ' [TW] infiltrator_filesystem_api_v2$'
if nm -D --defined-only "$binary" | grep -E 'infiltrator_filesystem_module_v[12]|infiltrator_filesystem_ffs_(fixture|defrag)_v1|ifs_ffs_native_(read|write)_ops|ldtm_raw_|ifs_ffs_worker_main'; then
 echo 'Historical filesystem ABI leaked into production FFS.so' >&2
 exit 1
fi
readelf -SW "$binary" | grep -F '.infiltrator_boot'
