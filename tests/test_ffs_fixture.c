/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "ifs_ffs_fixture_v1.h"
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
int main(void) {
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
       !api->format || !api->populate || !api->verify || !api->verify_after)goto done;
    IfsDefragServicesV1 services={.abi_version=IFS_DEFRAG_SERVICES_V1,
        .structure_bytes=sizeof(IfsDefragServicesV1),
        .ld_bitmap_calloc=bits_alloc,.ld_bitmap_get=bits_get,.ld_bitmap_set=bits_set};
    IfsFfsFragmentProfile p={.files=8,.chunks=3,.chunk_kib=4,
       .directory_initial=0,.directory_second=4};
    char detail[512]={0};
    if(api->format(path,"FFS_UNIT",&services)!=0){fputs("format rejected 16MiB fixture\n",stderr);goto done;}
    if(api->populate(path,&p,&services)!=0){fputs("population rejected 16MiB fixture\n",stderr);goto done;}
    if(api->verify(path,&p,detail,sizeof(detail),&services)!=0) {
        fprintf(stderr,"FFS fragment corpus verification: %s\n",detail);
        goto done;
    }
    puts("FFS.so independent format/populate/fragmentation fixture passed.");
    rc=0;
done:
    if(fd>=0)close(fd);
    unlink(path);
    return rc;
}
