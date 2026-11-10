/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real read-only Common V1 filesystem adapter, with bounded V2 overwrites.
 * The canonical FFS drop-in engine owns all on-disk parsing and mutation.
 */
#include "ffs_module.h"
#include <infiltratr/filesystem_module.h>
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <string.h>
static struct {
    IfsFfsDropInVolume v;
    IfsFsHostIoV1 io;
    uintptr_t generation,handle;
    uint32_t variant;
    int mounted;
} s;
static int live(void *v){
    return s.mounted && s.handle && (uintptr_t)v==s.handle && s.v.mounted;
}
static int host_ok(const IfsFsHostIoV1 *h){
    return h && h->structure_bytes>=sizeof(*h) && h->read_at &&
        h->volume_bytes>=8192u &&
        !(h->flags & ~(IFS_FS_IO_READ_ONLY|IFS_FS_IO_EXCLUSIVE));
}
static int detect(const IfsFsHostIoV1 *h,IfsFfsDropInVolume *found,
                  uint32_t *variant){
    uint8_t boot[4];
    int valid=0;
    const IfsFfsDropInModule *m=infiltrator_filesystem_module_v1();
    if(h->read_at(h->context,0,boot,sizeof(boot))||
        boot[0]!='D'||boot[1]!='O'||boot[2]!='S'||
        (boot[3]!=1u&&boot[3]!=3u&&boot[3]!=5u))
        return IFS_FS_NOT_FOUND;
    for(uint32_t size=512u;size<=32768u;size*=2u){
        IfsFfsDropInVolume v={0};
        IfsFfsDropInIo io={h->read_at,h->context,h->volume_bytes,size};
        if(h->volume_bytes%size ||
           m->mount(&v,&io)!=0)continue;
        if(++valid>1)return IFS_FS_CORRUPT;
        *found=v;
    }
    if(valid!=1)return IFS_FS_CORRUPT;
    *variant=boot[3];
    return IFS_FS_OK;
}
static int probe(const IfsFsHostIoV1 *h,uint32_t *variant){
    IfsFfsDropInVolume v={0};
    if(!host_ok(h)||!variant)return IFS_FS_INVALID;
    return detect(h,&v,variant);
}
static int mount(const IfsFsHostIoV1 *h,void **volume,
                 IfsFsVolumeInfoV1 *info){
    uint32_t variant;int r;
    if(!host_ok(h)||!volume||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
    if(s.mounted||s.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    r=detect(h,&s.v,&variant);
    if(r)return r;
    s.io=*h;s.variant=variant;s.mounted=1;
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
    const IfsFfsDropInModule *m=infiltrator_filesystem_module_v1();
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
        if(r)return r==1?IFS_FS_NOT_FOUND:IFS_FS_CORRUPT;
        current=next;*basename=start;*name_len=(uint32_t)(p-start);
        if(*p=='/'&&!*++p)return IFS_FS_INVALID;
    }
    *out=current;return IFS_FS_OK;
}
static int convert_name(const uint8_t *latin,uint32_t n,char *out,uint32_t cap,
                        uint32_t *written){
    uint32_t k=0;
    for(uint32_t i=0;i<n;++i){
        unsigned char c=latin[i];
        if(c<128u){if(k+1u>=cap)return IFS_FS_NO_SPACE;out[k++]=(char)c;}
        else {
            if(k+2u>=cap)return IFS_FS_NO_SPACE;
            out[k++]=(char)(0xc0u|(c>>6));
            out[k++]=(char)(0x80u|(c&0x3fu));
        }
    }
    if(k>=cap)return IFS_FS_NO_SPACE;
    out[k]=0;*written=k;
    return IFS_FS_OK;
}
static int fill(IfsFsEntryV1 *out,const IfsFfsDropInEntry *e,
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
static int stat_path(void *volume,const char *path,IfsFsEntryV1 *out){
    IfsFfsDropInEntry e={0};const char *name=NULL;uint32_t n=0;
    if(!live(volume))return IFS_FS_INVALID;
    int r=path_entry(path,&e,&name,&n);
    return r?r:fill(out,&e,name,n);
}
static int nth(void *volume,const char *dir,uint32_t index,IfsFsEntryV1 *out){
    IfsFfsDropInEntry parent={0},e={0};
    const char *name;uint32_t n;uint8_t raw[31];uint32_t length=0;
    IfsFfsDropInDirCursor cursor={0};
    if(!live(volume))return IFS_FS_INVALID;
    int r=path_entry(dir,&parent,&name,&n);
    if(r)return r;
    const IfsFfsDropInModule *m=infiltrator_filesystem_module_v1();
    for(uint32_t i=0;i<=index;++i) {
        r=m->next(&s.v,&parent,&cursor,&e,raw);
        if(r)return r==1?IFS_FS_END:IFS_FS_CORRUPT;
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
    int r=path_entry(path,&e,&name,&n);
    if(r)return r;
    if(e.type!=IFS_FFS_FILE)return IFS_FS_INVALID;
    return infiltrator_filesystem_module_v1()->read(&s.v,&e,(uint32_t)offset,
                                                     dest,len)==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int write_file(void *volume,const char *path,uint64_t offset,
                      const void *data,uint32_t len){
    IfsFfsDropInEntry e={0};const char *name;uint32_t n;
    if(!live(volume)||(!data&&len))return IFS_FS_INVALID;
    if((s.io.flags&IFS_FS_IO_READ_ONLY)||
       !(s.io.flags&IFS_FS_IO_EXCLUSIVE)||
       !s.io.write_at||!s.io.flush)return IFS_FS_ACCESS;
    if(offset>UINT32_MAX||len>UINT32_MAX-offset)return IFS_FS_UNSUPPORTED;
    int r=path_entry(path,&e,&name,&n);
    if(r)return r;
    if(e.type!=IFS_FFS_FILE)return IFS_FS_INVALID;
    if(offset>e.size||len>e.size-offset)return IFS_FS_UNSUPPORTED;
    IfsFfsWritableIo io={0};
    io.read=s.v.io;io.write_at=s.io.write_at;io.flush=s.io.flush;
    r=infiltrator_filesystem_module_v2()->overwrite_existing(&s.v,&e,&io,
        (uint32_t)offset,data,len);
    return r==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}
static const IfsFsModuleApiV1 api={
    IFS_FS_COMMON_ABI_V1,sizeof(IfsFsModuleApiV1),"ffs",
    IFS_FS_CAP_PROBE|IFS_FS_CAP_MOUNT|IFS_FS_CAP_LIST|
    IFS_FS_CAP_READ|IFS_FS_CAP_WRITE,
    probe,mount,unmount,stat_path,nth,read_file,write_file,
    NULL,NULL,NULL,NULL,NULL,NULL,NULL
};
__attribute__((visibility("default")))
const IfsFsModuleApiV1 *infiltrator_filesystem_api_v1(void) {
    return &api;
}

/* V2 provides only real volume inquiry; no unsupported repair/format claim. */
static int v2_query_volume(void *volume,IfsFsVolumeInfoV1 *info){
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
    if(s.io.flags&IFS_FS_IO_READ_ONLY)return IFS_FS_OK;
    if(!(s.io.flags&IFS_FS_IO_EXCLUSIVE) || !s.io.write_at || !s.io.flush)
        return IFS_FS_ACCESS;
    return s.io.flush(s.io.context)==0 ? IFS_FS_OK : IFS_FS_IO_ERROR;
}
static const IfsFsVolumeOpsV1 group_volume={
    .header={IFS_FS_GROUP_ABI_V1,sizeof(IfsFsVolumeOpsV1),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC},
    .query=v2_query_volume,
    .sync=v2_sync_volume
};
static const IfsFsGroupHeaderV2 *query_v2(
    uint32_t id,uint32_t minimum,uint32_t maximum){
    if(id!=IFS_FS_GROUP_VOLUME||minimum>IFS_FS_GROUP_ABI_V1||
       maximum<IFS_FS_GROUP_ABI_V1)return NULL;
    return &group_volume.header;
}
static const IfsFsModuleApiV2 module_v2={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ffs",
    IFS_FS_PROFILE_HOSTED,IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    &api,query_v2
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){
    return &module_v2;
}
