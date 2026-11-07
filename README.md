# Filesystem-FFS

Canonical independent AmigaDOS Fast File System driver for InfiltratorOS. FFS is
distinct from OFS (Original File System); neither driver's on-disk algorithms
belong inside Defragmenter.

**One filesystem, one repository, one distributable shared object:** this
repository produces only `FFS.so` for FFS. "Unified" means the FFS reader,
writer, defragmenter and test-media interfaces share one FFS-specific binary.
It does **not** combine FFS with OFS, exFAT or any other format. The older
`FFS-legacy.so` is an internal regression-test artifact, never an installable
driver.

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

## Installer files

- `/System/Filesystems/FFS.so`
- `/System/Resources/Documentation/Filesystem-FFS.pdf`
- `/System/Kernel/Filesystems/ffs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.
