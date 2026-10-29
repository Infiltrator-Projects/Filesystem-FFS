/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent read-only FFS DOS1/3/5 adapter: real FFS checksum, hash,
 * extension pointers and raw (not OFS-headered) data blocks. */
#include "ffs_module.h"
#include "../core/ffs_core.h"
#include "../core/ffs_primitives.h"

#define FFS_META 2u
#define FFS_LIST 16u
#define FFS_FILE ((uint32_t)-3)
#define FFS_DIR 2u
#define FFS_ROOT 1u

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t word(const uint8_t *p, uint32_t index)
{
    return be32(p + index * 4u);
}
static int block_valid(const IfsFfsDropInVolume *v, uint32_t block)
{
    return v && v->mounted && block >= 2u && block < v->blocks;
}
static int get(const IfsFfsDropInVolume *v, uint32_t block, uint8_t *buffer)
{
    uint64_t offset;
    if (!block_valid(v, block) || !buffer || !v->io.read_at)
        return -1;
    offset = (uint64_t)block * v->io.block_bytes;
    if (offset > v->io.volume_bytes ||
        v->io.block_bytes > v->io.volume_bytes - offset)
        return -1;
    return v->io.read_at(v->io.context, offset, buffer,
                         v->io.block_bytes) == 0 ? 0 : -1;
}
static int metadata(const IfsFfsDropInVolume *v, uint32_t block,
                    uint8_t *bytes, uint32_t type)
{
    return get(v, block, bytes) == 0 && word(bytes, 0u) == type &&
        ifs_ffs_block_checksum(bytes, v->io.block_bytes) == 0u ? 0 : -1;
}
static int directory(const IfsFfsDropInVolume *v,
                     const IfsFfsDropInEntry *dir, uint8_t *bytes)
{
    const uint32_t words = v ? v->io.block_bytes / 4u : 0u;
    if (!dir || dir->type != IFS_FFS_DIRECTORY ||
        metadata(v, dir->block, bytes, FFS_META) != 0)
        return -1;
    if (dir->block == v->root_block)
        return word(bytes, words - 1u) == FFS_ROOT &&
               word(bytes, 3u) == v->hash_size ? 0 : -1;
    return word(bytes, 1u) == dir->block &&
           word(bytes, words - 1u) == FFS_DIR ? 0 : -1;
}
static int entry(const IfsFfsDropInVolume *v, uint32_t block,
                 uint32_t parent, uint8_t *bytes, IfsFfsDropInEntry *out)
{
    const uint32_t words = v->io.block_bytes / 4u;
    uint32_t type;
    if (!out || metadata(v, block, bytes, FFS_META) != 0 ||
        word(bytes, 1u) != block || word(bytes, words - 3u) != parent ||
        bytes[v->io.block_bytes - 80u] == 0u ||
        bytes[v->io.block_bytes - 80u] > 30u ||
        ifs_ffs_validate_name(bytes + v->io.block_bytes - 79u,
            bytes[v->io.block_bytes - 80u], 1) != IFS_FFS_NAME_OK)
        return -1;
    type = word(bytes, words - 1u);
    if (type != FFS_FILE && type != FFS_DIR)
        return -1; /* No symlinks, DCFS cache or special records. */
    out->block = block;
    out->type = type == FFS_FILE ? IFS_FFS_FILE : IFS_FFS_DIRECTORY;
    out->size = type == FFS_FILE ? word(bytes, words - 47u) : 0u;
    return 0;
}
static int name_equals(const IfsFfsDropInVolume *v,
                       const uint8_t *bytes, const uint8_t *name,
                       uint32_t length)
{
    const uint32_t base = v->io.block_bytes - 80u;
    const int intl = (v->variant_flags & IFS_FFS_VARIANT_INTL) != 0u;
    uint32_t i;
    if (bytes[base] != length) return 0;
    for (i = 0u; i < length; ++i)
        if (ifs_ffs_fold_character(bytes[base + 1u + i], intl) !=
            ifs_ffs_fold_character(name[i], intl)) return 0;
    return 1;
}
static int mount_volume(IfsFfsDropInVolume *v, const IfsFfsDropInIo *io)
{
    uint8_t bytes[32768];
    uint32_t flags, root, blocks;
    if (!v || !io || !io->read_at || io->block_bytes < 512u ||
        io->block_bytes > 32768u || (io->block_bytes & (io->block_bytes - 1u)) ||
        io->volume_bytes % io->block_bytes ||
        io->volume_bytes / io->block_bytes > UINT32_MAX ||
        io->volume_bytes / io->block_bytes < 16u)
        return -1;
    v->mounted = 0u;
    blocks = (uint32_t)(io->volume_bytes / io->block_bytes);
    v->io = *io;
    v->blocks = blocks;
    v->hash_size = io->block_bytes / 4u - 56u;
    root = (uint32_t)((2u + (uint64_t)blocks - 1u) / 2u);
    if (io->read_at(io->context, 0u, bytes, 4u) != 0 ||
        ifs_ffs_classify_dostype(be32(bytes), &flags) != 0 ||
        (flags & IFS_FFS_VARIANT_MUFS) != 0u)
        return -1;
    v->variant_flags = flags;
    v->mounted = 1u;
    /* Linux AFFS checks the midpoint and its immediate successor. */
    for (uint32_t candidate = root; candidate <= root + 1u; ++candidate) {
        v->root_block = candidate;
        if (block_valid(v, candidate) &&
            metadata(v, candidate, bytes, FFS_META) == 0 &&
            word(bytes, io->block_bytes / 4u - 1u) == FFS_ROOT &&
            word(bytes, 3u) == v->hash_size)
            return 0;
    }
    v->mounted = 0u;
    return -1;
}
static IfsFfsDropInEntry module_root(const IfsFfsDropInVolume *v)
{
    IfsFfsDropInEntry e = {0u, 0u, IFS_FFS_DIRECTORY};
    if (v && v->mounted) e.block = v->root_block;
    return e;
}
static int module_lookup(const IfsFfsDropInVolume *v,
                         const IfsFfsDropInEntry *dir,
                         const uint8_t *name, uint32_t length,
                         IfsFfsDropInEntry *out)
{
    uint8_t bytes[32768];
    uint32_t bucket, block, visited = 0u;
    if (!v || !v->mounted || !name || !out || length == 0u ||
        length > 30u || ifs_ffs_validate_name(name, length, 1) != IFS_FFS_NAME_OK ||
        directory(v, dir, bytes) != 0) return -1;
    bucket = ifs_ffs_directory_hash(name, length, v->hash_size,
                (v->variant_flags & IFS_FFS_VARIANT_INTL) != 0u);
    block = word(bytes, 6u + bucket);
    while (block) {
        if (++visited > v->blocks || visited > IFS_FFS_MAX_DIRECTORY_ENTRIES ||
            entry(v, block, dir->block, bytes, out) != 0 ||
            ifs_ffs_directory_hash(bytes + v->io.block_bytes - 79u,
                bytes[v->io.block_bytes - 80u], v->hash_size,
                (v->variant_flags & IFS_FFS_VARIANT_INTL) != 0u) != bucket)
            return -1;
        if (name_equals(v, bytes, name, length)) return 0;
        block = word(bytes, v->io.block_bytes / 4u - 4u);
    }
    return 1;
}
static int module_next(const IfsFfsDropInVolume *v,
                       const IfsFfsDropInEntry *dir, IfsFfsDropInDirCursor *cursor,
                       IfsFfsDropInEntry *out, uint8_t name[31])
{
    uint8_t bytes[32768], dir_bytes[32768];
    uint32_t block, size;
    if (!v || !v->mounted || !cursor || !out || !name ||
        cursor->bucket > v->hash_size ||
        cursor->visited > v->blocks ||
        cursor->visited > IFS_FFS_MAX_DIRECTORY_ENTRIES ||
        directory(v, dir, dir_bytes) != 0) return -1;
    while (cursor->bucket < v->hash_size) {
        block = cursor->next_block ? cursor->next_block :
            word(dir_bytes, 6u + cursor->bucket);
        if (!block) {
            ++cursor->bucket;
            cursor->next_block = 0u;
            continue;
        }
        if (++cursor->visited > v->blocks ||
            cursor->visited > IFS_FFS_MAX_DIRECTORY_ENTRIES ||
            entry(v, block, dir->block, bytes, out) != 0 ||
            ifs_ffs_directory_hash(bytes + v->io.block_bytes - 79u,
                bytes[v->io.block_bytes - 80u], v->hash_size,
                (v->variant_flags & IFS_FFS_VARIANT_INTL) != 0u) != cursor->bucket)
            return -1;
        size = bytes[v->io.block_bytes - 80u];
        for (uint32_t i = 0u; i < size; ++i)
            name[i] = bytes[v->io.block_bytes - 79u + i];
        name[size] = 0u;
        cursor->next_block = word(bytes, v->io.block_bytes / 4u - 4u);
        if (!cursor->next_block) ++cursor->bucket;
        return 0;
    }
    return 1;
}
static int map_data(const IfsFfsDropInVolume *v, uint32_t file_block,
                    uint32_t logical, uint32_t *physical)
{
    uint8_t bytes[32768];
    uint32_t extension = file_block, per = v->hash_size;
    uint32_t ext_index = logical / per, at = logical % per;
    if (!physical || !block_valid(v, file_block) || ext_index >= v->blocks)
        return -1;
    for (uint32_t i = 0u; i <= ext_index; ++i) {
        if (metadata(v, extension, bytes, i == 0u ? FFS_META : FFS_LIST) != 0 ||
            word(bytes, 1u) != extension ||
            word(bytes, v->io.block_bytes / 4u - 1u) != FFS_FILE ||
            word(bytes, 2u) > per ||
            (i > 0u && word(bytes, v->io.block_bytes / 4u - 3u) != file_block) ||
            (word(bytes, 2u) == 0u && word(bytes, 4u) != 0u) ||
            (word(bytes, 2u) && word(bytes, 4u) !=
             word(bytes, 6u + per - 1u))) return -1;
        if (i != ext_index) {
            extension = word(bytes, v->io.block_bytes / 4u - 2u);
            if (!block_valid(v, extension) || extension == file_block)
                return -1;
        }
    }
    if (at >= word(bytes, 2u)) return -1;
    *physical = word(bytes, 6u + per - 1u - at);
    return block_valid(v, *physical) && *physical != file_block &&
           *physical != v->root_block ? 0 : -1;
}
static int module_read(const IfsFfsDropInVolume *v,
                       const IfsFfsDropInEntry *file,
                       uint32_t position, void *output, uint32_t length)
{
    uint8_t header[32768];
    uint8_t *dest = (uint8_t *)output;
    uint32_t size, block, skip, within;
    if (!v || !v->mounted || !file || file->type != IFS_FFS_FILE ||
        (!output && length) ||
        metadata(v, file->block, header, FFS_META) != 0 ||
        word(header, 1u) != file->block ||
        word(header, v->io.block_bytes / 4u - 1u) != FFS_FILE)
        return -1;
    size = word(header, v->io.block_bytes / 4u - 47u);
    if (size != file->size || position > size || length > size - position)
        return -1;
    skip = position / v->io.block_bytes;
    within = position % v->io.block_bytes;
    while (length) {
        uint32_t count = v->io.block_bytes - within;
        if (count > length) count = length;
        if (map_data(v, file->block, skip, &block) != 0 ||
            v->io.read_at(v->io.context,
                (uint64_t)block * v->io.block_bytes + within,
                dest, count) != 0) return -1;
        dest += count;
        length -= count;
        within = 0u;
        ++skip;
    }
    return 0;
}
__attribute__((used,section(".ifsmeta")))
static const char format_identity[] = "IFS-MODULE:ffs:1";
static const IfsFfsDropInModule module = {
    IFS_FFS_DROPIN_ABI_VERSION, sizeof(IfsFfsDropInModule),
    "ffs", "Amiga Fast File System",
    IFS_FFS_CAP_PROBE | IFS_FFS_CAP_LOOKUP | IFS_FFS_CAP_ENUMERATE |
    IFS_FFS_CAP_FILE_READ | IFS_FFS_CAP_DOS1 | IFS_FFS_CAP_DOS3 |
    IFS_FFS_CAP_DOS5 | IFS_FFS_CAP_BLOCK_512_TO_32768,
    mount_volume, module_root, module_lookup, module_next, module_read
};
__attribute__((visibility("default")))
const IfsFfsDropInModule *infiltrator_filesystem_module_v1(void)
{
    return &module;
}

/* V2 writes only unchanged file data bytes, with the full requested
 * block-pointer range validated BEFORE the first write. Source metadata,
 * allocation and directory records must never be rewritten by this API.
 * The host must hold the volume exclusively against concurrent mutation.
 */
static int overwrite_existing(const IfsFfsDropInVolume *volume,
                              const IfsFfsDropInEntry *file,
                              const IfsFfsWritableIo *io,
                              uint32_t position,
                              const void *source, uint32_t length)
{
    IfsFfsDropInVolume live;
    const uint8_t *input = (const uint8_t *)source;
    uint32_t offset_in_file, remaining;
    uint32_t block;
    if (!volume || !volume->mounted || !file || !io ||
        !io->read.read_at || !io->write_at || !io->flush ||
        (!source && length != 0u) ||
        io->read.read_at != volume->io.read_at ||
        io->read.context != volume->io.context ||
        io->read.volume_bytes != volume->io.volume_bytes ||
        io->read.block_bytes != volume->io.block_bytes ||
        mount_volume(&live, &io->read) != 0 ||
        live.root_block != volume->root_block ||
        live.variant_flags != volume->variant_flags ||
        live.blocks != volume->blocks ||
        module_read(&live, file, position, 0, 0u) != 0 ||
        length > file->size - position)
        return -1;
    if (!length) return 0;

    /* All blocks in the requested range must resolve cleanly before writes.
     * A write callback error can still leave earlier data committed. */
    offset_in_file = position;
    remaining = length;
    while (remaining) {
        const uint32_t within = offset_in_file % live.io.block_bytes;
        uint32_t count = live.io.block_bytes - within;
        if (count > remaining) count = remaining;
        if (map_data(&live, file->block,
                     offset_in_file / live.io.block_bytes, &block) != 0)
            return -1;
        offset_in_file += count;
        remaining -= count;
    }

    offset_in_file = position;
    remaining = length;
    while (remaining) {
        const uint32_t within = offset_in_file % live.io.block_bytes;
        uint32_t count = live.io.block_bytes - within;
        uint64_t physical_offset;
        if (count > remaining) count = remaining;
        if (map_data(&live, file->block,
                     offset_in_file / live.io.block_bytes, &block) != 0)
            return -1;
        physical_offset = (uint64_t)block * live.io.block_bytes + within;
        if (physical_offset > live.io.volume_bytes ||
            count > live.io.volume_bytes - physical_offset ||
            io->write_at(io->read.context, physical_offset, input, count) != 0)
            return -1;
        input += count;
        offset_in_file += count;
        remaining -= count;
    }
    return io->flush(io->read.context) == 0 ? 0 : -1;
}

__attribute__((used, section(".ifsmeta")))
static const char format_identity_v2[] = "IFS-MODULE:ffs:2";
static const IfsFfsDropInModuleV2 module_v2 = {
    IFS_FFS_DROPIN_V2_ABI_VERSION, sizeof(IfsFfsDropInModuleV2),
    "ffs", IFS_FFS_V2_CAP_OVERWRITE_EXISTING, &module, overwrite_existing
};
__attribute__((visibility("default")))
const IfsFfsDropInModuleV2 *infiltrator_filesystem_module_v2(void)
{
    return &module_v2;
}
