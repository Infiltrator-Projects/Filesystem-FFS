/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "ifs_ffs_fixture_v1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    IfsFfsFragmentProfile p={.files=8,.chunks=3,.chunk_kib=4,
       .directory_initial=0,.directory_second=4};
    char detail[512]={0};
    if(api->format(path,"FFS_UNIT")!=0){fputs("format rejected 16MiB fixture\n",stderr);goto done;}
    if(api->populate(path,&p)!=0){fputs("population rejected 16MiB fixture\n",stderr);goto done;}
    if(api->verify(path,&p,detail,sizeof(detail))!=0) {
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
