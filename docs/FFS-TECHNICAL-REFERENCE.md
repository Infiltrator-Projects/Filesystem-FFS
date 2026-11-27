# AmigaDOS Fast File System (FFS) — engineering reference

One format reference covering four FFS variants, their historical development, original on-media structures, algorithms and implementation requirements. Historical handler support and theoretical format limits do not establish current provider qualification.

## 1. Origins and release chronology

Commodore International's original AmigaDOS disk filesystem shipped in 1985 with AmigaOS 1.0; the designation OFS (Old or Original File System) was applied retrospectively. It was designed around floppy disks. FFS (Fast File System, also called AFFS in Linux) was introduced with AmigaOS 1.3 in 1988, handler v34. FFS removes the 24-byte per-data-block OFS header, permitting direct transfer of whole payload blocks and increasing capacity and sequential throughput on hard disks. OFS retains type, owner, sequence, length, next-block and checksum fields; FFS raw payload blocks do not. This affects salvage as well as performance.

AmigaOS 1.0–1.2: original DOS0. AmigaOS 1.3 (1988, v34): DOS1 and compatibility with DOS0. AmigaOS 2.0 (1990, v37; international profiles documented for 2.04): DOS2/DOS3, ROM-integrated FFS. AmigaOS 3.0 (1992, v39): DOS4/DOS5. AmigaOS 3.1 (1993, v40/v40.1): last Commodore FFS generation. Later third-party v44 TD64 patches and licensed v45 NSD handler releases (AmigaOS 3.5/3.9, Haage & Partner) extended device addressing.

In the early 2000s, Olaf Barthel's FFS2 evolved the handler for MorphOS (v46) and AmigaOS 4 (v50), including DOS6/DOS7 long-name modes. These are OFS and FFS format variants, not a new unrelated on-disk filesystem. Hyperion's AmigaOS 3.1.4 (September 2018) shipped v46 with multiple large-device interfaces; 3.1.4.1 (July 2019) updated the disk components. AmigaOS 3.2 (May 2021) shipped the v47 generation with further refinements. Version numbers are handler lineage, not the fourth DOSType byte; versions from distinct branches are not a single monotonically ordered release train.

## 2. Format identifiers and compatibility

| Big-endian ID | Binary four-byte name | Format | Variant rules |
| --- | --- | --- | --- |
| `0x444F5301` | `DOS\1` | Original Fast File System | Classic 30-byte filename metadata; FFS raw data blocks |
| `0x444F5303` | `DOS\3` | International FFS | International Latin-1 folding; classic 30-byte metadata |
| `0x444F5305` | `DOS\5` | Directory-cache FFS | International folding; classic directory metadata plus cache blocks |
| `0x444F5307` | `DOS\7` | Long-name FFS (LNFS) | International folding; separate NaC name/comment and type-64 overflow comment format |

The four-byte DOSType consists of the three ASCII bytes `DOS` plus a **binary** selector byte. All FFS identifiers are odd-numbered; all OFS identifiers are specified in the OFS document and must **not** be reinterpreted as FFS.

International mode changes equivalence/hashing for accented Latin-1 letters, not the raw character encoding; pre-international handlers could already store high-byte characters but did not consistently fold them. DOS2/3 are not backward-compatible with purely original handlers. DOS4/5 need directory-cache-capable handlers, and DOS6/7 cannot be mounted by older handlers that lack LNFS layout knowledge. Historical compatibility refers to the handler, ROM, and boot path, not just software version. A later handler can read earlier supported types, but the reverse is not guaranteed. DOS0 remained common on commercial floppy games and magazine disks for pre-2.x compatibility; trackloaders may bypass the filesystem entirely.

## 3. Geometry, address limits and device integration

All multi-byte on-disk numeric fields are big-endian. For logical filesystem block size B (512, 1024, 2048, 4096, 8192, 16384, 32768), the classic hash/pointer table contains H=B/4-56 longwords and bitmap pages describe Q=8B-32 blocks. The first two logical blocks normally form the boot area. The four-byte DOSType, boot checksum and root-key field are distinct from metadata block checksums. Root placement must be validated against reserved-block and volume geometry; do not guess from a DOS signature alone. Block keys are unsigned BE32 and zero is null. Ordinary FFS data has B bytes of payload; a file spanning H pointers requires T_LIST=16 extension headers.

Classic trackdisk CMD_READ/CMD_WRITE use 32-bit byte offsets, creating a 4 GiB (2^32 bytes) device-address boundary. Some old handlers, tools and signed arithmetic imposed a 2 GiB boundary; traditional AmigaDOS APIs also exposed a roughly 2 GiB signed file-size limit. The historically reported 127 GB partition ceiling depended on handler, DOS library, geometry and tooling, and is not a universal property of every DOS type. Old code may wrap byte addresses and overwrite earlier storage rather than fail. Never treat a legacy 4 GB figure as the canonical on-disk volume limit.

TrackDisk64 (TD64) third-party extensions used 64-bit device offsets, often with v44 handler patches and Phase5/controller packages. New Style Device (NSD) provided a distinct 64-bit command convention in the v45 licensed AmigaOS 3.5/3.9 FFS line. The schemes were not automatically interchangeable; a driver, filesystem and repair tool must agree on the interface or have a suitable translation layer. Hyperion v46 supports TD64, NSD and classic commands (and direct SCSI where appropriate); large-media operation still requires a compatible controller and correct RDB geometry. Later handlers can support partitions beyond 2 GiB and media beyond 4 GiB; do not infer that every old utility can.

On disk, unsigned 32-bit block keys and 32-bit file lengths are separate from host byte offsets. For B=512, 32-bit block addressing is just below 2 TiB; for B=32768 it is just below 128 TiB. These are arithmetic bounds, not historical or tested volume limits. A UINT32_MAX byte-length field is not a promise that a classic AmigaDOS application supports a 4 GiB file. Modern providers must use overflow-checked partition-relative arithmetic, test large offsets and distinguish format limits from provider and device limits.

### Boot and RDB

Hard disks can carry a Rigid Disk Block (RDB) with partition records (PART), filesystem headers (FSHD) and load segments (LSEG). The partition environment supplies DOSType, block geometry and boot priority; a suitable controller/driver can load a filesystem handler from the RDB before mounting it. Kickstart 1.3 added autoboot support for hard disks and reset-resident RAD: RAM disks. Workbench 1.3 supplied the disk-based FFS handler for RDB installation. Kickstart 1.2 could boot 1.3 from floppy but lacked native FFS hard-disk autoboot. Kickstart 2.x incorporated FFS in ROM; RDB handler loading remained available for replacements and third-party formats.

Unpartitioned trackdisk floppies have no RDB and rely on a ROM-supported DOSType for normal boot. FFS-formatted floppies generally require Kickstart 2.0 for native boot; pre-2.x access could be arranged through installed handlers or unsupported mount workarounds. Directory-cache variants require the AmigaOS 3.0-era handler. DOS types can also be selected via mountlists/DOSDrivers. Application compatibility normally follows AmigaDOS calls; raw trackloaders are outside filesystem semantics.

The project's Common V2 host delivers a bounded, partition-relative volume. RDB partition selection and executable Amiga handler loading are outside that filesystem ABI; a historical bootable disk is not proof of InfiltratorOS BOOT compatibility. GPT/MBR, RDB and floppy/hardfile geometry must not be conflated. Never execute an untrusted handler from media. Native host read/write offsets are checked wide byte offsets.

## 4. Root, directory and file metadata

Metadata blocks use additive big-endian longword checksums; the boot area uses the historical end-around-carry checksum. Root, user directories and file headers use primary T_HEADER=2; root and directory secondary types distinguish their roles, and files use the file secondary type. Root owns the bitmap-valid flag, bitmap page references, extension pointer, volume name and hash table. File headers carry parent, next hash entry, data pointers, extension key, logical length, comment, protection and DateStamp. File pointer order is reversed in classic pointer tables; check sequence and bounds before reading. User-directory reserved fields must not be interpreted as root hash-table size. A valid parser verifies ownership, type, checksum, parent and hash-chain invariants and prevents cycles and duplicate block references.

Refer to the [OFS structure specification](https://github.com/Infiltrator-Projects/Filesystem-OFS/blob/main/native/filesystems/ofs/OFS_SPECIFICATION.md) for shared classic root, header, bitmap and directory field tables. The FFS raw-data distinction and odd DOSType profiles override OFS payload-specific rules.

## 5. File payload and extension mapping

- Root and directory checksum, hash bucket and bitmap layouts are related to OFS, but FFS's **regular-file data blocks are raw payload bytes**. Do **not** place the OFS 24-byte header, OFS data-block checksum, sequence number or next-data pointer inside an FFS data block.
- Files have a `T_HEADER=2` file metadata block; their data-block pointers are held in the header's reversed pointer table, with `T_LIST=16` extension metadata when more pointer entries are needed. The payload per FFS data block is the **entire filesystem block** (`B` bytes), not `B-24`.

### Worked file mapping

For B=512, H=72 pointer slots in the header or extension and raw payload P=512. A 40000-byte file needs ceil(40000/512)=79 data blocks: 72 in the header and seven in an extension. Logical byte 38000 selects logical data block 74 with offset 112; extension 1, pointer index 2, byte offset B-204-4*2=300. The referenced raw data block is read beginning at byte 112, without any OFS header. The final block holds 64 visible bytes. Metadata headers and extension blocks use native checksums, but FFS data blocks are not individually protected by OFS data checksums. Reconstruct logical order from the reverse tables and extension chain.

## 6. Directory lookup, allocation and recovery

Allocation must preserve the distinction between reserved, allocated and free keys, including bitmap pages and bitmap-extension blocks. Mutation requires exclusive, durable access; mark the bitmap invalid before changes, flush data and metadata in a justified order, update bitmap and mark valid only after consistency. A checker must enumerate reachable ownership and report collisions, invalid references or structural contradictions; recovery cannot invent lost directory links. No generic journal or atomic multi-block operation is implied.

International case conversion maps ASCII a-z and Latin-1 lowercase E0-FE except F7 to uppercase. Hash and equality must agree; reject illegal delimiters, malformed lengths and duplicate names.

Historic disk-validator operation can make volumes read-only while validation runs; scans and access slowdowns grew severe on larger disks and could last hours. If the validator fails, recovery may require a salvage tool or copying recoverable files to another volume. Earlier boot disks supplied a disk-resident validator; malware such as the historically named Saddam Hussein virus exploited replaceable validator code. ROM integration in Kickstart 2.x reduced that replacement vector. These historical details are context, not a guarantee against all malware.

Commodore supplied DiskDoctor in earlier releases and later removed it from the standard disks; DiskSalv was a prominent third-party repair, undelete and quick-format recovery utility. Defragmentation and conversion require exclusive ownership or a locked volume; normal access is restricted while a maintenance tool relocates blocks. Tools with legacy device interfaces can misaddress storage beyond their safe boundary and cause damage. Do not equate a successful bitmap rebuild with file undelete, unformat, or repair of contradictory metadata.

### Worked allocation and collision exercise

For B=512, reserved count 2 and 1760 blocks, root is floor((2+1760-1)/2)=880. Bitmap blocks have 4064 usable bits after their checksum. Check native allocation bitmap ownership for header, extension and all data blocks. Directory entries use name hash chains and native case-folding; collisions need full-name comparison. DOS5 cache blocks must match authoritative entries, while DOS7 uses NaC fields and comment overflow, not a directory cache. A relocation must copy and verify raw payload, update the exact pointer slot, and release source only after safe publication; metadata checksums do not make arbitrary multi-block updates atomic. Test all four profiles against independently formatted images, including long names, extensions, comments and interrupted mutations.

## 7. Directory cache and long-name variants

- Directory enumeration and lookup use the correct DOSType-specific case folding. On international profiles, hash and equality comparisons must consistently perform the Latin-1 fold. A directory-cache writer must maintain valid cache records; read-only parsers may fall back to authoritative directory hash chains after compatibility checks.
- For `DOS\7`, all non-root entry headers must use the authentic LNFS 112-byte `NaC` field at `B-184` with name and comment as two adjacent BCPL strings; overflow comment pointer at `B-72`; shifted DateStamp at `B-60`; a type-64 comment block when necessary. The root volume name remains 30 bytes; the root repeats its DOSType at `B-16` and has an allocated-block counter at `B-44`. LNFS uses international folding, **not** directory caching. Hyperion's format guidance specifies a conservative 106-character writer limit.

DCFS stores auxiliary directory-list records in addition to authoritative hashed directory entries. It reduces scattered metadata reads and can speed directory enumeration, not ordinary file payload reading. Create, delete, rename and metadata updates must also update cache records, adding writes and consistency obligations. Olaf Barthel's documented limitations include lack of consolidation/garbage collection for partly filled cache blocks: partially used pages may persist and waste space. Floppy directories may benefit from lower seek cost but the cache also consumes scarce floppy space; larger hard disks can see net degradation from cache maintenance. Do not infer all implementations exhibit identical performance.

LNFS substitutes long-name/comment metadata and a root used-block counter/type echo, not DCFS. It retains bitmap validation and does not add transactional crash rollback. FFS2 does not remove the underlying validation requirement. Both original formats have Amiga protection flags, timestamps and file comments (filenotes); neither provides transparent compression or encryption as an original format feature. AmigaOS, MorphOS and AmigaOS 4 have supported FFS; SFS and PFS emerged as alternatives in the mid-1990s, with PFS3 and SFS2 having distinct on-media semantics. SFS has been used as a default filesystem in AROS distributions; defaults can vary by distribution/version.

DOS5 cache records require consistent ownership, names, size, dates, protection and comments. DOS7 uses the authentic NaC layout; never substitute a relabelled DOS3 image.

## 8. Provider conformance and qualification

A correct FFS implementation verifies and preserves metadata and allocation checksums, root discovery and bitmap accounting; supports files crossing pointer-extension boundaries; reads/writes the full `B`-byte payload; handles directory hash collisions, rename and deletion; and rejects damaged or foreign media without reinterpreting it.

Before adding writable `DOS\7` support, test NaC names at 30, 31 and 106 characters, comment overflow allocation/free, Latin-1 hash chains, root signature and used-block counter updates, and actual reader/writer interoperability. As with OFS, a successful build, trivial empty image or passing self-verifier does not alone establish historic AmigaOS compatibility.

At the documented source baseline 51ae5996ff53, Common V2 source in module/ffs_common.c accepts only DOS1, DOS3 and DOS5 in detect(); DOS7 is rejected. The formatter accepts DOS1 (or default selector 0), 512-byte logical sectors and volumes from 1024 to 4194304 blocks (512 KiB to 2 GiB). Creation and growth are restricted to DOS1 and the formatter's supported geometry. DOS3/DOS5 overwrites are limited to existing file extents; do not advertise general namespace/cache mutation. Core, volume and defrag-map groups are exported; mapping is not relocation. Format-level support and provider operation support are not equivalent.

Required qualification before expanding claims: independent DOS1/3/5/7 media, seven block sizes and overflow-safe high addresses; checksum and malformed media rejection; directory collisions and Latin-1 equivalence; create/rename/delete/truncate and extension-chain rollover; DOS5 cache coherency and page cleanup; DOS7 names 30, 31, 106 and read-only 107 where supported, comment overflow, root count and type echo; dirty-bitmap recovery and injected write/flush failures; hosted and firmware profiles; destructive operations only on disposable or explicitly authorised media. Do not silently use kernel VFS, FUSE or a legacy test interface to complete absent Common V2 functionality.

Current availability must be checked against the exported descriptor, operation masks and exact-revision evidence.

Linux's AFFS driver is an independent implementation, not an authority for our exported capabilities. Its supported DOSTypes and read/write limitations vary by version; historical descriptions claiming universal DOS0–5 formatting or writable DCFS overstate what a kernel driver guarantees. Formatting is normally a separate user-space tool. The Amiga xfs freeware utility could access older OFS/FFS among other formats, but predates LNFS and did not provide general formatting. ADFlib, amitools and independent Amiga media are valuable interoperability references; third-party compatibility must be demonstrated rather than assumed.

## 9. Original sources and historical references

**Original sources:** [AmigaDOS ROM Kernel Reference Manual (DOS)](https://developer.amigaos3.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf), [Olaf Barthel's official DCFS/LNFS on-disk structures](https://wiki.amigaos.net/wiki/DCFS_and_LNFS_Low_Level_Data_Structures), [ADFlib disk-format description](https://adflib.github.io/FAQ/adf_info.html), [Linux AFFS documentation](https://www.kernel.org/doc/html/v6.7/filesystems/affs.html), and [Hyperion's long-filename formatter FAQ](https://www.hyperion-entertainment.com/index.php/frequently-asked-questions/55-amigaos-314/204-long-filenames-i-was-promised-long-filenames-how-do-i-use-them).

Historical comparison: Amiga Fast File System article (user-supplied 2026-11-25), https://en.wikipedia.org/wiki/Amiga_Fast_File_System . The article is a secondary source; its unsourced characteristics and broad compatibility statements are not normative. Consult original manuals and handler-version documentation for device-specific details.

Hyperion large-media FAQ https://www.hyperion-entertainment.com/index.php/frequently-asked-questions/55-amigaos-314/202-can-i-use-partitions-beyond-the-4gb-boundary ; Hyperion 3.1.4.1 release notice https://www.hyperion-entertainment.com/index.php/news/1-latest-news/280-update-to-amigaos-314-released ; AmigaOS 3.2 product documentation https://www.hyperion-entertainment.com/index.php/where-to-buy/direct-downloads/327-amigaos-32 . Historical chronology: https://www.gregdonner.org/workbench/ ; Thomas Rapp, FAQ about the 4GB problem, https://thomas-rapp.hier-im-netz.de/4gb.html .
