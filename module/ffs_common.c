/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 over source-owned DOS1/3/5 FFS read/overwrite engines.
 * The canonical FFS drop-in engine owns all on-disk parsing and mutation.
 */
#include "ffs_module.h"
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <string.h>
static struct {
    IfsFfsDropInVolume v;
    IfsFsHostRuntimeV2 io;
    uintptr_t generation,handle;
    uint32_t variant;
    int mounted;
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
    s.handle=++s.generation;
    info->variant=variant;info->max_name_bytes=60u;
    info->block_bytes=s.v.io.block_bytes;
    info->total_bytes=s.v.io.volume_bytes;
    info->free_bytes=0u; /* Not provided by the current read engine. */
    *volume=(void *)s.handle;
    return IFS_FS_OK;
}
static int unmount(void *volume){
    if(!live(volume))return IFS_FS_INVALID;
    s.v.mounted=0u;s.mounted=0;s.handle=0;
    return IFS_FS_OK;
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
#if !defined(IFS_FFS_FIRMWARE_BUILD)
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
    if(offset>e.size||len>e.size-offset)return IFS_FS_UNSUPPORTED;
    IfsFfsWritableIo io={0};
    io.read=s.v.io;io.write_at=v2_write;io.flush=v2_flush;
    r=ifs_ffs_native_write_ops()->overwrite_existing(&s.v,&e,&io,
        (uint32_t)offset,data,len);
    return r==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}
#endif /* !IFS_FFS_FIRMWARE_BUILD */

/* V2 provides only real volume inquiry; no unsupported repair/format claim. */
static int v2_query_volume(void *volume,IfsFsVolumeInfoV2 *info){
    if(!live(volume)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    info->variant=s.variant;
    info->block_bytes=s.v.io.block_bytes;
    info->max_name_bytes=60u;
    info->total_bytes=s.v.io.volume_bytes;
    info->free_bytes=0u; /* Not reported by the read-only engine. */
    return IFS_FS_OK;
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
/* One direct V2 CORE/VOLUME contract. Never claim unqualified format,
 * repair, allocation mutation or DEFRAG callbacks. */
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
static const IfsFsDefragOpsV2 defrag_map_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=map_file
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
             #if defined(IFS_FFS_FIRMWARE_BUILD)
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
#else
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE},
#endif
    .probe=probe,.mount=mount,.unmount=unmount,
    .stat=stat_path,.directory_entry=nth,.read=read_file
#if !defined(IFS_FFS_FIRMWARE_BUILD)
    ,.write=write_file
#endif
};
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,
             IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC},
    .query=v2_query_volume,.sync=v2_sync_volume
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
