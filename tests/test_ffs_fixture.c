/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "ifs_ffs_fixture_v1.h"
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static uint8_t *bits_alloc(uint64_t bits) {
    return (uint8_t *)calloc((size_t)((bits+7U)/8U),1U);
}
static bool bits_get(const uint8_t *p,uint64_t bit) {
    return (p[bit>>3U] & (uint8_t)(1U<<(bit&7U)))!=0U;
}
static void bits_set(uint8_t *p,uint64_t bit,bool value) {
    const uint8_t mask=(uint8_t)(1U<<(bit&7U));
    if(value)p[bit>>3U]|=mask;
    else p[bit>>3U]&=(uint8_t)~mask;
}
/* Verify the actual FFS.so Common V2 extent map on the fixture populated
 * by the independent FFS formatter. No Defragmenter disk parser is used. */
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {int fd;uint64_t length;} V2TestDisk;
static int v2_test_read(void *ctx,uint64_t offset,void *dst,uint32_t bytes)
{
    const V2TestDisk *d=ctx;
    if(!d||!dst||offset%512u||bytes%512u||!bytes||
       offset>d->length||bytes>d->length-offset)
        return -1;
    return pread(d->fd,dst,bytes,(off_t)offset)==(ssize_t)bytes?0:-1;
}
static void *v2_test_alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void v2_test_free(void *ctx,void *p){(void)ctx;free(p);}
static int verify_common_v2_map(const char *path)
{
    static const char *file=
      "/Defragmenter-TestData/fragmented-files/fragmented-00.bin";
    V2TestDisk disk={-1,16u*1024u*1024u};
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core;
    const IfsFsDefragOpsV2 *map;
    IfsFsHostRuntimeV2 host={0};
    IfsFsVolumeInfoV2 info={0};
    IfsFsEntryV2 entry={0};
    char name[128]={0};
    uint32_t variant=0u;
    void *mounted=NULL;
    uint64_t first=0u,total_bytes=0u;
    int ok=0;
    disk.fd=open(path,O_RDONLY|O_CLOEXEC);
    if(disk.fd<0)return -1;
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=disk.length;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=65536u;
    host.context=&disk;
    host.read_at=v2_test_read;
    host.allocate=v2_test_alloc;
    host.release=v2_test_free;
    if(!ifs_fs_host_v2_valid(&host) || !ifs_fs_api_v2_valid(api,"ffs"))
        goto done;
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    map=(const IfsFsDefragOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core||!core->probe||!core->mount||!core->unmount||!core->stat||
       !core->read||!map||!map->map||
       map->header.capabilities!=IFS_FS_DEFRAG_MAP||
       map->plan||map->execute||map->verify||map->recover)
        goto done;
    info.structure_bytes=sizeof(info);
    entry.structure_bytes=sizeof(entry);
    entry.name=name;
    entry.name_capacity=sizeof(name);
    if(core->probe(&host,&variant)!=IFS_FS_OK ||
       core->mount(&host,&mounted,&info)!=IFS_FS_OK||!mounted||
       core->stat(mounted,file,&entry)!=IFS_FS_OK||!entry.size_bytes)
        goto done;
    for(unsigned page=0u;page<1024u;++page){
        IfsFsExtentV2 ext[16]={{0}};
        uint32_t count=0u;
        const int rc=map->map(mounted,file,first,ext,16u,&count);
        if(rc==IFS_FS_END){if(count==0u&&total_bytes==entry.size_bytes)ok=1;break;}
        if(rc!=IFS_FS_OK||!count||count>16u)break;
        for(uint32_t i=0u;i<count;++i){
            uint64_t logical=(first+i)*info.block_bytes;
            uint8_t physical=0u,logical_byte=0u;
            if(ext[i].logical_offset!=logical||
               ext[i].physical_offset%info.block_bytes||
               !ext[i].length||ext[i].length>info.block_bytes||
               ext[i].physical_offset>=disk.length||
               ext[i].length>disk.length-ext[i].physical_offset||
               ext[i].flags||ext[i].reserved||
               core->read(mounted,file,logical,&logical_byte,1u)!=IFS_FS_OK||
               pread(disk.fd,&physical,1u,
                     (off_t)ext[i].physical_offset)!=1||
               physical!=logical_byte){goto done;}
            total_bytes+=ext[i].length;
        }
        first+=count;
    }
    if(map->map(mounted,"/",0u,(IfsFsExtentV2[1]){{0}},1u,
                &(uint32_t){0})!=IFS_FS_UNSUPPORTED)ok=0;
    if(map->map(mounted,"/does-not-exist",0u,
                (IfsFsExtentV2[1]){{0}},1u,
                &(uint32_t){0})!=IFS_FS_NOT_FOUND)ok=0;
done:
    if(mounted && core->unmount(mounted)!=IFS_FS_OK)ok=0;
    close(disk.fd);
    return ok?0:-1;
}
/* Test-only fixture export for the independent Defragmenter Common V2
 * consumer. Never export test-media writers as public filesystem runtime
 * callbacks: a successful standalone test may retain its verified image. */
int main(int argc, char **argv) {
    if (argc!=1 && (argc!=3 || strcmp(argv[1],"--output") ||
                    !argv[2] || !argv[2][0]))
        return 2;
    char path[]="/tmp/ifs-ffs-corpus-XXXXXX";
    int fd=mkstemp(path);
    if(fd<0)return 1;
    int rc=1;
    if(ftruncate(fd,16*1024*1024)!=0)goto done;
    if(close(fd)!=0) {fd=-1;goto done;}
    fd=-1;
    const IfsFfsFixtureV1 *api=infiltrator_filesystem_ffs_fixture_v1();
    if(!api || api->abi_version!=IFS_FFS_FIXTURE_ABI_V1 ||
       api->structure_bytes<sizeof(*api) ||
       !api->format_id || strcmp(api->format_id,"ffs") ||
       api->capabilities!=(IFS_FFS_FIXTURE_CAP_FORMAT|IFS_FFS_FIXTURE_CAP_POPULATE|IFS_FFS_FIXTURE_CAP_VERIFY) ||
       !api->format || !api->validate || !api->populate || !api->verify || !api->verify_after)goto done;
    IfsDefragServicesV1 services={.abi_version=IFS_DEFRAG_SERVICES_V1,
        .structure_bytes=sizeof(IfsDefragServicesV1),
        .ld_bitmap_calloc=bits_alloc,.ld_bitmap_get=bits_get,.ld_bitmap_set=bits_set};
    IfsFfsFragmentProfile p={.files=8,.chunks=3,.chunk_kib=4,
       .directory_initial=0,.directory_second=4};
    char detail[512]={0};
    if(api->format(path,"FFS_UNIT",&services)!=0){fputs("format rejected 16MiB fixture\n",stderr);goto done;}
    if(api->validate(path,&services)!=0){fputs("format validation failed\\n",stderr);goto done;}
    if(api->populate(path,&p,&services)!=0){fputs("population rejected 16MiB fixture\n",stderr);goto done;}
    if(api->verify(path,&p,detail,sizeof(detail),&services)!=0) {
        fprintf(stderr,"FFS fragment corpus verification: %s\n",detail);
        goto done;
    }
    if(verify_common_v2_map(path)!=0) {
        fputs("FFS Common V2 real-file extents, metadata or page verification failed\n",stderr);
        goto done;
    }
    if(api->verify(path,&p,detail,sizeof(detail),&services)!=0)goto done;
    puts("FFS.so independent format/populate/fragmentation and Common V2 MAP passed.");
    rc=0;
done:
    if(fd>=0)close(fd);
    if(rc==0 && argc==3 && rename(path,argv[2])==0)
        return 0;
    if(rc==0 && argc==3) {
        perror("FFS test-only image export");
        rc=1;
    }
    unlink(path);
    return rc;
}
