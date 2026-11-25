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

static struct {
    IfsFfsDropInVolume v;
    IfsFsHostRuntimeV2 io;
    uintptr_t generation,handle;
    uint32_t variant;
    int mounted;
    int growth_ready,write_fault;
    uint32_t growth_first,growth_hint;
} s;
/* The native FFS reader returns 0/-1. Preserve the precise Common V2
 * host failure instead of treating an unavailable sector as missing files,
 * corrupt metadata or a different filesystem. One active session per SO. */
static int last_read_error=IFS_FS_OK;
static int live(void *v){
    return s.mounted && s.handle && (uintptr_t)v==s.handle && s.v.mounted;
}
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
static int measured_free_bytes(uint64_t *out)
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
    *out=free_blocks*bs;
    rc=IFS_FS_OK;
done:
    s.io.release(s.io.context,seen);
    s.io.release(s.io.context,buf);
    return rc;
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
    s.growth_ready=0;s.write_fault=0;s.growth_first=0;s.growth_hint=0;
    s.handle=++s.generation;
    info->variant=variant;info->max_name_bytes=60u;
    info->block_bytes=s.v.io.block_bytes;
    info->total_bytes=s.v.io.volume_bytes;
    /* The namespace remains readable even when its allocation map is
     * dirty or missing. A subsequent explicit volume query reports the
     * allocation error; mount itself keeps the conservative zero value. */
    r=measured_free_bytes(&info->free_bytes);
    if(r!=IFS_FS_OK && r!=IFS_FS_CORRUPT && r!=IFS_FS_UNSUPPORTED) {
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
    if(!live(volume))return IFS_FS_INVALID;
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
    int rc=grow_profile(volume);if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE || !path || path[0]!='/' || !path[1] ||
       strchr(path+1,'/') || !strcmp(path,"/.") || !strcmp(path,"/.."))
        return IFS_FS_UNSUPPORTED;
    size_t bytes=strlen(path+1);
    if(bytes>60u)return IFS_FS_INVALID;
    uint8_t name[31];uint32_t n;
    rc=utf8_to_latin1(path+1,(uint32_t)bytes,name,&n);
    if(rc!=IFS_FS_OK)return rc;
    if(ifs_ffs_validate_name(name,n,1)!=IFS_FFS_NAME_OK)return IFS_FS_INVALID;
    IfsFfsDropInEntry found={0};const char *base;uint32_t len;
    last_read_error=IFS_FS_OK;rc=path_entry(path,&found,&base,&len);
    if(rc==IFS_FS_OK)return IFS_FS_EXISTS;
    if(rc!=IFS_FS_NOT_FOUND)return rc;
    FfsTxn t={.hint=s.growth_hint};FfsPatch *root,*file;
    rc=grow_patch(&t,s.v.root_block,&root);if(rc!=IFS_FS_OK)goto done;
    if(!bitmap_checksum(root->now,128u) || bitmap_word(root->now,78u)!=UINT32_MAX) {
        rc=IFS_FS_CORRUPT;goto done;
    }
    rc=grow_alloc(&t,&file);if(rc!=IFS_FS_OK)goto done;
    const uint32_t bucket=ifs_ffs_directory_hash(name,n,72u,0);
    grow_word(file->now,0,2u);grow_word(file->now,1,file->number);
    grow_word(file->now,124,bitmap_word(root->now,6u+bucket));
    grow_word(file->now,125,s.v.root_block);
    grow_word(file->now,127,(uint32_t)-3);
    file->now[432]=(uint8_t)n;memcpy(file->now+433,name,n);
    grow_checksum(file->now,5);file->dirty=1;
    grow_word(root->now,6u+bucket,file->number);grow_checksum(root->now,5);root->dirty=1;
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
 * PLAN/EXECUTE/VERIFY/RECOVER are not qualified and stay absent. */
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
    for(uint64_t i=0u;i<count;++i) {
        uint64_t offset;
        if(ifs_ffs_data_block(&s.v,&file,(uint32_t)(first+i),
                              &physical[i])!=0)
            return last_read_error!=IFS_FS_OK?
                last_read_error:IFS_FS_CORRUPT;
        offset=(uint64_t)physical[i]*s.v.io.block_bytes;
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

static const IfsFsDefragOpsV2 defrag_map_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=guarded_map_file
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
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,
             IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC|IFS_FS_VOLUME_LABEL_GET
#if !defined(IFS_FFS_FIRMWARE_BUILD)
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query=guarded_v2_query_volume,.sync=guarded_v2_sync_volume,.label=volume_label
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    ,.format=guarded_format_dos1
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
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),query_group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
