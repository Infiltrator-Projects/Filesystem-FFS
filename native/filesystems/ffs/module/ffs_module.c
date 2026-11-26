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
/* The volume label lives in the checksum-protected root block, not in
 * the boot block or a synthetic path name. Reuse the native root validator. */
int ifs_ffs_volume_label(const IfsFfsDropInVolume *v,uint8_t name[30],uint32_t *length)
{
    uint8_t bytes[32768];
    if (!v || !name || !length || !v->mounted) return -1;
    *length=0;
    IfsFfsDropInEntry root={v->root_block,0u,IFS_FFS_DIRECTORY};
    if (directory(v,&root,bytes)!=0) return -1;
    uint32_t base=v->io.block_bytes-80u,n=bytes[base];
    if (n>30u || (n && ifs_ffs_validate_name(bytes+base+1u,n,1)!=IFS_FFS_NAME_OK))
        return -1;
    for (uint32_t i=0;i<n;++i) name[i]=bytes[base+1u+i];
    *length=n;
    return 0;
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
/* A header and its T_LIST continuation blocks store data pointers in reverse
 * word order. The count and the file's byte length together bound the chain.
 * The legacy first-data hint is meaningful in the header. Real FFS list
 * blocks leave it zero, while some producers repeat the first entry there. */
typedef struct {
    uint8_t raw[32768];
    uint32_t page, index, pages, data_blocks, anchor, power, distance;
} PointerPage;

static int load_page(const IfsFfsDropInVolume *v, uint32_t file, PointerPage *p)
{
    const uint32_t words = v->io.block_bytes / 4;
    uint32_t count = p->data_blocks - p->index * v->hash_size;
    if (count > v->hash_size) count = v->hash_size;
    if (metadata(v, p->page, p->raw, p->index ? FFS_LIST : FFS_META) ||
        word(p->raw, 1) != p->page || word(p->raw, 2) != count ||
        word(p->raw, words - 1) != FFS_FILE ||
        (p->index && word(p->raw, words - 3) != file)) return -1;
    const uint32_t hint = word(p->raw, 4);
    if (hint && (!count || hint != word(p->raw, 5 + v->hash_size))) return -1;
    const uint32_t next = word(p->raw, words - 2);
    if (p->index + 1 == p->pages) return next ? -1 : 0;
    return block_valid(v, next) && next != p->page && next != file && next != v->root_block ? 0 : -1;
}

static int first_page(const IfsFfsDropInVolume *v, const IfsFfsDropInEntry *file, PointerPage *p)
{
    if (!v || !v->mounted || !v->hash_size || !file || file->type != IFS_FFS_FILE)
        return -1;
    p->page = p->anchor = file->block;
    p->index = p->distance = 0;
    p->power = 1;
    p->data_blocks = (uint32_t)(((uint64_t)file->size + v->io.block_bytes - 1) / v->io.block_bytes);
    p->pages = p->data_blocks ? 1 + (p->data_blocks - 1) / v->hash_size : 1;
    if (p->data_blocks >= v->blocks || load_page(v, file->block, p)) return -1;
    return word(p->raw, v->io.block_bytes / 4 - 47) == file->size ? 0 : -1;
}

static int data_pointer(const IfsFfsDropInVolume *v, uint32_t file, PointerPage *p,
                        uint32_t logical, uint32_t *physical)
{
    if (logical >= p->data_blocks) return -1;
    const uint32_t wanted = logical / v->hash_size;
    while (p->index < wanted) {
        uint32_t next = word(p->raw, v->io.block_bytes / 4 - 2);
        /* Brent's cycle check uses constant memory on the firmware stack. */
        if (next == p->anchor) return -1;
        if (++p->distance == p->power) {
            p->anchor = next;
            p->distance = 0;
            if (p->power <= UINT32_MAX / 2) p->power *= 2;
        }
        p->page = next;
        ++p->index;
        if (load_page(v, file, p)) return -1;
    }
    *physical = word(p->raw, 5 + v->hash_size - logical % v->hash_size);
    return block_valid(v, *physical) && *physical != file && *physical != p->page &&
           *physical != v->root_block ? 0 : -1;
}

int ifs_ffs_data_block(const IfsFfsDropInVolume *v, const IfsFfsDropInEntry *file,
                       uint32_t logical, uint32_t *physical)
{
    PointerPage p;
    if (!physical || first_page(v, file, &p)) return -1;
    return data_pointer(v, file->block, &p, logical, physical);
}

/* Resolve consecutive logical blocks using one validated native cursor.
 * Private engine helper; the external ABI remains exclusively Common V2. */
int ifs_ffs_data_blocks(const IfsFfsDropInVolume *v,const IfsFfsDropInEntry *file,
                        uint32_t first,uint32_t count,uint32_t *physical)
{
    PointerPage p;
    if(!physical || !count || first_page(v,file,&p) ||
       first>=p.data_blocks || count>p.data_blocks-first)return -1;
    for(uint32_t i=0u;i<count;++i)
        if(data_pointer(v,file->block,&p,first+i,&physical[i]))return -1;
    return 0;
}

static int module_read(const IfsFfsDropInVolume *v, const IfsFfsDropInEntry *file,
                       uint32_t position, void *output, uint32_t length)
{
    PointerPage p;
    if ((!output && length) || first_page(v, file, &p) ||
        position > file->size || length > file->size - position) return -1;
    uint8_t *dest = output;
    while (length) {
        const uint32_t within = position % v->io.block_bytes;
        uint32_t count = v->io.block_bytes - within, block;
        if (count > length) count = length;
        if (data_pointer(v, file->block, &p, position / v->io.block_bytes, &block) ||
            v->io.read_at(v->io.context, (uint64_t)block * v->io.block_bytes + within,
                          dest, count)) return -1;
        dest += count;
        position += count;
        length -= count;
    }
    return 0;
}
static const IfsFfsDropInModule module = {
    IFS_FFS_NATIVE_TABLE_VERSION, sizeof(IfsFfsDropInModule),
    "ffs", "Amiga Fast File System",
    IFS_FFS_CAP_PROBE | IFS_FFS_CAP_LOOKUP | IFS_FFS_CAP_ENUMERATE |
    IFS_FFS_CAP_FILE_READ | IFS_FFS_CAP_DOS1 | IFS_FFS_CAP_DOS3 |
    IFS_FFS_CAP_DOS5 | IFS_FFS_CAP_BLOCK_512_TO_32768,
    mount_volume, module_root, module_lookup, module_next, module_read
};
const IfsFfsDropInModule *ifs_ffs_native_read_ops(void)
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
        if (ifs_ffs_data_block(&live, file,
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
        if (ifs_ffs_data_block(&live, file,
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

static const IfsFfsDropInModuleV2 module_v2 = {
    IFS_FFS_NATIVE_WRITER_VERSION, sizeof(IfsFfsDropInModuleV2),
    "ffs", IFS_FFS_V2_CAP_OVERWRITE_EXISTING, &module, overwrite_existing
};
const IfsFfsDropInModuleV2 *ifs_ffs_native_write_ops(void)
{
    return &module_v2;
}
