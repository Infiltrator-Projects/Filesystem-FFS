/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 over source-owned DOS1/3/5 FFS read/overwrite engines.
 * The canonical FFS drop-in engine owns all on-disk parsing and mutation.
 */
#include "ffs_module.h"
#include "../native/filesystems/ffs/core/ffs_primitives.h"
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

/* The native engine has one active image and shared scratch/IO state.
 * Enforce exclusion inside the V2 provider, not merely in its callers.
 * A reentrant host callback must receive BUSY rather than deadlocking
 * or replacing the device while a transfer is in progress. */
static atomic_flag v2_operation_gate=ATOMIC_FLAG_INIT;
static int v2_enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&v2_operation_gate,memory_order_acquire);
}
static int v2_leave_operation(int rc)
{
    atomic_flag_clear_explicit(&v2_operation_gate,memory_order_release);
    return rc;
}

#if !defined(IFS_FFS_FIRMWARE_BUILD)
typedef struct FfsRelocationPlan FfsRelocationPlan;
static void free_relocation_plan(FfsRelocationPlan *);
#endif
static struct {
    IfsFfsDropInVolume v;
    IfsFsHostRuntimeV2 io;
    uintptr_t generation,handle;
    uint32_t variant;
    int mounted,recovery_pending;
    int growth_ready,write_fault;
    uint32_t growth_first,growth_hint;
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    FfsRelocationPlan *plan;
#endif
} s;
/* The native FFS reader returns 0/-1. Preserve the precise Common V2
 * host failure instead of treating an unavailable sector as missing files,
 * corrupt metadata or a different filesystem. One active session per SO. */
static int last_read_error=IFS_FS_OK;
static int session_live(void *v){
    return s.mounted && s.handle && (uintptr_t)v==s.handle && s.v.mounted;
}
static int live(void *v){return session_live(v) && !s.recovery_pending;}
static int host_ok(const IfsFsHostRuntimeV2 *h)
{
    return ifs_fs_host_v2_valid(h) && h->volume_bytes>=8192u;
}
static int v2_read(void *ctx,uint64_t offset,void *dst,uint32_t bytes)
{
    const IfsFsHostRuntimeV2 *h=ctx;
    const int rc=ifs_fs_host_read_bytes_v2(h,offset,dst,bytes);
    if(rc!=IFS_FS_OK)last_read_error=rc;
    return rc==IFS_FS_OK?0:-1;
}
#if !defined(IFS_FFS_FIRMWARE_BUILD)
/* The independent FFS writer preflights allocation before copying allocated
 * file bytes. Only full aligned V2 host sectors may be written. Reconstruct
 * partial sectors without changing neighbouring data. No crash rollback is
 * claimed after a host I/O failure. */
static int v2_write(void *ctx,uint64_t at,const void *bytes,uint32_t length)
{
    const IfsFsHostRuntimeV2 *h=ctx;
    const uint8_t *src=bytes;
    const uint32_t sector=h ? h->logical_sector_bytes : 0u;
    uint8_t *scratch;
    int result=-1;
    if(!host_ok(h) || !h->write_at || !h->flush ||
       (h->capabilities & IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities & IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities & IFS_FS_HOST_DURABLE) ||
       (!src && length) || at>h->volume_bytes ||
       length>h->volume_bytes-at)
        return -1;
    if(!length)return 0;
    scratch=h->allocate(h->context,sector);
    if(!scratch)return -1;
    while(length) {
        const uint64_t base=at-at%sector;
        const uint32_t skip=(uint32_t)(at%sector);
        uint32_t n=sector-skip;
        if(n>length)n=length;
        if(base>h->volume_bytes || sector>h->volume_bytes-base)
            goto finish;
        if(skip || n!=sector) {
            if(h->read_at(h->context,base,scratch,sector)!=0)
                goto finish;
            memcpy(scratch+skip,src,n);
        } else memcpy(scratch,src,sector);
        if(h->write_at(h->context,base,scratch,sector)!=0)
            goto finish;
        src+=n;at+=n;length-=n;
    }
    result=0;
finish:
    h->release(h->context,scratch);
    return result;
}
static int v2_flush(void *ctx)
{
    const IfsFsHostRuntimeV2 *h=ctx;
    return host_ok(h) && h->flush && h->flush(h->context)==0 ? 0 : -1;
}
#endif /* !IFS_FFS_FIRMWARE_BUILD */

static int detect(const IfsFsHostRuntimeV2 *h,IfsFfsDropInVolume *found,
                  uint32_t *variant){
    uint8_t boot[4];
    int valid=0;
    const IfsFfsDropInModule *m=ifs_ffs_native_read_ops();
    last_read_error=IFS_FS_OK;
    if(v2_read((void *)h,0,boot,sizeof(boot)))
        return last_read_error!=IFS_FS_OK?
            last_read_error:IFS_FS_IO_ERROR;
    if(boot[0]!='D'||boot[1]!='O'||boot[2]!='S'||
        (boot[3]!=1u&&boot[3]!=3u&&boot[3]!=5u))
        return IFS_FS_NOT_FOUND;
    for(uint32_t size=512u;size<=32768u;size*=2u){
        IfsFfsDropInVolume v={0};
        IfsFfsDropInIo io={v2_read,(void *)h,h->volume_bytes,size};
        if(h->volume_bytes%size)continue;
        last_read_error=IFS_FS_OK;
        if(m->mount(&v,&io)!=0) {
            if(last_read_error!=IFS_FS_OK)return last_read_error;
            continue;
        }
        if(++valid>1)return IFS_FS_CORRUPT;
        *found=v;
    }
    if(valid!=1)return IFS_FS_CORRUPT;
    *variant=boot[3];
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant){
    IfsFfsDropInVolume v={0};
    if(!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!host_ok(h))return IFS_FS_INVALID;
    return detect(h,&v,variant);
}
static int guarded_probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(probe(h,variant));
}

/* Measure free space from the on-media AFFS allocation bitmap. Its
 * pointers are relative to the partition, its bits start at block two,
 * and its root tail layout is independent of the filesystem block size.
 * Never interpret an invalid/dirty bitmap as a fully allocated volume. */
static uint32_t bitmap_word(const uint8_t *p,uint32_t i)
{
    p+=i*4u;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}
static int bitmap_read(uint32_t number,uint8_t *p)
{
    if(number<2u || number>=s.v.blocks)return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(&s.io,
        (uint64_t)number*s.v.io.block_bytes,p,s.v.io.block_bytes);
}
static int bitmap_checksum(const uint8_t *p,uint32_t words)
{
    uint32_t sum=0u;
    for(uint32_t i=0;i<words;++i)sum+=bitmap_word(p,i);
    return sum==0u;
}
/* Open-addressed set bounds duplicate/cycle detection to the number of
 * bitmap and extension blocks rather than the entire volume geometry.
 * Zero is an empty slot; legal metadata block numbers start at two. */
static int bitmap_remember(uint32_t *set,size_t capacity,uint32_t number)
{
    size_t i=(size_t)((uint64_t)number*UINT32_C(2654435761))&(capacity-1u);
    while(set[i]) {
        if(set[i]==number)return IFS_FS_CORRUPT;
        i=(i+1u)&(capacity-1u);
    }
    set[i]=number;
    return IFS_FS_OK;
}
/* Collect and validate the allocation bitmap keys for every caller.
 * Keys are caller-owned and valid only on successful completion. Both
 * volume accounting and allocation paging must reject metadata marked free. */
static int scan_free_bytes(uint64_t *out,uint32_t *keys,uint8_t *owners)
{
    *out=0u;
    const uint32_t bs=s.v.io.block_bytes,words=bs/4u;
    const uint32_t bits=(words-1u)*32u;
    const uint64_t span=(uint64_t)s.v.blocks-2u;
    const uint32_t maps=(uint32_t)((span+bits-1u)/bits);
    const uint32_t exts=maps>25u?(maps-25u+words-2u)/(words-1u):0u;
    size_t capacity=1u;
    while(capacity<2u*((size_t)maps+exts+1u))capacity*=2u;
    uint8_t *buf=s.io.allocate(s.io.context,(size_t)bs*2u);
    if(!buf)return IFS_FS_NO_MEMORY;
    uint32_t *seen=s.io.allocate(s.io.context,capacity*sizeof(*seen));
    if(!seen){s.io.release(s.io.context,buf);return IFS_FS_NO_MEMORY;}
    memset(seen,0,capacity*sizeof(*seen));
    uint8_t *ptrs=buf,*map=buf+bs;
    int rc=bitmap_read(s.v.root_block,ptrs);
    if(rc!=IFS_FS_OK)goto done;
    if(!bitmap_checksum(ptrs,words) || bitmap_word(ptrs,0u)!=2u ||
       bitmap_word(ptrs,words-1u)!=1u) {rc=IFS_FS_CORRUPT;goto done;}
    if(bitmap_word(ptrs,words-50u)!=UINT32_MAX) {
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    rc=bitmap_remember(seen,capacity,s.v.root_block);
    if(rc!=IFS_FS_OK)goto done;
    uint32_t slot=words-49u,limit=words-24u;
    uint32_t next=bitmap_word(ptrs,limit);
    uint64_t free_blocks=0u;
    for(uint32_t i=0u;i<maps;++i) {
        if(s.io.cancelled && s.io.cancelled(s.io.context)) {
            rc=IFS_FS_BUSY;goto done;
        }
        if(slot==limit) {
            if(next<2u || next>=s.v.blocks) {rc=IFS_FS_CORRUPT;goto done;}
            rc=bitmap_remember(seen,capacity,next);
            if(rc!=IFS_FS_OK)goto done;
            rc=bitmap_read(next,ptrs);if(rc!=IFS_FS_OK)goto done;
            slot=0u;limit=words-1u;next=bitmap_word(ptrs,limit);
        }
        const uint32_t number=bitmap_word(ptrs,slot++);
        if(number<2u || number>=s.v.blocks) {rc=IFS_FS_CORRUPT;goto done;}
        rc=bitmap_remember(seen,capacity,number);if(rc!=IFS_FS_OK)goto done;
        keys[i]=number;
        rc=bitmap_read(number,map);if(rc!=IFS_FS_OK)goto done;
        if(!bitmap_checksum(map,words)){rc=IFS_FS_CORRUPT;goto done;}
        const uint64_t start=(uint64_t)i*bits;
        uint32_t valid=(uint32_t)(span-start<bits?span-start:bits);
        /* Count only addressable bits: unused tail bits are not storage. */
        for(uint32_t w=1u;valid;++w) {
            uint32_t value=bitmap_word(map,w);
            const uint32_t n=valid<32u?valid:32u;
            if(n<32u)value&=(UINT32_C(1)<<n)-1u;
            while(value){value&=value-1u;++free_blocks;}
            valid-=n;
        }
    }
    if(next){rc=IFS_FS_CORRUPT;goto done;}
    /* A checksum-correct map can still falsely mark its own root or
     * bitmap/extension blocks free. Reject that before publishing a map. */
    {
        uint32_t loaded=UINT32_MAX;
        for(size_t i=0;i<capacity;++i) {
            if(!seen[i])continue;
            const uint32_t rel=seen[i]-2u,index=rel/bits,bit=rel%bits;
            if(index>=maps){rc=IFS_FS_CORRUPT;goto done;}
            if(index!=loaded) {
                rc=bitmap_read(keys[index],map);
                if(rc!=IFS_FS_OK)goto done;
                if(!bitmap_checksum(map,words)){rc=IFS_FS_CORRUPT;goto done;}
                loaded=index;
            }
            if(bitmap_word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u))) {
                rc=IFS_FS_CORRUPT;goto done;
            }
        }
    }
    /* Root, bitmap and extension blocks are fixed metadata. Optionally
     * initialise the integrity checker's ownership set with these blocks. */
    if(owners)
        for(size_t i=0;i<capacity;++i)
            if(seen[i])owners[seen[i]/8u]|=(uint8_t)(1u<<(seen[i]%8u));
    *out=free_blocks*bs;
    rc=IFS_FS_OK;
done:
    s.io.release(s.io.context,seen);
    s.io.release(s.io.context,buf);
    return rc;
}

static int measured_free_bytes(uint64_t *out)
{
    /* The same metadata ownership validation used for allocation pages
     * is mandatory for the scalar free-space count. A valid checksum alone
     * cannot prove that the allocator's own metadata remains allocated. */
    *out=0u;
    const uint32_t bits=(s.v.io.block_bytes/4u-1u)*32u;
    const uint64_t span=(uint64_t)s.v.blocks-2u;
    const uint64_t maps=(span+bits-1u)/bits;
    if(!maps || maps>SIZE_MAX/sizeof(uint32_t))return IFS_FS_CORRUPT;
    uint32_t *keys=s.io.allocate(s.io.context,(size_t)maps*sizeof(*keys));
    if(!keys)return IFS_FS_NO_MEMORY;
    const int rc=scan_free_bytes(out,keys,NULL);
    s.io.release(s.io.context,keys);
    return rc;
}

/* Whole-volume allocation comes from the source-owned AFFS bitmap, not
 * a list of regular-file extents. Verify all bitmap pages and their own
 * metadata ownership before exposing any range, even on a short page.
 * The map's physical cursor may begin inside a filesystem block. */
static int allocation_map(void *volume,uint64_t first,
                          IfsFsAllocationRangeV2 *out,uint32_t capacity,
                          uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!live(volume)||!out||!capacity||capacity>64u)return IFS_FS_INVALID;
    const uint64_t limit=s.io.volume_bytes;
    if(first>limit)return IFS_FS_INVALID;
    if(s.write_fault)return IFS_FS_CORRUPT;
    const uint32_t bs=s.v.io.block_bytes,words=bs/4u,bits=(words-1u)*32u;
    const uint64_t span=(uint64_t)s.v.blocks-2u;
    const uint32_t maps=(uint32_t)((span+bits-1u)/bits);
    if(!maps || (uint64_t)maps*sizeof(uint32_t)>SIZE_MAX)return IFS_FS_CORRUPT;
    uint32_t *keys=s.io.allocate(s.io.context,(size_t)maps*sizeof(*keys));
    if(!keys)return IFS_FS_NO_MEMORY;
    uint8_t *map=s.io.allocate(s.io.context,bs);
    if(!map){s.io.release(s.io.context,keys);return IFS_FS_NO_MEMORY;}
    uint64_t free_bytes=0u;
    int rc=scan_free_bytes(&free_bytes,keys,NULL);
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint32_t count=0u,loaded=UINT32_MAX,scanned=0u;
    uint64_t at=first;
    if(rc!=IFS_FS_OK)goto done;
    if(first==limit){rc=IFS_FS_END;goto done;}
    while(at<limit && scanned<65536u) {
        if(s.io.cancelled && s.io.cancelled(s.io.context)) {
            rc=IFS_FS_BUSY;goto done;
        }
        const uint64_t block=at/bs;
        uint64_t end;
        uint32_t state;
        if(block<2u) {
            state=IFS_FS_SPACE_RESERVED;end=2u*(uint64_t)bs;
        } else if(block>=s.v.blocks) {
            state=IFS_FS_SPACE_RESERVED;end=limit;
        } else {
            const uint32_t rel=(uint32_t)block-2u,index=rel/bits,bit=rel%bits;
            if(index>=maps){rc=IFS_FS_CORRUPT;goto done;}
            if(index!=loaded) {
                rc=bitmap_read(keys[index],map);
                if(rc!=IFS_FS_OK)goto done;
                if(!bitmap_checksum(map,words)){rc=IFS_FS_CORRUPT;goto done;}
                loaded=index;
            }
            state=(bitmap_word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u)))?
                IFS_FS_SPACE_FREE:IFS_FS_SPACE_ALLOCATED;
            end=(block+1u)*bs;
        }
        if(end>limit)end=limit;
        if(count && page[count-1u].state==state)page[count-1u].length+=end-at;
        else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){
                .physical_offset=at,.length=end-at,.state=state};
        }
        at=end;++scanned;
    }
    memcpy(out,page,count*sizeof(*page));
    *written=count;
    rc=IFS_FS_OK;
done:
    s.io.release(s.io.context,map);
    s.io.release(s.io.context,keys);
    return rc;
}
static int guarded_allocation_map(void *volume,uint64_t first,
                          IfsFsAllocationRangeV2 *out,uint32_t capacity,
                          uint32_t *written)
{
    if(written)*written=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(allocation_map(volume,first,out,capacity,written));
}
static const IfsFsAllocationOpsV2 allocation_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsAllocationOpsV2),
             IFS_FS_GROUP_ALLOCATION,0u,IFS_FS_ALLOCATION_MAP},
    .map=guarded_allocation_map
};

/* The native formatter leaves the second boot sector empty. During an
 * offline relocation it is a temporary redo locator, never a permanent
 * filesystem extension. Do not overwrite arbitrary foreign boot code.
 * A dirty locator blocks every ordinary mount operation until replay.
 * This profile requires 512-byte physical sectors; a torn locator is
 * rejected, never interpreted as authority for arbitrary writes. */
static const uint8_t relocation_magic[8]={'I','F','S','F','F','S','R','1'};
static uint32_t relocation_crc(uint32_t crc,const uint8_t *p,size_t n)
{
    while(n--) {
        crc^=*p++;
        for(unsigned k=0u;k<8u;++k)
            crc=(crc>>1)^((0u-(crc&1u))&UINT32_C(0x82f63b78));
    }
    return crc;
}
static int relocation_marker(uint8_t marker[512],int *pending)
{
    *pending=0;
    if(s.variant!=1u || s.v.io.block_bytes!=512u)
        return IFS_FS_UNSUPPORTED;
    uint8_t boot[512];
    int rc=ifs_fs_host_read_bytes_v2(&s.io,0u,boot,sizeof(boot));
    if(rc!=IFS_FS_OK)return rc;
    if(bitmap_word(boot,0u)!=UINT32_C(0x444f5301) || bitmap_word(boot,2u)!=s.v.root_block)
        return IFS_FS_UNSUPPORTED;
    uint32_t sum=0u;
    for(unsigned i=0u;i<128u;++i) {
        uint32_t next=sum+bitmap_word(boot,i);
        sum=next+(next<sum?1u:0u);
    }
    if(sum!=UINT32_MAX)return IFS_FS_UNSUPPORTED;
    for(unsigned i=12u;i<512u;++i)if(boot[i])return IFS_FS_UNSUPPORTED;
    rc=ifs_fs_host_read_bytes_v2(&s.io,512u,marker,512u);
    if(rc!=IFS_FS_OK)return rc;
    unsigned nonzero=0u;
    for(unsigned i=0u;i<512u;++i)nonzero|=marker[i];
    if(!nonzero)return IFS_FS_OK;
    for(unsigned i=0u;i<sizeof(relocation_magic);++i)
        if(marker[i]!=relocation_magic[i])return IFS_FS_CORRUPT;
    if(bitmap_word(marker,127u)!=relocation_crc(UINT32_MAX,marker,508u))
        return IFS_FS_CORRUPT;
    *pending=1;
    return IFS_FS_OK;
}

static int mount(const IfsFsHostRuntimeV2 *h,void **volume,
                 IfsFsVolumeInfoV2 *info){
    uint32_t variant;int r;
    if(!host_ok(h)||!volume||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
#if defined(IFS_FFS_FIRMWARE_BUILD)
    /* The freestanding descriptor advertises no write operations.
     * Reject a writable Common V2 lease before native probing or I/O. */
    if (!(h->capabilities&IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
#endif
    if(s.mounted||s.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    r=detect(h,&s.v,&variant);
    if(r)return r;
    /* Preserve the caller's size-versioned V2 prefix without reading
     * optional fields that an earlier host did not allocate. */
    size_t host_bytes=h->structure_bytes<sizeof(s.io) ?
        h->structure_bytes : sizeof(s.io);
    memset(&s.io,0,sizeof(s.io));
    memcpy(&s.io,h,host_bytes);
    s.io.structure_bytes=(uint32_t)host_bytes;
    /* The native engine retains this callback context through unmount. */
    s.v.io.context=&s.io;
    s.variant=variant;s.mounted=1;
    s.growth_ready=0;s.write_fault=0;s.recovery_pending=0;s.growth_first=0;s.growth_hint=0;
    s.handle=++s.generation;
    info->variant=variant;info->max_name_bytes=60u;
    info->block_bytes=s.v.io.block_bytes;
    info->total_bytes=s.v.io.volume_bytes;
    /* Permit forensic namespace reads on a damaged allocation map, but
     * never grant a writable session whose free-space ownership is invalid.
     * The explicit VOLUME.QUERY and ALLOCATION.MAP still report the error. */
    uint8_t marker[512];int pending=0;
    r=relocation_marker(marker,&pending);
    if(r!=IFS_FS_OK && r!=IFS_FS_UNSUPPORTED) {
        s.v.mounted=0u;s.mounted=0;s.handle=0;return r;
    }
    if(pending) {
        if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
           h->logical_sector_bytes!=512u || h->physical_sector_bytes!=512u) {
            s.v.mounted=0u;s.mounted=0;s.handle=0;return IFS_FS_BUSY;
        }
        s.recovery_pending=1;info->free_bytes=0u;
        *volume=(void *)s.handle;return IFS_FS_OK;
    }
    r=measured_free_bytes(&info->free_bytes);
    if(r!=IFS_FS_OK && (!(h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       (r!=IFS_FS_CORRUPT && r!=IFS_FS_UNSUPPORTED))) {
        s.v.mounted=0u;s.mounted=0;s.handle=0;
        return r;
    }
    *volume=(void *)s.handle;
    return IFS_FS_OK;
}
static int guarded_mount(const IfsFsHostRuntimeV2 *h,void **volume,
                 IfsFsVolumeInfoV2 *info)
{
    if (volume) *volume=NULL;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(mount(h,volume,info));
}

static int unmount(void *volume){
    if(!session_live(volume))return IFS_FS_INVALID;
    #if !defined(IFS_FFS_FIRMWARE_BUILD)
    if(s.plan){free_relocation_plan(s.plan);s.plan=NULL;}
    #endif
    s.v.mounted=0u;s.mounted=0;s.handle=0;
    return IFS_FS_OK;
}
static int guarded_unmount(void *volume)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(unmount(volume));
}

static int utf8_to_latin1(const char *src,uint32_t length,
                          uint8_t out[31],uint32_t *count) {
    uint32_t i=0,n=0;
    while(i<length){
        unsigned char c=(unsigned char)src[i++];
        if(c<128u){if(!c||c=='/'||c=='\\')return IFS_FS_INVALID;}
        else if((c==0xc2u||c==0xc3u)&&i<length){
            unsigned char b=(unsigned char)src[i++];
            if((b&0xc0u)!=0x80u)return IFS_FS_INVALID;
            c=(unsigned char)((c==0xc2u?0x80u:0xc0u)|(b&0x3fu));
        } else return IFS_FS_INVALID;
        if(n>=30u)return IFS_FS_UNSUPPORTED;
        out[n++]=c;
    }
    if(!n)return IFS_FS_INVALID;
    *count=n;return IFS_FS_OK;
}
static int path_entry(const char *path,IfsFfsDropInEntry *out,
                      const char **basename,uint32_t *name_len){
    const IfsFfsDropInModule *m=ifs_ffs_native_read_ops();
    IfsFfsDropInEntry current=m->root(&s.v);
    if(!path||path[0]!='/')return IFS_FS_INVALID;
    const char *p=path+1;
    if(!*p){*out=current;*basename=path;*name_len=1u;return IFS_FS_OK;}
    while(*p){
        const char *start=p;uint8_t latin[31];uint32_t n;
        while(*p&&*p!='/')++p;
        if(p==start)return IFS_FS_INVALID;
        int r=utf8_to_latin1(start,(uint32_t)(p-start),latin,&n);
        if(r)return r;
        IfsFfsDropInEntry next={0};
        r=m->lookup(&s.v,&current,latin,n,&next);
        if(r)return r==1?IFS_FS_NOT_FOUND:
            last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        current=next;*basename=start;*name_len=(uint32_t)(p-start);
        if(*p=='/'&&!*++p)return IFS_FS_INVALID;
    }
    *out=current;return IFS_FS_OK;
}
static int convert_name(const uint8_t *latin,uint32_t n,char *out,uint32_t cap,
                        uint32_t *written){
    uint32_t required=0u,k=0u;
    if(!latin || !out || !written)return IFS_FS_INVALID;
    /* Validate the complete UTF-8 size, including the terminator,
     * before touching the caller-owned buffer. A capacity failure must
     * never expose a partially converted directory name. */
    for(uint32_t i=0;i<n;++i)
        required+=latin[i]<128u?1u:2u;
    if(required>=cap)return IFS_FS_NO_SPACE;
    for(uint32_t i=0;i<n;++i){
        const unsigned char c=latin[i];
        if(c<128u)out[k++]=(char)c;
        else {
            out[k++]=(char)(0xc0u|(c>>6));
            out[k++]=(char)(0x80u|(c&0x3fu));
        }
    }
    out[k]=0;*written=k;
    return IFS_FS_OK;
}
static int fill(IfsFsEntryV2 *out,const IfsFfsDropInEntry *e,
                const char *name,uint32_t n) {
    uint8_t raw[31];uint32_t chars;
    if(!out||out->structure_bytes<sizeof(*out)||!out->name||
       !out->name_capacity)return IFS_FS_INVALID;
    int r=IFS_FS_OK;
    if(n==1u && name[0]=='/'){
        if(out->name_capacity<2u)return IFS_FS_NO_SPACE;
        out->name[0]='/';out->name[1]=0;out->name_bytes=1u;
    }else{
        r=utf8_to_latin1(name,n,raw,&chars);
        if(r)return r;
        r=convert_name(raw,chars,out->name,out->name_capacity,&out->name_bytes);
        if(r)return r;
    }
    out->size_bytes=e->size;out->attributes=0;
    out->type=e->type==IFS_FFS_DIRECTORY?
        IFS_FS_ENTRY_DIRECTORY:IFS_FS_ENTRY_FILE;
    return IFS_FS_OK;
}
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *out){
    IfsFfsDropInEntry e={0};const char *name=NULL;uint32_t n=0;
    if(!live(volume))return IFS_FS_INVALID;
    last_read_error=IFS_FS_OK;
    int r=path_entry(path,&e,&name,&n);
    return r?r:fill(out,&e,name,n);
}
static int guarded_stat_path(void *volume,const char *path,IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(stat_path(volume,path,out));
}

static int nth(void *volume,const char *dir,uint32_t index,IfsFsEntryV2 *out){
    IfsFfsDropInEntry parent={0},e={0};
    const char *name;uint32_t n;uint8_t raw[31];uint32_t length=0;
    IfsFfsDropInDirCursor cursor={0};
    if(!live(volume))return IFS_FS_INVALID;
    last_read_error=IFS_FS_OK;
    int r=path_entry(dir,&parent,&name,&n);
    if(r)return r;
    const IfsFfsDropInModule *m=ifs_ffs_native_read_ops();
    for(uint32_t i=0;i<=index;++i) {
        r=m->next(&s.v,&parent,&cursor,&e,raw);
        if(r)return r==1?IFS_FS_END:
            last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        if(i==UINT32_MAX)return IFS_FS_INVALID;
    }
    while(length<31u && raw[length])++length;
    if(length==31u)return IFS_FS_CORRUPT;
    if(!out||out->structure_bytes<sizeof(*out)||!out->name||
       !out->name_capacity)return IFS_FS_INVALID;
    r=convert_name(raw,length,out->name,out->name_capacity,&out->name_bytes);
    if(r)return r;
    out->size_bytes=e.size;out->attributes=0;
    out->type=e.type==IFS_FFS_DIRECTORY?
       IFS_FS_ENTRY_DIRECTORY:IFS_FS_ENTRY_FILE;
    return IFS_FS_OK;
}
static int guarded_nth(void *volume,const char *dir,uint32_t index,IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(nth(volume,dir,index,out));
}

static int read_file(void *volume,const char *path,uint64_t offset,
                     void *dest,uint32_t len) {
    IfsFfsDropInEntry e={0};const char *name;uint32_t n;
    if(!live(volume)||(!dest&&len))return IFS_FS_INVALID;
    if(offset>UINT32_MAX||len>UINT32_MAX-offset)return IFS_FS_UNSUPPORTED;
    last_read_error=IFS_FS_OK;
    int r=path_entry(path,&e,&name,&n);
    if(r)return r;
    if(e.type!=IFS_FFS_FILE || offset>e.size || len>e.size-offset)return IFS_FS_INVALID;
    return ifs_ffs_native_read_ops()->read(&s.v,&e,(uint32_t)offset,
                                                     dest,len)==0?
        IFS_FS_OK:(last_read_error!=IFS_FS_OK?
                  last_read_error:IFS_FS_CORRUPT);
}
static int guarded_read_file(void *volume,const char *path,uint64_t offset,
                     void *dest,uint32_t len)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(read_file(volume,path,offset,dest,len));
}

#if !defined(IFS_FFS_FIRMWARE_BUILD)
/* Native DOS1 creation/growth on the formatter's 512-byte layout.
 * Stage full blocks, including bitmap and checksummed header changes,
 * so ordinary write/flush errors can restore attempted blocks. This is
 * not a power-loss journal; failed rollback disables further mutation.
 * All metadata and allocation choices remain inside FFS.so. */
enum { GROW_BS=512, GROW_WORDS=128, GROW_BITS=4064, GROW_PATCHES=160 };
typedef struct {uint32_t number;uint8_t *old,*now;int dirty;} FfsPatch;
typedef struct {FfsPatch p[GROW_PATCHES];unsigned count;uint32_t hint;} FfsTxn;
static void grow_word(uint8_t *p,uint32_t i,uint32_t n)
{
    p+=4u*i;p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);
    p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;
}
static void grow_checksum(uint8_t *p,uint32_t i)
{
    grow_word(p,i,0);uint32_t sum=0;
    for(unsigned j=0;j<GROW_WORDS;++j)sum+=bitmap_word(p,j);
    grow_word(p,i,0u-sum);
}
static uint32_t grow_metadata(uint32_t ordinal)
{
    uint32_t b=ordinal+2u;
    return b>=s.v.root_block?b+1u:b;
}
/* Verify the exact formatter-owned allocation geometry once per mount.
 * A foreign bitmap placement remains readable, but cannot use this
 * bounded writer. Never allocate a root, bitmap or bitmap-extension
 * block even if malformed media advertises it as free. */
static int grow_profile(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    if((s.io.capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(s.io.capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(s.io.capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(s.write_fault)return IFS_FS_IO_ERROR;
    if(s.variant!=1u || s.v.io.block_bytes!=GROW_BS ||
       s.io.logical_sector_bytes!=GROW_BS || s.v.blocks>4194304u ||
       s.v.hash_size!=72u)return IFS_FS_UNSUPPORTED;
    if(s.growth_ready)return IFS_FS_OK;
    uint8_t root[512],ext[512],map[512];uint64_t free_bytes=0;
    int rc=measured_free_bytes(&free_bytes);if(rc!=IFS_FS_OK)return rc;
    rc=bitmap_read(s.v.root_block,root);if(rc!=IFS_FS_OK)return rc;
    const uint32_t maps=(s.v.blocks-2u+GROW_BITS-1u)/GROW_BITS;
    const uint32_t exts=maps>25u?(maps-25u+126u)/127u:0u;
    const uint32_t last=grow_metadata(maps+exts-1u);
    for(uint32_t i=0;i<maps;++i) {
        uint32_t ptr;
        if(i<25u)ptr=bitmap_word(root,79u+i);
        else {
            uint32_t e=(i-25u)/127u,k=(i-25u)%127u;
            if(!k) {
                uint32_t next=e?bitmap_word(ext,127u):bitmap_word(root,104u);
                if(next!=grow_metadata(maps+e))return IFS_FS_UNSUPPORTED;
                rc=bitmap_read(next,ext);if(rc!=IFS_FS_OK)return rc;
            }
            ptr=bitmap_word(ext,k);
        }
        if(ptr!=grow_metadata(i))return IFS_FS_UNSUPPORTED;
        rc=bitmap_read(ptr,map);if(rc!=IFS_FS_OK)return rc;
        for(uint32_t k=0;k<GROW_BITS;++k) {
            uint32_t disk=2u+i*GROW_BITS+k;
            if((disk<=last || disk==s.v.root_block || disk>=s.v.blocks) &&
               (bitmap_word(map,1u+k/32u)&(UINT32_C(1)<<(k%32u))))
                return IFS_FS_CORRUPT;
        }
    }
    s.growth_first=last+1u;s.growth_hint=s.growth_first;
    s.growth_ready=1;return IFS_FS_OK;
}
static void grow_release(FfsTxn *t)
{
    for(unsigned i=0;i<t->count;++i)s.io.release(s.io.context,t->p[i].old);
}
static int grow_patch(FfsTxn *t,uint32_t n,FfsPatch **out)
{
    if(n<2u || n>=s.v.blocks)return IFS_FS_CORRUPT;
    for(unsigned i=0;i<t->count;++i)
        if(t->p[i].number==n){*out=&t->p[i];return IFS_FS_OK;}
    if(t->count>=GROW_PATCHES)return IFS_FS_NO_MEMORY;
    uint8_t *raw=s.io.allocate(s.io.context,1024u);
    if(!raw)return IFS_FS_NO_MEMORY;
    int rc=bitmap_read(n,raw);
    if(rc!=IFS_FS_OK){s.io.release(s.io.context,raw);return rc;}
    memcpy(raw+512u,raw,512u);
    FfsPatch *p=&t->p[t->count++];
    *p=(FfsPatch){.number=n,.old=raw,.now=raw+512u};*out=p;
    return IFS_FS_OK;
}
static int grow_commit(FfsTxn *t)
{
    unsigned attempted=0;int rc=IFS_FS_OK;
    for(unsigned i=0;i<t->count;++i) {
        FfsPatch *p=&t->p[i];
        if(!p->dirty)continue;
        if(s.io.cancelled && s.io.cancelled(s.io.context)){rc=IFS_FS_BUSY;break;}
        attempted=i+1u;
        rc=ifs_fs_host_write_bytes_v2(&s.io,(uint64_t)p->number*512u,p->now,512u);
        if(rc!=IFS_FS_OK)break;
    }
    if(rc==IFS_FS_OK && s.io.flush(s.io.context)!=0)rc=IFS_FS_IO_ERROR;
    if(rc!=IFS_FS_OK) {
        int bad=0;
        while(attempted) {
            FfsPatch *p=&t->p[--attempted];
            if(p->dirty && ifs_fs_host_write_bytes_v2(&s.io,(uint64_t)p->number*512u,
                                       p->old,512u)!=IFS_FS_OK)bad=1;
        }
        if(s.io.flush(s.io.context)!=0)bad=1;
        if(bad)s.write_fault=1;
        return rc;
    }
    s.growth_hint=t->hint;return IFS_FS_OK;
}
static int grow_map(FfsTxn *t,uint32_t n,FfsPatch **map,uint32_t *word,uint32_t *mask)
{
    if(n<s.growth_first || n>=s.v.blocks || n==s.v.root_block)
        return IFS_FS_CORRUPT;
    uint32_t bit=n-2u;
    int rc=grow_patch(t,grow_metadata(bit/GROW_BITS),map);
    if(rc!=IFS_FS_OK)return rc;
    if(!bitmap_checksum((*map)->now,128u))return IFS_FS_CORRUPT;
    bit%=GROW_BITS;*word=1u+bit/32u;*mask=UINT32_C(1)<<(bit%32u);
    return IFS_FS_OK;
}
static int grow_alloc(FfsTxn *t,FfsPatch **out)
{
    /* Scan whole bitmap words when searching for a free block. Keep
     * the next allocation cursor across operations for interleaved growth. */
    uint32_t start=t->hint;
    if(start<s.growth_first || start>=s.v.blocks)start=s.growth_first;
    uint32_t n=start;
    do {
        if(s.io.cancelled && s.io.cancelled(s.io.context))return IFS_FS_BUSY;
        if(n!=s.v.root_block) {
            FfsPatch *map;uint32_t word,mask;
            int rc=grow_map(t,n,&map,&word,&mask);
            if(rc!=IFS_FS_OK)return rc;
            uint32_t val=bitmap_word(map->now,word);
            if(val&mask) {
                grow_word(map->now,word,val&~mask);grow_checksum(map->now,0);
                map->dirty=1;
                rc=grow_patch(t,n,out);if(rc!=IFS_FS_OK)return rc;
                memset((*out)->now,0,512u);(*out)->dirty=1;
                t->hint=n+1u;return IFS_FS_OK;
            }
        }
        if(++n==s.v.blocks)n=s.growth_first;
    }while(n!=start);
    return IFS_FS_NO_SPACE;
}
static int grow_allocated(FfsTxn *t,uint32_t n)
{
    FfsPatch *map;uint32_t word,mask;
    int rc=grow_map(t,n,&map,&word,&mask);
    if(rc!=IFS_FS_OK)return rc;
    return bitmap_word(map->now,word)&mask?IFS_FS_CORRUPT:IFS_FS_OK;
}
static int grow_create(void *volume,const char *path,uint32_t type)
{
    if(s.plan)return IFS_FS_BUSY;
    int rc=grow_profile(volume);if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE && type!=IFS_FS_ENTRY_DIRECTORY)
        return IFS_FS_UNSUPPORTED;
    if(!path || path[0]!='/' || !path[1] || strstr(path,"//"))return IFS_FS_INVALID;
    const char *slash=strrchr(path,'/');
    const char *leaf=slash+1;
    const size_t bytes=strlen(leaf);
    if(!bytes || bytes>60u || !strcmp(leaf,".") || !strcmp(leaf,".."))
        return IFS_FS_INVALID;
    uint8_t name[31];uint32_t n;
    rc=utf8_to_latin1(leaf,(uint32_t)bytes,name,&n);
    if(rc!=IFS_FS_OK)return rc;
    if(ifs_ffs_validate_name(name,n,1)!=IFS_FFS_NAME_OK)return IFS_FS_INVALID;
    const IfsFfsDropInModule *native=ifs_ffs_native_read_ops();
    IfsFfsDropInEntry parent=native->root(&s.v),found={0};
    last_read_error=IFS_FS_OK;
    /* Resolve the actual native parent, never interpret a slash-containing
     * leaf as a root filename. Missing/non-directory parents cannot allocate. */
    if(slash!=path) {
        const size_t length=(size_t)(slash-path);
        if(length>=4096u)return IFS_FS_INVALID;
        char *prefix=s.io.allocate(s.io.context,length+1u);
        if(!prefix)return IFS_FS_NO_MEMORY;
        memcpy(prefix,path,length);prefix[length]=0;
        const char *base;uint32_t len;
        rc=path_entry(prefix,&parent,&base,&len);
        s.io.release(s.io.context,prefix);
        if(rc!=IFS_FS_OK)return rc;
        if(parent.type!=IFS_FFS_DIRECTORY)return IFS_FS_INVALID;
    }
    rc=native->lookup(&s.v,&parent,name,n,&found);
    if(!rc)return IFS_FS_EXISTS;
    if(rc!=1)return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    FfsTxn t={.hint=s.growth_hint};FfsPatch *dir,*file;
    if(parent.block!=s.v.root_block) {
        rc=grow_allocated(&t,parent.block);if(rc!=IFS_FS_OK)goto done;
    }
    rc=grow_patch(&t,parent.block,&dir);if(rc!=IFS_FS_OK)goto done;
    if(!bitmap_checksum(dir->now,128u) || bitmap_word(dir->now,0u)!=2u ||
       (parent.block==s.v.root_block ?
        (bitmap_word(dir->now,127u)!=1u || bitmap_word(dir->now,78u)!=UINT32_MAX) :
        (bitmap_word(dir->now,127u)!=2u || bitmap_word(dir->now,1u)!=parent.block))) {
        rc=IFS_FS_CORRUPT;goto done;
    }
    rc=grow_alloc(&t,&file);if(rc!=IFS_FS_OK)goto done;
    const uint32_t bucket=ifs_ffs_directory_hash(name,n,72u,0);
    grow_word(file->now,0,2u);grow_word(file->now,1,file->number);
    grow_word(file->now,124,bitmap_word(dir->now,6u+bucket));
    grow_word(file->now,125,parent.block);
    /* A user directory is a native T_SHORT/ST_USERDIR hash table. The
     * zeroed allocation has no children, unlike a regular file header. */
    grow_word(file->now,127,type==IFS_FS_ENTRY_DIRECTORY?2u:(uint32_t)-3);
    file->now[432]=(uint8_t)n;memcpy(file->now+433,name,n);
    grow_checksum(file->now,5);file->dirty=1;
    grow_word(dir->now,6u+bucket,file->number);grow_checksum(dir->now,5);dir->dirty=1;
    rc=grow_commit(&t);
done:
    grow_release(&t);return rc;
}
static int guarded_grow_create(void *volume,const char *path,uint32_t type)
{
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(grow_create(volume,path,type));
}
/* Walk the existing list chain without staging every unchanged page.
 * Only the header, last occupied page, newly allocated list blocks and
 * data blocks enter the mutation. Each old pointer page is checksummed. */
static int grow_append(void *volume,const IfsFfsDropInEntry *e,
                       uint64_t pos,const void *data,uint32_t len)
{
    int rc=grow_profile(volume);if(rc!=IFS_FS_OK)return rc;
    if(!data || !len || len>65536u || pos!=e->size ||
       pos>UINT32_MAX || len>UINT32_MAX-pos)return IFS_FS_UNSUPPORTED;
    FfsTxn t={.hint=s.growth_hint};FfsPatch *header,*page=NULL;
    rc=grow_allocated(&t,e->block);if(rc!=IFS_FS_OK)goto done;
    rc=grow_patch(&t,e->block,&header);if(rc!=IFS_FS_OK)goto done;
    uint32_t used=(e->size+511ull)/512u;
    uint32_t pages=used?(used-1u)/72u+1u:1u;
    uint32_t number=e->block;
    uint8_t raw[512];
    for(uint32_t index=0;index<pages;++index) {
        rc=bitmap_read(number,raw);if(rc!=IFS_FS_OK)goto done;
        uint32_t count=used>index*72u?used-index*72u:0u;if(count>72u)count=72u;
        if(!bitmap_checksum(raw,128u) || bitmap_word(raw,0)!=(index?16u:2u) ||
           bitmap_word(raw,1)!=number || bitmap_word(raw,2)!=count ||
           bitmap_word(raw,127)!=(uint32_t)-3 ||
           (index && bitmap_word(raw,125)!=e->block)) {rc=IFS_FS_CORRUPT;goto done;}
        uint32_t next=bitmap_word(raw,126);
        if(index+1u==pages) {
            if(next){rc=IFS_FS_CORRUPT;goto done;}
            rc=grow_allocated(&t,number);if(rc!=IFS_FS_OK)goto done;
            rc=grow_patch(&t,number,&page);if(rc!=IFS_FS_OK)goto done;
        } else {
            if(next<s.growth_first || next>=s.v.blocks ||
               next==number || next==e->block || next==s.v.root_block) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            number=next;
        }
    }
    const uint8_t *src=data;
    while(len) {
        uint32_t logical=(uint32_t)(pos/512u),skip=(uint32_t)(pos%512u);
        uint32_t n=512u-skip;if(n>len)n=len;
        FfsPatch *block;
        if(skip) {
            uint32_t disk;
            if(ifs_ffs_data_block(&s.v,e,logical,&disk)!=0) {rc=IFS_FS_CORRUPT;goto done;}
            rc=grow_allocated(&t,disk);if(rc!=IFS_FS_OK)goto done;
            rc=grow_patch(&t,disk,&block);if(rc!=IFS_FS_OK)goto done;
        } else {
            uint32_t slot=logical%72u;
            if(logical && !slot) {
                FfsPatch *next;rc=grow_alloc(&t,&next);if(rc!=IFS_FS_OK)goto done;
                grow_word(next->now,0,16u);grow_word(next->now,1,next->number);
                grow_word(next->now,125,e->block);grow_word(next->now,127,(uint32_t)-3);
                grow_checksum(next->now,5);
                grow_word(page->now,126,next->number);grow_checksum(page->now,5);
                page->dirty=1;page=next;
            }
            rc=grow_alloc(&t,&block);if(rc!=IFS_FS_OK)goto done;
            grow_word(page->now,77u-slot,block->number);
            grow_word(page->now,2,slot+1u);
            if(!logical)grow_word(header->now,4,block->number);
            grow_checksum(page->now,5);page->dirty=1;
        }
        memcpy(block->now+skip,src,n);block->dirty=1;
        pos+=n;src+=n;len-=n;
    }
    grow_word(header->now,81,(uint32_t)pos);grow_checksum(header->now,5);
    header->dirty=1;rc=grow_commit(&t);
done:
    grow_release(&t);return rc;
}

static int write_file(void *volume,const char *path,uint64_t offset,
                      const void *data,uint32_t len){
    IfsFfsDropInEntry e={0};const char *name;uint32_t n;
    if(!live(volume)||(!data&&len))return IFS_FS_INVALID;
    if(s.plan)return IFS_FS_BUSY;
    if((s.io.capabilities&IFS_FS_HOST_READ_ONLY)||
       !(s.io.capabilities&IFS_FS_HOST_EXCLUSIVE)||
       !(s.io.capabilities&IFS_FS_HOST_DURABLE)||
       !s.io.write_at||!s.io.flush)return IFS_FS_ACCESS;
    if(offset>UINT32_MAX||len>UINT32_MAX-offset)return IFS_FS_UNSUPPORTED;
    int r=path_entry(path,&e,&name,&n);
    if(r)return r;
    if(e.type!=IFS_FFS_FILE)return IFS_FS_INVALID;
    if(s.write_fault)return IFS_FS_IO_ERROR;
    if(offset==e.size && len)return grow_append(volume,&e,offset,data,len);
    if(offset>e.size||len>e.size-offset)return IFS_FS_UNSUPPORTED;
    IfsFfsWritableIo io={0};
    io.read=s.v.io;io.write_at=v2_write;io.flush=v2_flush;
    r=ifs_ffs_native_write_ops()->overwrite_existing(&s.v,&e,&io,
        (uint32_t)offset,data,len);
    return r==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int guarded_write_file(void *volume,const char *path,uint64_t offset,
                      const void *data,uint32_t len)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(write_file(volume,path,offset,data,len));
}

#endif /* !IFS_FFS_FIRMWARE_BUILD */

/* V2 volume inquiry uses the mounted native reader. */
static int v2_query_volume(void *volume,IfsFsVolumeInfoV2 *info){
    if(!live(volume)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    info->variant=s.variant;
    info->block_bytes=s.v.io.block_bytes;
    info->max_name_bytes=60u;
    info->total_bytes=s.v.io.volume_bytes;
    return measured_free_bytes(&info->free_bytes);
}
static int guarded_v2_query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(v2_query_volume(volume,info));
}

/* Flush through the actual mounted provider's host, not an external
 * filesystem utility. Read-only mounts have no outstanding dirty buffers. */
static int v2_sync_volume(void *volume){
    if(!live(volume))return IFS_FS_INVALID;
    if(s.io.capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_OK;
    if(!(s.io.capabilities&IFS_FS_HOST_EXCLUSIVE) || !s.io.write_at || !s.io.flush)
        return IFS_FS_ACCESS;
    return s.io.flush(s.io.context)==0 ? IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int guarded_v2_sync_volume(void *volume)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(v2_sync_volume(volume));
}

/* One direct V2 CORE/VOLUME contract. Never claim unqualified repair,
 * allocation mutation or DEFRAG callbacks. */
/* Source-owned, read-only Common V2 per-file extent enumeration.
 * A page is fully verified before exposing any returned extent; never
 * infer an allocation from a short or corrupt file-extension chain.
 * Hosted relocation uses the separately qualified native redo protocol. */
static int map_file(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    IfsFfsDropInEntry file={0};
    const char *basename=NULL;
    uint32_t name_length=0u,physical[64];
    uint64_t total,count;
    int rc;
    if(!live(volume)||!path||!out||!written||!capacity||capacity>64u)
        return IFS_FS_INVALID;
    *written=0u;
    last_read_error=IFS_FS_OK;
    rc=path_entry(path,&file,&basename,&name_length);
    if(rc)return rc;
    if(file.type!=IFS_FFS_FILE || !s.v.io.block_bytes)
        return IFS_FS_UNSUPPORTED;
    total=((uint64_t)file.size+s.v.io.block_bytes-1u)/s.v.io.block_bytes;
    if(first>=total)return IFS_FS_END;
    count=total-first;
    if(count>capacity)count=capacity;
    /* Resolve a whole page with a single native extension-chain walk.
     * Restarting at the file header for each block was quadratic in
     * the number of continuation pages for large fragmented files. */
    if(ifs_ffs_data_blocks(&s.v,&file,(uint32_t)first,(uint32_t)count,physical)!=0)
        return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    for(uint64_t i=0u;i<count;++i) {
        uint64_t offset=(uint64_t)physical[i]*s.v.io.block_bytes;
        if(offset>=s.v.io.volume_bytes ||
           s.v.io.block_bytes>s.v.io.volume_bytes-offset)
            return IFS_FS_CORRUPT;
        for(uint64_t j=0u;j<i;++j)
            if(physical[i]==physical[j])return IFS_FS_CORRUPT;
    }
    for(uint64_t i=0u;i<count;++i) {
        uint64_t logical=(first+i)*s.v.io.block_bytes;
        uint64_t remaining=file.size-logical;
        out[i].logical_offset=logical;
        out[i].physical_offset=(uint64_t)physical[i]*s.v.io.block_bytes;
        out[i].length=remaining<s.v.io.block_bytes?
                      remaining:s.v.io.block_bytes;
        out[i].flags=0u;out[i].reserved=0u;
    }
    *written=(uint32_t)count;
    return IFS_FS_OK;
}
static int guarded_map_file(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if (written) *written=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(map_file(volume,path,first,out,capacity,written));
}

#if !defined(IFS_FFS_FIRMWARE_BUILD)
static int guarded_relocation_plan(void *,const char *,uint32_t,void **);
static void guarded_release_relocation_plan(void *,void *);
static int guarded_relocation_execute(void *,void *,uint64_t *);
static int guarded_relocation_verify(void *,void *);
static int guarded_relocation_recover(void *,uint32_t,uint64_t *);
#endif
static const IfsFsDefragOpsV2 defrag_map_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP
#if !defined(IFS_FFS_FIRMWARE_BUILD)
             |IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY|IFS_FS_DEFRAG_RECOVER
#endif
    },
    .map=guarded_map_file
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    ,.plan=guarded_relocation_plan,.release_plan=guarded_release_relocation_plan
    ,.execute=guarded_relocation_execute,.verify=guarded_relocation_verify,.recover=guarded_relocation_recover
#endif
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
             #if defined(IFS_FFS_FIRMWARE_BUILD)
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
#else
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|IFS_FS_CORE_CREATE},
#endif
    .probe=guarded_probe,.mount=guarded_mount,.unmount=guarded_unmount,
    .stat=guarded_stat_path,.directory_entry=guarded_nth,.read=guarded_read_file
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    ,.write=guarded_write_file,.create=guarded_grow_create
#endif
};
#if !defined(IFS_FFS_FIRMWARE_BUILD)
/* DOS1 empty-volume formatter. The FFS provider owns all disk structures;
 * callers supply only a verified, exclusive Common V2 host. The bitmap
 * geometry is the same as the repository's original FFS fixture codec.
 * Publish the boot identity last, after root and allocation metadata. */
enum { FORMAT_BLOCK=512u, FORMAT_WORDS=128u, FORMAT_MAP_BITS=4064u,
       FORMAT_DIRECT_MAPS=25u, FORMAT_EXTENSION_MAPS=127u };
static void format_word(uint8_t *b,uint32_t index,uint32_t value)
{
    uint8_t *p=b+index*4u;
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);
    p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}
static uint32_t format_read_word(const uint8_t *b,uint32_t index)
{
    const uint8_t *p=b+index*4u;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}
static void format_checksum(uint8_t *b,uint32_t field)
{
    uint32_t sum=0u;
    format_word(b,field,0u);
    for(uint32_t i=0u;i<FORMAT_WORDS;++i)sum+=format_read_word(b,i);
    format_word(b,field,0u-sum);
}
static uint32_t format_metadata_block(uint32_t ordinal,uint32_t root)
{
    const uint32_t block=ordinal+2u;
    return block>=root?block+1u:block;
}
static int format_dos1(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                       const char *label,uint32_t flags)
{
    if(!host_ok(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(s.mounted)return IFS_FS_BUSY;
    if((variant!=0u && variant!=1u) || flags || h->logical_sector_bytes!=FORMAT_BLOCK ||
       h->volume_bytes%FORMAT_BLOCK)return IFS_FS_UNSUPPORTED;
    const uint64_t count=h->volume_bytes/FORMAT_BLOCK;
    /* Bound initial qualification to 512-byte DOS1 media up to 2 GiB.
     * Other geometry and DOS3/5 remain readable, but not format-enabled. */
    if(count<1024u || count>4194304u)return IFS_FS_UNSUPPORTED;
    const uint32_t blocks=(uint32_t)count;
    const uint32_t root=(blocks+1u)/2u;
    const uint32_t maps=(blocks-2u+FORMAT_MAP_BITS-1u)/FORMAT_MAP_BITS;
    const uint32_t extensions=maps>FORMAT_DIRECT_MAPS?
        (maps-FORMAT_DIRECT_MAPS+FORMAT_EXTENSION_MAPS-1u)/FORMAT_EXTENSION_MAPS:0u;
    const uint32_t last=format_metadata_block(maps+extensions-1u,root);
    if(last>=blocks)return IFS_FS_UNSUPPORTED;
    uint8_t name[31]={0};
    uint32_t name_bytes=0u;
    if(label && *label) {
        const size_t bytes=strlen(label);
        if(bytes>60u || utf8_to_latin1(label,(uint32_t)bytes,name,&name_bytes)!=IFS_FS_OK)
            return IFS_FS_INVALID;
        for(uint32_t i=0u;i<name_bytes;++i)
            if(name[i]<32u || name[i]==':')return IFS_FS_INVALID;
    } else { memcpy(name,"Untitled",8u);name_bytes=8u; }

    uint8_t boot[1024]={0},block[FORMAT_BLOCK]={0};
    int rc=ifs_fs_host_write_bytes_v2(h,0u,boot,sizeof(boot));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    /* AFFS bitmap bits enumerate disk blocks beginning at block two:
     * one means free, zero means allocated or outside the volume. */
    for(uint32_t i=0u;i<maps;++i) {
        memset(block,0,sizeof(block));
        for(uint32_t word=0u;word<127u;++word) {
            uint32_t value=0u;
            for(uint32_t bit=0u;bit<32u;++bit) {
                const uint32_t disk=2u+i*FORMAT_MAP_BITS+word*32u+bit;
                if(disk<blocks && disk>last && disk!=root)
                    value|=UINT32_C(1)<<bit;
            }
            format_word(block,word+1u,value);
        }
        format_checksum(block,0u);
        rc=ifs_fs_host_write_bytes_v2(h,
            (uint64_t)format_metadata_block(i,root)*FORMAT_BLOCK,
            block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    /* Each extension has 127 bitmap pointers plus its next extension.
     * No checksum occupies a bitmap-extension pointer slot. */
    for(uint32_t i=0u;i<extensions;++i) {
        memset(block,0,sizeof(block));
        const uint32_t start=FORMAT_DIRECT_MAPS+i*FORMAT_EXTENSION_MAPS;
        for(uint32_t j=0u;j<FORMAT_EXTENSION_MAPS && start+j<maps;++j)
            format_word(block,j,format_metadata_block(start+j,root));
        if(i+1u<extensions)
            format_word(block,127u,format_metadata_block(maps+i+1u,root));
        rc=ifs_fs_host_write_bytes_v2(h,
            (uint64_t)format_metadata_block(maps+i,root)*FORMAT_BLOCK,
            block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    memset(block,0,sizeof(block));
    format_word(block,0u,2u); /* primary header type */
    format_word(block,3u,72u); /* directory hash buckets */
    format_word(block,78u,UINT32_MAX); /* valid allocation bitmap */
    for(uint32_t i=0u;i<maps && i<FORMAT_DIRECT_MAPS;++i)
        format_word(block,79u+i,format_metadata_block(i,root));
    if(extensions)format_word(block,104u,format_metadata_block(maps,root));
    block[432]=(uint8_t)name_bytes;
    memcpy(block+433,name,name_bytes);
    format_word(block,127u,1u); /* root secondary type */
    format_checksum(block,5u);
    rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)root*FORMAT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    memcpy(boot,"DOS",3u);boot[3]=1u;
    format_word(boot,2u,root);
    /* Amiga boot blocks use end-around-carry addition, unlike the
     * ordinary metadata block checksum. */
    uint32_t sum=0u;
    for(uint32_t i=0u;i<256u;++i) {
        const uint32_t word=format_read_word(boot,i);
        const uint32_t next=sum+word;
        sum=next+(next<sum?1u:0u);
    }
    format_word(boot,1u,~sum);
    /* Write the second boot block before making the signature visible. */
    rc=ifs_fs_host_write_bytes_v2(h,FORMAT_BLOCK,boot+FORMAT_BLOCK,FORMAT_BLOCK);
    if(rc!=IFS_FS_OK)return rc;
    rc=ifs_fs_host_write_bytes_v2(h,0u,boot,FORMAT_BLOCK);
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(boot,0,sizeof(boot));
        (void)ifs_fs_host_write_bytes_v2(h,0u,boot,sizeof(boot));
        (void)h->flush(h->context);
        return IFS_FS_IO_ERROR;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    return rc!=IFS_FS_OK?rc:observed==1u?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int guarded_format_dos1(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                       const char *label,uint32_t flags)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(format_dos1(h,variant,label,flags));
}

#endif /* hosted format */

/* Native label bytes are Latin-1; the V2 name is UTF-8. Preserve the
 * precise host failure and never emit a truncated multibyte sequence. */
static int volume_label(void *handle,char *out,uint32_t capacity,uint32_t *bytes)
{
    if (!bytes) return IFS_FS_INVALID;
    *bytes=0u;
    if (!out || !capacity) return IFS_FS_INVALID;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    if (!live(handle)) return v2_leave_operation(IFS_FS_INVALID);
    last_read_error=IFS_FS_OK;
    uint8_t raw[30];
    uint32_t length=0;
    if (ifs_ffs_volume_label(&s.v,raw,&length)!=0)
        return v2_leave_operation(last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT);
    return v2_leave_operation(convert_name(raw,length,out,capacity,bytes));
}
#if !defined(IFS_FFS_FIRMWARE_BUILD)
/* Native read-only ownership verification. FFS DOS1 and DOS3 use a
 * checksum-protected hash tree and bitmap, without directory-cache blocks.
 * Every reachable regular file, directory and continuation block must be
 * uniquely owned and every bitmap allocation must have an owner. This
 * intentionally refuses unimplemented directory-cache/special-object
 * layouts rather than certifying an incomplete walk. */
typedef struct {
    uint8_t *owners;
    int status;
} FfsIntegrity;
static int integrity_claim(void *opaque,uint32_t block)
{
    FfsIntegrity *c=opaque;
    if(s.io.cancelled && s.io.cancelled(s.io.context)) {
        c->status=IFS_FS_BUSY;return -1;
    }
    if(block<2u || block>=s.v.blocks) {
        c->status=IFS_FS_CORRUPT;return -1;
    }
    const uint8_t mask=(uint8_t)(1u<<(block%8u));
    if(c->owners[block/8u]&mask) {
        c->status=IFS_FS_CORRUPT;return -1;
    }
    c->owners[block/8u]|=mask;
    return 0;
}
static int integrity_directory(FfsIntegrity *c,const IfsFfsDropInEntry *dir,
                               unsigned depth)
{
    if(depth>64u)return IFS_FS_UNSUPPORTED;
    const IfsFfsDropInModule *native=ifs_ffs_native_read_ops();
    IfsFfsDropInDirCursor cursor={0};
    for(;;) {
        IfsFfsDropInEntry child={0};uint8_t name[31];
        if(s.io.cancelled && s.io.cancelled(s.io.context))return IFS_FS_BUSY;
        last_read_error=IFS_FS_OK;
        int rc=native->next(&s.v,dir,&cursor,&child,name);
        if(rc==1)return IFS_FS_OK;
        if(rc)return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        if(integrity_claim(c,child.block))return c->status;
        if(child.type==IFS_FFS_DIRECTORY) {
            rc=integrity_directory(c,&child,depth+1u);
            if(rc!=IFS_FS_OK)return rc;
        } else {
            last_read_error=IFS_FS_OK;
            if(ifs_ffs_visit_file_blocks(&s.v,&child,integrity_claim,c))
                return c->status!=IFS_FS_OK?c->status:
                       last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        }
    }
}
static int integrity_check(void *volume,uint32_t flags,uint64_t *errors)
{
    if(!errors)return IFS_FS_INVALID;
    *errors=0u;
    if(!live(volume))return IFS_FS_INVALID;
    if(flags || (s.variant!=1u && s.variant!=3u))return IFS_FS_UNSUPPORTED;
    if(s.write_fault)return IFS_FS_IO_ERROR;
    const uint32_t bs=s.v.io.block_bytes,words=bs/4u,bits=(words-1u)*32u;
    const uint64_t span=(uint64_t)s.v.blocks-2u;
    const uint64_t maps=(span+bits-1u)/bits;
    const uint64_t owner_bytes=((uint64_t)s.v.blocks+7u)/8u;
    if(!maps || maps>SIZE_MAX/sizeof(uint32_t) || owner_bytes>SIZE_MAX)
        return IFS_FS_NO_MEMORY;
    uint32_t *keys=s.io.allocate(s.io.context,(size_t)maps*sizeof(*keys));
    uint8_t *owners=s.io.allocate(s.io.context,(size_t)owner_bytes);
    uint8_t *map=s.io.allocate(s.io.context,bs);
    int rc=IFS_FS_NO_MEMORY;
    if(!keys || !owners || !map)goto done;
    memset(owners,0,(size_t)owner_bytes);
    uint64_t free_bytes=0u;
    rc=scan_free_bytes(&free_bytes,keys,owners);
    if(rc!=IFS_FS_OK)goto done;
    FfsIntegrity state={owners,IFS_FS_OK};
    const IfsFfsDropInEntry root=ifs_ffs_native_read_ops()->root(&s.v);
    rc=integrity_directory(&state,&root,0u);
    if(rc!=IFS_FS_OK)goto done;
    /* Match ownership against the real allocation bits, not just counts.
     * A referenced free block or an allocated unreachable block is corrupt. */
    for(uint64_t i=0u;i<maps;++i) {
        if(s.io.cancelled && s.io.cancelled(s.io.context)){rc=IFS_FS_BUSY;goto done;}
        rc=bitmap_read(keys[i],map);
        if(rc!=IFS_FS_OK)goto done;
        if(!bitmap_checksum(map,words)){rc=IFS_FS_CORRUPT;goto done;}
        const uint64_t start=i*bits;
        const uint32_t valid=(uint32_t)(span-start<bits?span-start:bits);
        for(uint32_t bit=0u;bit<valid;++bit) {
            const uint32_t block=(uint32_t)(2u+start+bit);
            const int owned=(owners[block/8u]&(1u<<(block%8u)))!=0u;
            const int is_free=(bitmap_word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u)))!=0u;
            if(owned==is_free){rc=IFS_FS_CORRUPT;goto done;}
        }
    }
    rc=IFS_FS_OK;
done:
    if(map)s.io.release(s.io.context,map);
    if(owners)s.io.release(s.io.context,owners);
    if(keys)s.io.release(s.io.context,keys);
    if(rc==IFS_FS_CORRUPT)*errors=1u;
    return rc;
}
static int guarded_integrity_check(void *volume,uint32_t flags,uint64_t *errors)
{
    if(errors)*errors=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(integrity_check(volume,flags,errors));
}

/* Native, read-only planning. The bitmap and namespace are verified before
 * any placement decision. Only genuinely free blocks are chosen as new
 * destinations; the original data remains allocated throughout planning.
 * This is a conservative copy-before-switch strategy. A future durable
 * execution transaction must revalidate this plan under a writable lease.
 * No EXECUTE, VERIFY or RECOVER capability is claimed by this planner. */
typedef struct {
    uint32_t header,source_first,destination,blocks,reserve;
    uint32_t bytes,digest;
    int relocate;
} FfsRelocationEntry;
struct FfsRelocationPlan {
    uintptr_t generation;
    uint32_t policy,journal_first,journal_blocks;
    int executed;
    size_t count,capacity;
    FfsRelocationEntry *entries;
};
typedef struct {
    FfsRelocationPlan *plan;
    uint8_t *free;
} FfsPlanner;
static void free_relocation_plan(FfsRelocationPlan *plan)
{
    if(!plan)return;
    if(plan->entries)s.io.release(s.io.context,plan->entries);
    s.io.release(s.io.context,plan);
}
static int planner_free(const FfsPlanner *p,uint32_t block)
{
    return (p->free[block/8u]&(1u<<(block%8u)))!=0u;
}
static void planner_take(FfsPlanner *p,uint32_t first,uint32_t count)
{
    for(uint32_t i=0u;i<count;++i) {
        const uint32_t block=first+i;
        p->free[block/8u]&=(uint8_t)~(1u<<(block%8u));
    }
}
static int planner_run(FfsPlanner *p,uint32_t count,uint32_t *first)
{
    uint32_t start=2u,run=0u;
    for(uint32_t block=2u;block<s.v.blocks;++block) {
        if(!(block%4096u) && s.io.cancelled && s.io.cancelled(s.io.context))
            return IFS_FS_BUSY;
        if(planner_free(p,block)) {
            if(!run)start=block;
            if(++run==count){*first=start;return IFS_FS_OK;}
        } else run=0u;
    }
    return IFS_FS_NO_SPACE;
}
static int planner_append(FfsPlanner *p,FfsRelocationEntry entry)
{
    FfsRelocationPlan *plan=p->plan;
    if(plan->count==plan->capacity) {
        const size_t next=plan->capacity?plan->capacity*2u:32u;
        if(next<=plan->capacity || next>SIZE_MAX/sizeof(*plan->entries))
            return IFS_FS_NO_MEMORY;
        FfsRelocationEntry *entries=s.io.allocate(s.io.context,next*sizeof(*entries));
        if(!entries)return IFS_FS_NO_MEMORY;
        if(plan->count)memcpy(entries,plan->entries,plan->count*sizeof(*entries));
        if(plan->entries)s.io.release(s.io.context,plan->entries);
        plan->entries=entries;plan->capacity=next;
    }
    plan->entries[plan->count++]=entry;
    return IFS_FS_OK;
}
static int planner_file(FfsPlanner *p,const IfsFfsDropInEntry *file)
{
    uint32_t first=0u,blocks=0u;
    int packed=0;
    last_read_error=IFS_FS_OK;
    if(ifs_ffs_file_layout(&s.v,file,&first,&blocks,&packed))
        return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    if(!blocks)return IFS_FS_OK;
    const uint32_t reserve=p->plan->policy?(blocks/10u+(blocks%10u!=0u)):0u;
    if(blocks>UINT32_MAX-reserve)return IFS_FS_NO_SPACE;
    uint32_t destination=first;
    /* Preserve a file already satisfying the policy, but reserve the
     * adjacent gap in the private planning bitmap so later files cannot
     * overlap it. The native allocation bitmap is never modified here. */
    int retain=packed;
    if(retain && reserve) {
        const uint64_t end=(uint64_t)first+blocks;
        if(end>s.v.blocks || reserve>s.v.blocks-end)retain=0;
        else for(uint32_t j=0u;j<reserve;++j)
            if(!planner_free(p,(uint32_t)end+j)){retain=0;break;}
    }
    if(retain) {
        if(reserve)planner_take(p,first+blocks,reserve);
    } else {
        int rc=planner_run(p,blocks+reserve,&destination);
        if(rc!=IFS_FS_OK)return rc;
        planner_take(p,destination,blocks+reserve);
    }
    return planner_append(p,(FfsRelocationEntry){
        .header=file->block,.source_first=first,.destination=destination,
        .blocks=blocks,.reserve=reserve,.bytes=file->size,.relocate=!retain});
}
static int planner_directory(FfsPlanner *p,const IfsFfsDropInEntry *dir,
                              unsigned depth)
{
    if(depth>64u)return IFS_FS_UNSUPPORTED;
    IfsFfsDropInDirCursor cursor={0};
    const IfsFfsDropInModule *native=ifs_ffs_native_read_ops();
    for(;;) {
        IfsFfsDropInEntry child={0};uint8_t name[31];
        if(s.io.cancelled && s.io.cancelled(s.io.context))return IFS_FS_BUSY;
        last_read_error=IFS_FS_OK;
        int rc=native->next(&s.v,dir,&cursor,&child,name);
        if(rc==1)return IFS_FS_OK;
        if(rc)return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        rc=child.type==IFS_FFS_DIRECTORY?planner_directory(p,&child,depth+1u):
            planner_file(p,&child);
        if(rc!=IFS_FS_OK)return rc;
    }
}
static int relocation_plan(void *volume,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!live(volume) || !path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    if(s.plan)return IFS_FS_BUSY;
    uint64_t errors=0u;
    int rc=integrity_check(volume,0u,&errors);
    if(rc!=IFS_FS_OK)return rc;
    IfsFfsDropInEntry target={0};const char *name;uint32_t length;
    last_read_error=IFS_FS_OK;
    rc=path_entry(path,&target,&name,&length);
    if(rc!=IFS_FS_OK)return rc;
    const uint32_t bs=s.v.io.block_bytes,words=bs/4u,bits=(words-1u)*32u;
    const uint64_t span=(uint64_t)s.v.blocks-2u,maps=(span+bits-1u)/bits;
    const uint64_t bitmap_bytes=((uint64_t)s.v.blocks+7u)/8u;
    if(!maps || maps>SIZE_MAX/sizeof(uint32_t) || bitmap_bytes>SIZE_MAX)
        return IFS_FS_NO_MEMORY;
    uint32_t *keys=s.io.allocate(s.io.context,(size_t)maps*sizeof(*keys));
    uint8_t *bitmap=s.io.allocate(s.io.context,(size_t)bitmap_bytes);
    uint8_t *page=s.io.allocate(s.io.context,bs);
    FfsRelocationPlan *plan=s.io.allocate(s.io.context,sizeof(*plan));
    if(plan)*plan=(FfsRelocationPlan){.generation=s.generation,.policy=policy};
    rc=IFS_FS_NO_MEMORY;
    if(!keys || !bitmap || !page || !plan)goto done;
    memset(bitmap,0,(size_t)bitmap_bytes);
    uint64_t free_bytes=0u;
    rc=scan_free_bytes(&free_bytes,keys,NULL);
    if(rc!=IFS_FS_OK)goto done;
    for(uint64_t i=0u;i<maps;++i) {
        if(s.io.cancelled && s.io.cancelled(s.io.context)){rc=IFS_FS_BUSY;goto done;}
        rc=bitmap_read(keys[i],page);
        if(rc!=IFS_FS_OK)goto done;
        if(!bitmap_checksum(page,words)){rc=IFS_FS_CORRUPT;goto done;}
        const uint64_t start=i*bits;
        const uint32_t valid=(uint32_t)(span-start<bits?span-start:bits);
        for(uint32_t bit=0u;bit<valid;++bit)
            if(bitmap_word(page,1u+bit/32u)&(UINT32_C(1)<<(bit%32u))) {
                const uint32_t block=(uint32_t)(start+bit+2u);
                bitmap[block/8u]|=(uint8_t)(1u<<(block%8u));
            }
    }
    FfsPlanner planner={plan,bitmap};
    rc=target.type==IFS_FFS_DIRECTORY?planner_directory(&planner,&target,0u):
        planner_file(&planner,&target);
    if(rc==IFS_FS_OK) {
        /* Journal sectors are selected from the same private free map,
         * after all destinations and growth gaps are excluded. One run
         * is reused for successive independent file transactions. */
        uint32_t pages=0u;
        for(size_t i=0u;i<plan->count;++i)
            if(plan->entries[i].relocate) {
                uint32_t need=(plan->entries[i].blocks+71u)/72u;
                if(need>pages)pages=need;
            }
        if(pages && bs==512u) {
            uint64_t need=2u*(maps+pages);
            if(need>UINT32_MAX)rc=IFS_FS_NO_SPACE;
            else {
                plan->journal_blocks=(uint32_t)need;
                rc=planner_run(&planner,plan->journal_blocks,&plan->journal_first);
            }
        }
    }
    if(rc==IFS_FS_OK){s.plan=plan;*out=plan;plan=NULL;}
done:
    free_relocation_plan(plan);
    if(page)s.io.release(s.io.context,page);
    if(bitmap)s.io.release(s.io.context,bitmap);
    if(keys)s.io.release(s.io.context,keys);
    return rc;
}
static int guarded_relocation_plan(void *volume,const char *path,uint32_t policy,void **out)
{
    if(out)*out=NULL;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(relocation_plan(volume,path,policy,out));
}
static void guarded_release_relocation_plan(void *volume,void *plan)
{
    if(!v2_enter_operation())return;
    if(session_live(volume) && plan && s.plan==plan) {
        free_relocation_plan(s.plan);s.plan=NULL;
    }
    (void)v2_leave_operation(IFS_FS_OK);
}

/* Each redo record consists of one checksummed descriptor and one full
 * native metadata sector. File data is copied to free destinations and
 * verified BEFORE publishing the locator; original data remains allocated.
 * The journal itself occupies temporary free blocks excluded from all
 * planned destinations. Replaying a committed locator is idempotent.
 * No old data is overwritten, and no filesystem worker is involved. */
static int relocation_sector(uint32_t block,const uint8_t *data)
{
    return ifs_fs_host_write_bytes_v2(&s.io,(uint64_t)block*512u,data,512u);
}
static int relocation_flush(void)
{
    return s.io.flush(s.io.context)==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int relocation_digest(uint32_t start,uint32_t blocks,uint32_t *digest)
{
    uint8_t data[512];
    uint32_t crc=UINT32_MAX;
    for(uint32_t i=0u;i<blocks;++i) {
        if(s.io.cancelled && s.io.cancelled(s.io.context))return IFS_FS_BUSY;
        int rc=bitmap_read(start+i,data);if(rc!=IFS_FS_OK)return rc;
        crc=relocation_crc(crc,data,sizeof(data));
    }
    *digest=crc;return IFS_FS_OK;
}
static int relocation_record(uint32_t start,uint32_t index,uint32_t target,
                             const uint8_t data[512],uint32_t *digest)
{
    uint8_t tag[512]={0};
    grow_word(tag,0u,target);
    grow_word(tag,1u,relocation_crc(UINT32_MAX,data,512u));
    grow_word(tag,2u,index);
    grow_word(tag,127u,relocation_crc(UINT32_MAX,tag,508u));
    int rc=relocation_sector(start+index*2u,tag);
    if(rc==IFS_FS_OK)rc=relocation_sector(start+index*2u+1u,data);
    if(rc==IFS_FS_OK) {
        *digest=relocation_crc(*digest,tag,512u);
        *digest=relocation_crc(*digest,data,512u);
    }
    return rc;
}
static int relocation_read_record(uint32_t start,uint32_t index,
                                  uint32_t *target,uint8_t data[512],
                                  uint32_t *digest)
{
    uint8_t tag[512];
    int rc=bitmap_read(start+index*2u,tag);
    if(rc==IFS_FS_OK)rc=bitmap_read(start+index*2u+1u,data);
    if(rc!=IFS_FS_OK)return rc;
    if(bitmap_word(tag,2u)!=index ||
       bitmap_word(tag,127u)!=relocation_crc(UINT32_MAX,tag,508u) ||
       bitmap_word(tag,1u)!=relocation_crc(UINT32_MAX,data,512u))
        return IFS_FS_CORRUPT;
    *target=bitmap_word(tag,0u);
    if(digest) {
        *digest=relocation_crc(*digest,tag,512u);
        *digest=relocation_crc(*digest,data,512u);
    }
    return IFS_FS_OK;
}
/* Validate the entire journal before replaying any sector. Bounds, unique
 * targets, native bitmap geometry and the complete replacement file chain
 * are independently checked; the marker cannot address boot/root blocks,
 * journal blocks or destination data. The full data copy is checked too. */
static int relocation_replay(const uint8_t marker[512],uint64_t *repaired)
{
    const uint32_t start=bitmap_word(marker,2u),records=bitmap_word(marker,3u);
    const uint32_t file=bitmap_word(marker,4u),dest=bitmap_word(marker,5u);
    const uint32_t blocks=bitmap_word(marker,6u),pages=bitmap_word(marker,7u);
    const uint32_t maps=(s.v.blocks-2u+GROW_BITS-1u)/GROW_BITS;
    const uint32_t exts=maps>25u?(maps-25u+126u)/127u:0u;
    const uint32_t fixed=grow_metadata(maps+exts-1u);
    if(!blocks || pages!=(blocks+71ull)/72u || records<pages ||
       records-pages>maps || start<=fixed || start>=s.v.blocks ||
       records>(s.v.blocks-start)/2u || dest<=fixed || dest>=s.v.blocks ||
       blocks>s.v.blocks-dest || file<=fixed || file>=s.v.blocks ||
       file==s.v.root_block || bitmap_word(marker,10u)!=s.v.blocks ||
       bitmap_word(marker,11u)!=s.v.root_block ||
       (s.v.root_block>=start && s.v.root_block-start<2u*records) ||
       (s.v.root_block>=dest && s.v.root_block-dest<blocks) ||
       ((uint64_t)start+2u*records>dest && (uint64_t)dest+blocks>start))
        return IFS_FS_CORRUPT;
    uint32_t actual=0u;
    int rc=relocation_digest(dest,blocks,&actual);
    if(rc!=IFS_FS_OK)return rc;
    if(actual!=bitmap_word(marker,9u))return IFS_FS_CORRUPT;
    const size_t owner_bytes=((size_t)s.v.blocks+7u)/8u;
    uint8_t *seen=s.io.allocate(s.io.context,owner_bytes);
    if(!seen)return IFS_FS_NO_MEMORY;
    memset(seen,0,owner_bytes);
    uint32_t crc=UINT32_MAX,expected_page=file,logical=0u;
    uint8_t raw[512];
    for(uint32_t i=0u;i<records;++i) {
        uint32_t target;
        rc=relocation_read_record(start,i,&target,raw,&crc);
        if(rc!=IFS_FS_OK)goto done;
        if(target<2u || target>=s.v.blocks || target==s.v.root_block ||
           (target>=start && target-start<2u*records) ||
           (target>=dest && target-dest<blocks) ||
           (seen[target/8u]&(1u<<(target%8u))) || !bitmap_checksum(raw,128u)) {
            rc=IFS_FS_CORRUPT;goto done;
        }
        seen[target/8u]|=(uint8_t)(1u<<(target%8u));
        if(i<pages) {
            uint32_t count=blocks-logical;if(count>72u)count=72u;
            if(target<=fixed || target!=expected_page ||
               bitmap_word(raw,0u)!=(i?16u:2u) ||
               bitmap_word(raw,1u)!=target || bitmap_word(raw,2u)!=count ||
               bitmap_word(raw,127u)!=(uint32_t)-3 ||
               (i && bitmap_word(raw,125u)!=file) ||
               (!i && bitmap_word(raw,4u)!=dest)) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            for(uint32_t k=0u;k<72u;++k)
                if(bitmap_word(raw,77u-k)!=(k<count?dest+logical+k:0u)) {
                    rc=IFS_FS_CORRUPT;goto done;
                }
            logical+=count;expected_page=bitmap_word(raw,126u);
            if((i+1u==pages)!=(expected_page==0u)) {
                rc=IFS_FS_CORRUPT;goto done;
            }
        } else {
            /* Formatter's bitmap keys are contiguous around the root. */
            uint32_t ordinal=target-2u-(target>s.v.root_block);
            if(ordinal>=maps || grow_metadata(ordinal)!=target) {
                rc=IFS_FS_CORRUPT;goto done;
            }
            const uint64_t base=2u+(uint64_t)ordinal*GROW_BITS;
            for(uint32_t bit=0u;bit<GROW_BITS;++bit) {
                const uint64_t block=base+bit;
                if((block<=fixed || block==s.v.root_block || block>=s.v.blocks ||
                    (block>=dest && block-dest<blocks)) &&
                   (bitmap_word(raw,1u+bit/32u)&(UINT32_C(1)<<(bit%32u)))) {
                    rc=IFS_FS_CORRUPT;goto done;
                }
            }
        }
    }
    if(crc!=bitmap_word(marker,8u)){rc=IFS_FS_CORRUPT;goto done;}
    /* After publication cancellation cannot abandon a half-applied
     * transaction. The durable locator survives every I/O failure. */
    for(uint32_t i=0u;i<records;++i) {
        uint32_t target;
        rc=relocation_read_record(start,i,&target,raw,NULL);
        if(rc==IFS_FS_OK)rc=relocation_sector(target,raw);
        if(rc!=IFS_FS_OK)goto done;
    }
    rc=relocation_flush();if(rc!=IFS_FS_OK)goto done;
    /* Validate the fully replayed native tree before clearing authority.
     * Temporarily allow only this internal check, not external operations. */
    s.recovery_pending=0;s.write_fault=0;
    uint64_t errors=0u;
    rc=integrity_check((void *)s.handle,0u,&errors);
    s.recovery_pending=1;
    if(rc!=IFS_FS_OK)goto done;
    memset(raw,0,sizeof(raw));
    rc=relocation_sector(1u,raw);
    if(rc==IFS_FS_OK)rc=relocation_flush();
    if(rc==IFS_FS_OK) {
        s.recovery_pending=0;s.growth_ready=0;s.write_fault=0;
        if(repaired)*repaired=records;
    }
done:
    s.io.release(s.io.context,seen);
    return rc;
}
static int relocation_one(FfsRelocationPlan *plan,FfsRelocationEntry *e)
{
    const uint32_t pages=(e->blocks+71u)/72u;
    const uint32_t maps=(s.v.blocks-2u+GROW_BITS-1u)/GROW_BITS;
    uint32_t *data=s.io.allocate(s.io.context,(size_t)e->blocks*sizeof(*data));
    uint32_t *keys=s.io.allocate(s.io.context,(size_t)pages*sizeof(*keys));
    uint8_t *bitmaps=s.io.allocate(s.io.context,(size_t)maps*512u);
    uint8_t *dirty=s.io.allocate(s.io.context,maps);
    int rc=IFS_FS_NO_MEMORY;
    if(!data || !keys || !bitmaps || !dirty)goto done;
    memset(dirty,0,maps);
    IfsFfsDropInEntry file={.block=e->header,.size=e->bytes,.type=IFS_FFS_FILE};
    last_read_error=IFS_FS_OK;
    if(ifs_ffs_relocation_blocks(&s.v,&file,data,e->blocks,keys,pages)) {
        rc=last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;goto done;
    }
    if(data[0]!=e->source_first){rc=IFS_FS_CORRUPT;goto done;}
    for(uint32_t i=0u;i<maps;++i) {
        rc=bitmap_read(grow_metadata(i),bitmaps+(size_t)i*512u);
        if(rc!=IFS_FS_OK)goto done;
        if(!bitmap_checksum(bitmaps+(size_t)i*512u,128u)){rc=IFS_FS_CORRUPT;goto done;}
    }
    /* Check destination, growth gap and redo scratch against the live
     * native bitmap before touching any free data sector. */
    for(unsigned range=0u;range<2u;++range) {
        uint32_t first=range?plan->journal_first:e->destination;
        uint32_t count=range?plan->journal_blocks:e->blocks+e->reserve;
        if(first<s.growth_first || first>=s.v.blocks || count>s.v.blocks-first) {
            rc=IFS_FS_CORRUPT;goto done;
        }
        for(uint32_t j=0u;j<count;++j) {
            uint32_t bit=first+j-2u;
            const uint8_t *map=bitmaps+(size_t)(bit/GROW_BITS)*512u;
            bit%=GROW_BITS;
            if(!(bitmap_word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u)))) {
                rc=IFS_FS_CORRUPT;goto done;
            }
        }
    }
    uint8_t raw[512],copy[512];
    uint32_t digest=UINT32_MAX;
    for(uint32_t i=0u;i<e->blocks;++i) {
        if(s.io.cancelled && s.io.cancelled(s.io.context)){rc=IFS_FS_BUSY;goto done;}
        uint32_t old=data[i]-2u,newbit=e->destination+i-2u;
        uint32_t oi=old/GROW_BITS,ni=newbit/GROW_BITS;
        old%=GROW_BITS;newbit%=GROW_BITS;
        uint8_t *om=bitmaps+(size_t)oi*512u,*nm=bitmaps+(size_t)ni*512u;
        uint32_t ow=1u+old/32u,nw=1u+newbit/32u;
        uint32_t ob=UINT32_C(1)<<(old%32u),nb=UINT32_C(1)<<(newbit%32u);
        if(bitmap_word(om,ow)&ob || !(bitmap_word(nm,nw)&nb)) {
            rc=IFS_FS_CORRUPT;goto done;
        }
        grow_word(om,ow,bitmap_word(om,ow)|ob);
        grow_word(nm,nw,bitmap_word(nm,nw)&~nb);
        dirty[oi]=dirty[ni]=1;
        rc=bitmap_read(data[i],raw);
        if(rc==IFS_FS_OK)rc=relocation_sector(e->destination+i,raw);
        if(rc==IFS_FS_OK)rc=bitmap_read(e->destination+i,copy);
        if(rc!=IFS_FS_OK)goto done;
        if(memcmp(raw,copy,512u)){rc=IFS_FS_IO_ERROR;goto done;}
        digest=relocation_crc(digest,raw,512u);
    }
    /* Durable data before durable redo, then durable publication. */
    rc=relocation_flush();if(rc!=IFS_FS_OK)goto done;
    uint32_t records=0u,crc=UINT32_MAX,logical=0u;
    for(uint32_t i=0u;i<pages;++i) {
        rc=bitmap_read(keys[i],raw);if(rc!=IFS_FS_OK)goto done;
        if(!bitmap_checksum(raw,128u)){rc=IFS_FS_CORRUPT;goto done;}
        const uint32_t count=e->blocks-logical<72u?e->blocks-logical:72u;
        for(uint32_t k=0u;k<72u;++k)
            grow_word(raw,77u-k,k<count?e->destination+logical+k:0u);
        if(!i || bitmap_word(raw,4u))grow_word(raw,4u,e->destination+logical);
        grow_checksum(raw,5u);
        rc=relocation_record(plan->journal_first,records++,keys[i],raw,&crc);
        if(rc!=IFS_FS_OK)goto done;
        logical+=count;
    }
    for(uint32_t i=0u;i<maps;++i)if(dirty[i]) {
        uint8_t *map=bitmaps+(size_t)i*512u;grow_checksum(map,0u);
        rc=relocation_record(plan->journal_first,records++,grow_metadata(i),map,&crc);
        if(rc!=IFS_FS_OK)goto done;
    }
    if(records>plan->journal_blocks/2u){rc=IFS_FS_CORRUPT;goto done;}
    rc=relocation_flush();if(rc!=IFS_FS_OK)goto done;
    if(s.io.cancelled && s.io.cancelled(s.io.context)){rc=IFS_FS_BUSY;goto done;}
    memset(raw,0,sizeof(raw));memcpy(raw,relocation_magic,sizeof(relocation_magic));
    grow_word(raw,2u,plan->journal_first);grow_word(raw,3u,records);
    grow_word(raw,4u,e->header);grow_word(raw,5u,e->destination);
    grow_word(raw,6u,e->blocks);grow_word(raw,7u,pages);
    grow_word(raw,8u,crc);grow_word(raw,9u,digest);
    grow_word(raw,10u,s.v.blocks);grow_word(raw,11u,s.v.root_block);
    grow_word(raw,127u,relocation_crc(UINT32_MAX,raw,508u));
    s.recovery_pending=1;
    rc=relocation_sector(1u,raw);
    if(rc==IFS_FS_OK)rc=relocation_flush();
    if(rc==IFS_FS_OK)rc=relocation_replay(raw,NULL);
    if(rc==IFS_FS_OK)e->digest=digest;
done:
    if(dirty)s.io.release(s.io.context,dirty);
    if(bitmaps)s.io.release(s.io.context,bitmaps);
    if(keys)s.io.release(s.io.context,keys);
    if(data)s.io.release(s.io.context,data);
    return rc;
}
static int relocation_execute(void *volume,void *opaque,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0u;
    if(!live(volume) || !opaque || opaque!=s.plan || s.plan->executed)
        return IFS_FS_INVALID;
    int rc=grow_profile(volume);if(rc!=IFS_FS_OK)return rc;
    if(s.io.physical_sector_bytes!=512u)return IFS_FS_UNSUPPORTED;
    uint8_t marker[512];int pending=0;
    rc=relocation_marker(marker,&pending);
    if(rc!=IFS_FS_OK || pending)return rc!=IFS_FS_OK?rc:IFS_FS_BUSY;
    uint64_t errors=0u;
    rc=integrity_check(volume,0u,&errors);if(rc!=IFS_FS_OK)return rc;
    /* Any attempted execution consumes its plan even on failure. */
    s.plan->executed=-1;
    for(size_t i=0u;i<s.plan->count;++i) {
        FfsRelocationEntry *e=&s.plan->entries[i];
        if(e->relocate) {
            rc=relocation_one(s.plan,e);if(rc!=IFS_FS_OK)return rc;
            *moved+=(uint64_t)e->blocks*512u;
        }
    }
    s.plan->executed=1;
    return IFS_FS_OK;
}
static int relocation_verify(void *volume,void *opaque)
{
    if(!live(volume) || !opaque || opaque!=s.plan || s.plan->executed!=1)
        return IFS_FS_INVALID;
    uint64_t errors=0u;int rc=integrity_check(volume,0u,&errors);
    if(rc!=IFS_FS_OK)return rc;
    for(size_t i=0u;i<s.plan->count;++i) {
        FfsRelocationEntry *e=&s.plan->entries[i];
        IfsFfsDropInEntry file={.block=e->header,.size=e->bytes,.type=IFS_FFS_FILE};
        uint32_t first,blocks;int packed;
        last_read_error=IFS_FS_OK;
        if(ifs_ffs_file_layout(&s.v,&file,&first,&blocks,&packed))
            return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        if(!packed || first!=e->destination || blocks!=e->blocks)return IFS_FS_CORRUPT;
        if(e->relocate) {
            uint32_t digest;
            rc=relocation_digest(first,blocks,&digest);
            if(rc!=IFS_FS_OK)return rc;
            if(digest!=e->digest)return IFS_FS_CORRUPT;
        }
        uint32_t loaded=UINT32_MAX;uint8_t map[512];
        for(uint32_t j=0u;j<e->reserve;++j) {
            uint32_t bit=first+blocks+j-2u,index=bit/GROW_BITS;
            if(index!=loaded) {
                rc=bitmap_read(grow_metadata(index),map);if(rc!=IFS_FS_OK)return rc;
                if(!bitmap_checksum(map,128u))return IFS_FS_CORRUPT;
                loaded=index;
            }
            bit%=GROW_BITS;
            if(!(bitmap_word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u))))
                return IFS_FS_CORRUPT;
        }
    }
    return IFS_FS_OK;
}
static int relocation_recover(void *volume,uint32_t flags,uint64_t *repaired)
{
    if(!repaired)return IFS_FS_INVALID;
    *repaired=0u;
    if(!session_live(volume) || flags)return IFS_FS_INVALID;
    if(s.io.logical_sector_bytes!=512u || s.io.physical_sector_bytes!=512u)
        return IFS_FS_UNSUPPORTED;
    if((s.io.capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(s.io.capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(s.io.capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    uint8_t marker[512];int pending=0;
    int rc=relocation_marker(marker,&pending);
    if(rc!=IFS_FS_OK)return rc;
    if(!pending) {
        /* A failed locator-clear flush may have completed the write.
         * Confirm native integrity before unblocking the session. */
        s.recovery_pending=0;s.write_fault=0;
        uint64_t errors=0u;rc=integrity_check(volume,0u,&errors);
        if(rc!=IFS_FS_OK)s.recovery_pending=1;
        return rc;
    }
    s.recovery_pending=1;
    return relocation_replay(marker,repaired);
}
static int guarded_relocation_execute(void *volume,void *plan,uint64_t *moved)
{
    if(moved)*moved=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(relocation_execute(volume,plan,moved));
}
static int guarded_relocation_verify(void *volume,void *plan)
{
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(relocation_verify(volume,plan));
}
static int guarded_relocation_recover(void *volume,uint32_t flags,uint64_t *repaired)
{
    if(repaired)*repaired=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(relocation_recover(volume,flags,repaired));
}
#endif

static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,
             IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC|IFS_FS_VOLUME_LABEL_GET
#if !defined(IFS_FFS_FIRMWARE_BUILD)
             |IFS_FS_VOLUME_FORMAT|IFS_FS_VOLUME_CHECK
#endif
    },
    .query=guarded_v2_query_volume,.sync=guarded_v2_sync_volume,.label=volume_label
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    ,.format=guarded_format_dos1,.check=guarded_integrity_check
#endif
};
static const IfsFsGroupHeaderV2 *query_group(uint32_t id,
                                               uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2)
        return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag_map_ops.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation_ops.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:ffs:common2";
static const IfsFsModuleApiV2 api={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ffs",
#if defined(IFS_FFS_FIRMWARE_BUILD)
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
    IFS_FS_PROFILE_HOSTED,
#endif
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION),query_group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
