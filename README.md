# Filesystem-FFS

**Canonical source owner:** independent AmigaDOS Fast File System implementation for InfiltratorOS. Source and original unit tests imported, file-for-file, from `Infiltrator-Projects/Filesystems` branch commit `c0ca29597293c1bc4a7cef231dffe37f57722e8c`, including its existing freestanding DOS1/3/5 `FFS.so` V1/V2 implementation.

- `native/filesystems/ffs/core/` is the format-specific canonical implementation.
- `native/filesystems/ffs/module/` is an independent freestanding FFS.so reader and bounded existing-file overwrite implementation.
- `native/filesystems/ffs/linux/` is the project-owned separate FFS Linux driver implementation.
- `tests/ffs_*` contains the imported FFS qualification fixtures and tests.

`make test` builds and runs the native SO and tests on disposable memory/media, never altering boot devices. **Not claimed complete:** full shared-object namespace mutation, complete format support, crash-safe writes, EFI driver, universal BOOT/kernel SO loader or physically qualified root installations. Do not substitute OFS data-header logic for FFS raw data blocks.
