/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_FFS_DROPIN_MODULE_H
#define INFILTRATOR_FFS_DROPIN_MODULE_H
#include <stdint.h>
#define IFS_FFS_DROPIN_ABI_VERSION 1u
#define IFS_FFS_CAP_PROBE UINT64_C(1)
#define IFS_FFS_CAP_LOOKUP UINT64_C(2)
#define IFS_FFS_CAP_ENUMERATE UINT64_C(4)
#define IFS_FFS_CAP_FILE_READ UINT64_C(8)
#define IFS_FFS_CAP_DOS1 UINT64_C(16)
#define IFS_FFS_CAP_DOS3 UINT64_C(32)
#define IFS_FFS_CAP_DOS5 UINT64_C(64)
#define IFS_FFS_CAP_BLOCK_512_TO_32768 UINT64_C(128)
#define IFS_FFS_MAX_DIRECTORY_ENTRIES 4096u
typedef int (*IfsFfsReadAt)(void *context, uint64_t offset,
                             void *buffer, uint32_t bytes);
typedef struct {
    IfsFfsReadAt read_at;
    void *context;
    uint64_t volume_bytes; /* Partition-relative, never a whole disk. */
    uint32_t block_bytes;  /* 512..32768 power-of-two filesystem blocks. */
} IfsFfsDropInIo;
typedef struct {
    IfsFfsDropInIo io;
    uint32_t blocks;
    uint32_t root_block;
    uint32_t hash_size;
    uint32_t variant_flags;
    uint32_t mounted;
} IfsFfsDropInVolume;
enum { IFS_FFS_FILE = 1, IFS_FFS_DIRECTORY = 2 };
typedef struct {
    uint32_t block;
    uint32_t size;
    uint32_t type;
} IfsFfsDropInEntry;
typedef struct {
    uint32_t bucket;
    uint32_t next_block;
    uint32_t visited;
} IfsFfsDropInDirCursor;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    const char *format_name;
    uint64_t capabilities;
    int (*mount)(IfsFfsDropInVolume *, const IfsFfsDropInIo *);
    IfsFfsDropInEntry (*root)(const IfsFfsDropInVolume *);
    int (*lookup)(const IfsFfsDropInVolume *, const IfsFfsDropInEntry *,
                  const uint8_t *name, uint32_t name_length,
                  IfsFfsDropInEntry *);
    int (*next)(const IfsFfsDropInVolume *, const IfsFfsDropInEntry *,
                IfsFfsDropInDirCursor *, IfsFfsDropInEntry *, uint8_t name[31]);
    int (*read)(const IfsFfsDropInVolume *, const IfsFfsDropInEntry *,
                uint32_t position, void *buffer, uint32_t length);
} IfsFfsDropInModule;
const IfsFfsDropInModule *infiltrator_filesystem_module_v1(void);

/* Optional, separately discoverable V2. Existing V1 layout is immutable.
 * Overwrite allocated bytes of an existing regular file only. The host must
 * own an exclusive unmounted volume lease, supply flush, and accept that an
 * I/O failure may leave partial data; NO allocation or recovery is provided.
 */
#define IFS_FFS_DROPIN_V2_ABI_VERSION 2u
#define IFS_FFS_V2_CAP_OVERWRITE_EXISTING UINT64_C(256)
typedef int (*IfsFfsWriteAt)(void *context, uint64_t offset,
                             const void *source, uint32_t bytes);
typedef int (*IfsFfsFlush)(void *context);
typedef struct {
    IfsFfsDropInIo read;
    IfsFfsWriteAt write_at;
    IfsFfsFlush flush;
} IfsFfsWritableIo;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    const IfsFfsDropInModule *readonly_v1;
    int (*overwrite_existing)(const IfsFfsDropInVolume *,
                              const IfsFfsDropInEntry *,
                              const IfsFfsWritableIo *,
                              uint32_t position,
                              const void *source, uint32_t length);
} IfsFfsDropInModuleV2;
const IfsFfsDropInModuleV2 *infiltrator_filesystem_module_v2(void);
#endif
