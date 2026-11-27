# Filesystem-FFS

## 1. Purpose and format scope

Amiga Fast File System (DOS1/DOS3/DOS5 mutation subset), distinct from OFS. This repository owns the format-specific implementation of `FFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

FFS stores raw payload blocks without the OFS 24-byte data header. The root, hashed directories, file headers, allocation bitmap and extension chains define ownership and block mapping. Native file growth allocates bitmap entries and links extension headers when the initial pointer table is exhausted.

## 3. Common V2 implementation and capabilities

Common V2 declares probe, mount, stat, enumeration, read, write, create and DOS1 format. Native file header creation, bitmap allocation and interleaved extension-chain growth are implemented. Extent mapping and volume query are distinct from relocation. The historical test-media and relocation engine remain in source. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Writes require exclusive durable host access. Do not infer DOS7 or universal directory-cache mutation, complete power-loss safety, or kernel/firmware acceptance from source tests. Keep legacy test artifacts out of production. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/FFS.so` and `/System/Resources/Documentation/Filesystem-FFS.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-FFS/actions/runs/45534284195) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-FFS/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/ffs_common.c)
- [Migration and source history](docs/DEFRAg_MIGRATION.md)
- [Plain-text PDF drawing source (not Apple Pages)](docs/FFS.pdfstreams) and [PDF](docs/FFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The canonical specification and installed manual include the historical release chronology, DOS0-DOS7 distinctions, RDB/Kickstart boot history, validation and repair tools, TD64/NSD and large-media boundaries, directory-cache limitations, LNFS and independent source references. Historical handler support is not a claim of current provider parity.

The [engineering reference](docs/FFS-TECHNICAL-REFERENCE.md) incorporates the detailed format specification and worked exercises. The PDF and Markdown are one integrated reference; superseded editions remain in Git history.
