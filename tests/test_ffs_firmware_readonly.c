/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the actual firmware descriptor and native FFS engine. A valid
 * writable host must be denied before the first read or write callback.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FFS firmware V2 line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {unsigned reads,writes;} Media;
static int read_at(void *ctx,uint64_t at,void *out,uint32_t n)
{(void)at;(void)out;(void)n;++((Media *)ctx)->reads;return -1;}
static int write_at(void *ctx,uint64_t at,const void *in,uint32_t n)
{(void)at;(void)in;(void)n;++((Media *)ctx)->writes;return -1;}
static int flush(void *ctx){(void)ctx;return -1;}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ffs"));
    CHECK(api->runtime_profiles ==
          (IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE));
    const IfsFsCoreOpsV2 *core=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->mount && core->probe &&
          core->read && core->stat && core->directory_entry &&
          !core->write && !core->create && !core->remove);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(vol && !vol->format);
    Media media={0};
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    host.volume_bytes=1024u*1024u;
    host.logical_sector_bytes=host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=4096u;
    host.context=&media;
    host.read_at=read_at;
    host.write_at=write_at;
    host.flush=flush;
    host.allocate=allocate;
    host.release=release;
    CHECK(ifs_fs_host_v2_valid(&host));
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *volume=(void *)(uintptr_t)1u;
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_ACCESS &&
          !volume && !media.reads && !media.writes);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.write_at=NULL;host.flush=NULL;
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=99u;
    CHECK(core->probe(&host,&variant)==IFS_FS_IO_ERROR && !variant);
    CHECK(media.reads>0u && !media.writes);
    CHECK(core->unmount((void *)(uintptr_t)1u)==IFS_FS_INVALID);
    puts("FFS-FREESTANDING-READONLY-LEASE-AND-IO-ERROR-PASS");
    return 0;
}
