# Filesystem-FFS

Canonical independent AmigaDOS Fast File System driver for InfiltratorOS. FFS is
distinct from OFS (Original File System); neither driver's on-disk algorithms
belong inside Defragmenter.

The **default `make` output is the unified `FFS.so`**, and the CMake
`ffs-unified` target produces the same module. It exposes the original
freestanding read-only V1 API and bounded existing-file overwrite V2 API,
plus the separately versioned native FFS Defrag/Recover V1 and Test Media
format/populate/verify V1 interfaces. The Linux host provides generic I/O,
identity/stop controls and journal services over typed ABIs. No application
fallback to private FFS filesystem source is permitted.

- `native/filesystems/ffs/core/` and `native/filesystems/ffs/module/` own
  the independent freestanding read API and limited overwrite logic
- `native/defrag/` owns the scanner, FFS allocation planner, relayout,
  journalling and recovery implementation migrated from Defragmenter
- `native/fixtures/` owns disposable-image formatting, fragmented file
  population, and payload verification
- `module/` contains the independently callable defragmentation and
  Test Media interfaces
- `tests/` validates the reader and a disposable fragmented FFS image

Run `make test` to qualify both the legacy **FFS-legacy.so** ABI and the
unified **FFS.so** formatter, payload, and API exports. The Makefile keeps
the legacy test artifact separate; it must not overwrite the distributable
driver.

This project does **not** claim universal EFI/BOOT/kernel loading, fully
qualified boot-device modification, or unlimited AmigaDOS variants.
Only the tested native DOS1/DOS3/DOS5 subset is supported by the current
mutation path. Do not apply OFS data-header logic to FFS raw data blocks.
