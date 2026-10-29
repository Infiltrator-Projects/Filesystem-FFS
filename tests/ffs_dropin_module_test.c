/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent disposable FFS DOS1/3/5 fixtures; no physical disk writes. */
#include "ffs_module.h"
#include "../native/filesystems/ffs/core/ffs_primitives.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { BLOCKS = 256u, DIR = 4u, HEADER = 5u, EXT = 6u, DATA = 40u };
static uint8_t *disk;
static uint32_t bytes, root;
static uint32_t writes, flushes;
static void be32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);
  p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static uint8_t *blk(uint32_t n) { return disk + (size_t)n*bytes; }
static void put(uint32_t n,uint32_t i,uint32_t v)
{ be32(blk(n)+(size_t)i*4u,v); }
static uint32_t hash(const char *name)
{ return ifs_ffs_directory_hash((const uint8_t *)name,(uint32_t)strlen(name),
           bytes/4u-56u,0); }
static void set_name(uint32_t n,const char *s)
{ const size_t len=strlen(s);blk(n)[bytes-80u]=(uint8_t)len;
  memcpy(blk(n)+bytes-79u,s,len); }
static void checksum(uint32_t n)
{ put(n,5u,0u);put(n,5u,0u-ifs_ffs_block_checksum(blk(n),bytes)); }
static int read_at(void *ctx,uint64_t offset,void *out,uint32_t n)
{ (void)ctx;if(offset>(uint64_t)BLOCKS*bytes ||
     n>(uint64_t)BLOCKS*bytes-offset)return -1;
  memcpy(out,disk+(size_t)offset,n);return 0; }
static int write_at(void *ctx,uint64_t offset,const void *source,uint32_t count)
{ (void)ctx;
  if(offset<(uint64_t)DATA*bytes ||
     offset>(uint64_t)BLOCKS*bytes ||
     count>(uint64_t)BLOCKS*bytes-offset) return -1;
  memcpy(disk+(size_t)offset,source,count);
  ++writes;
  return 0;
}
static int flush_media(void *ctx)
{ (void)ctx; ++flushes; return 0; }
static void fixture(uint32_t block_bytes,uint32_t dostype,uint32_t data_blocks)
{
    const uint32_t slots=block_bytes/4u-56u;
    bytes=block_bytes;root=(2u+BLOCKS-1u)/2u;
    memset(disk,0,(size_t)BLOCKS*bytes);
    put(0u,0u,dostype);
    put(root,0u,2u);put(root,3u,slots);
    put(root,bytes/4u-1u,1u);put(root,6u+hash("boot"),DIR);
    set_name(root,"FFSROOT");checksum(root);
    put(DIR,0u,2u);put(DIR,1u,DIR);put(DIR,bytes/4u-3u,root);
    put(DIR,bytes/4u-1u,2u);put(DIR,6u+hash("kernel.efi"),HEADER);
    set_name(DIR,"boot");checksum(DIR);
    put(HEADER,0u,2u);put(HEADER,1u,HEADER);
    put(HEADER,2u,data_blocks>slots?slots:data_blocks);
    put(HEADER,4u,DATA);put(HEADER,bytes/4u-47u,data_blocks*bytes);
    put(HEADER,bytes/4u-3u,DIR);put(HEADER,bytes/4u-1u,(uint32_t)-3);
    put(HEADER,bytes/4u-2u,data_blocks>slots?EXT:0u);
    set_name(HEADER,"kernel.efi");
    for(uint32_t i=0u;i<data_blocks;i++){
        const uint32_t target=i<slots?HEADER:EXT;
        put(target,6u+slots-1u-i%slots,DATA+i);
        memset(blk(DATA+i),'A'+(int)(i%26u),bytes);
    }
    checksum(HEADER);
    if(data_blocks>slots){
        put(EXT,0u,16u);put(EXT,1u,EXT);
        put(EXT,2u,data_blocks-slots);put(EXT,4u,DATA+slots);
        put(EXT,bytes/4u-3u,HEADER);put(EXT,bytes/4u-1u,(uint32_t)-3);
        checksum(EXT);
    }
}
static int fail(const char *why)
{ fprintf(stderr,"FFS module test: %s\n",why);free(disk);return 1; }
int main(void)
{
    const IfsFfsDropInModule *api=infiltrator_filesystem_module_v1();
    const IfsFfsDropInModuleV2 *v2=infiltrator_filesystem_module_v2();
    IfsFfsDropInIo io={read_at,NULL,0u,0u};
    IfsFfsWritableIo writable={ {read_at,NULL,0u,0u},
                                write_at, flush_media };
    IfsFfsDropInVolume v={0};
    IfsFfsDropInEntry rt,dir,file,out;
    IfsFfsDropInDirCursor cursor;
    uint8_t text[8],name[31];
    if(!v2 || v2->abi_version!=IFS_FFS_DROPIN_V2_ABI_VERSION ||
       v2->structure_bytes!=sizeof(*v2) ||
       strcmp(v2->format_id,"ffs")!=0 ||
       v2->readonly_v1!=api ||
       !(v2->capabilities&IFS_FFS_V2_CAP_OVERWRITE_EXISTING))
       return fail("invalid optional FFS V2 ABI");
    if(!api || api->abi_version!=1u ||
        api->structure_bytes!=sizeof(*api) ||
        strcmp(api->format_id,"ffs")!=0 ||
        (api->capabilities&(IFS_FFS_CAP_DOS1|IFS_FFS_CAP_DOS3|
        IFS_FFS_CAP_DOS5|IFS_FFS_CAP_FILE_READ|
        IFS_FFS_CAP_BLOCK_512_TO_32768)) !=
        (IFS_FFS_CAP_DOS1|IFS_FFS_CAP_DOS3|IFS_FFS_CAP_DOS5|
        IFS_FFS_CAP_FILE_READ|IFS_FFS_CAP_BLOCK_512_TO_32768))
        return fail("invalid independent FFS module ABI");
    disk=malloc((size_t)BLOCKS*32768u);
    if(!disk)return fail("memory allocation");
    for(uint32_t b=512u;b<=32768u;b*=2u){
        io.block_bytes=b;io.volume_bytes=(uint64_t)BLOCKS*b;
        fixture(b,0x444f5301u,2u);
        if(api->mount(&v,&io)!=0)return fail("valid DOS1 mount");
        rt=api->root(&v);
        if(rt.block!=root ||
           api->lookup(&v,&rt,(const uint8_t *)"BOOT",4u,&dir)!=0 ||
           api->lookup(&v,&dir,(const uint8_t *)"kernel.efi",10u,&file)!=0 ||
           file.size!=b*2u)return fail("nested lookup/size");
        memset(&cursor,0,sizeof(cursor));
        if(api->next(&v,&dir,&cursor,&out,name)!=0 ||
           strcmp((const char*)name,"kernel.efi")!=0 ||
           api->next(&v,&dir,&cursor,&out,name)!=1)
            return fail("directory traversal");
        if(api->read(&v,&file,b-2u,text,4u)!=0 ||
           memcmp(text,"AABB",4u)!=0)
            return fail("raw FFS payload crossing block boundary");
        if(api->read(&v,&file,file.size-1u,text,2u)==0)
            return fail("out of range read accepted");
        fixture(b,0x444f5300u,2u);
        if(api->mount(&v,&io)==0)return fail("foreign OFS accepted");
        fixture(b,0x444f5303u,2u);
        if(api->mount(&v,&io)!=0)return fail("DOS3 rejected");
        fixture(b,0x444f5305u,2u);
        if(api->mount(&v,&io)!=0)return fail("DOS5 rejected");
        fixture(b,0x444f5301u,2u);blk(root)[80]^=1u;
        if(api->mount(&v,&io)==0)return fail("corrupt root accepted");
    }
    io.block_bytes=512u;io.volume_bytes=BLOCKS*512u;
    fixture(512u,0x444f5301u,73u);
    if(api->mount(&v,&io)!=0)return fail("extension mount");
    rt=api->root(&v);
    if(api->lookup(&v,&rt,(const uint8_t *)"boot",4u,&dir)!=0 ||
       api->lookup(&v,&dir,(const uint8_t *)"kernel.efi",10u,&file)!=0)
        return fail("extension lookup");
    if(api->read(&v,&file,72u*512u-2u,text,4u)!=0 ||
       memcmp(text,"TTUU",4u)!=0)
        return fail("extension pointers/raw data");
    writable.read=io;
    writes=flushes=0u;
    if(v2->overwrite_existing(&v,&file,&writable,72u*512u-2u,
                              "WXYZ",4u)!=0 ||
       writes!=2u || flushes!=1u ||
       api->read(&v,&file,72u*512u-2u,text,4u)!=0 ||
       memcmp(text,"WXYZ",4u)!=0)
       return fail("V2 cross-extension overwrite and flush");
    if(v2->overwrite_existing(&v,&file,&writable,file.size-1u,
                              "AB",2u)==0 || writes!=2u)
       return fail("V2 permitted file extension");
    writable.flush=NULL;
    if(v2->overwrite_existing(&v,&file,&writable,0u,"X",1u)==0)
       return fail("V2 allowed write without flush");
    writable.flush=flush_media;
    writable.write_at=NULL;
    if(v2->overwrite_existing(&v,&file,&writable,0u,"X",1u)==0)
       return fail("V2 allowed write without callback");
    writable.write_at=write_at;
    put(EXT,0u,8u);checksum(EXT);
    if(v2->overwrite_existing(&v,&file,&writable,72u*512u,
                              "Q",1u)==0 || writes!=2u)
       return fail("V2 accepted corrupt extension");
    if(api->read(&v,&file,72u*512u,text,1u)==0)
        return fail("invalid extension type accepted");
    fixture(512u,0x444f5301u,73u);
    put(EXT,512u/4u-3u,2u);checksum(EXT);
    if(api->read(&v,&file,72u*512u,text,1u)==0)
        return fail("foreign extension parent accepted");
    io.volume_bytes=512u;
    if(api->mount(&v,&io)==0)return fail("truncated filesystem accepted");
    free(disk);
    puts("FFS-DOS1-DOS3-DOS5-512-32768-EXTENSION-READ-PASS");
    return 0;
}
