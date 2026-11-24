/* SPDX-License-Identifier: GPL-3.0-or-later
 * One strict Common filesystem V2 contract over the native FFS engine.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {uint8_t *bytes;uint64_t size;unsigned writes,flushes;} Media;
static int rd(void *ctx,uint64_t at,void *out,uint32_t n)
{
    Media *m=ctx;
    if(at%512u || n%512u || n>4096u || at>m->size || n>m->size-at)return -1;
    memcpy(out,m->bytes+at,n);return 0;
}
static int wr(void *ctx,uint64_t at,const void *in,uint32_t n)
{
    Media *m=ctx;
    if(at%512u || n%512u || n>4096u || at>m->size || n>m->size-at)return -1;
    memcpy(m->bytes+at,in,n);++m->writes;return 0;
}
static int flush(void *ctx){++((Media*)ctx)->flushes;return 0;}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void rel(void *ctx,void *p){(void)ctx;free(p);}
static uint32_t word(const uint8_t *p,unsigned n)
{
    p+=4u*n;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FFS format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
static int format_test(const IfsFsCoreOpsV2 *core,const IfsFsVolumeOpsV2 *vol,
                       uint32_t blocks)
{
    Media media={.size=(uint64_t)blocks*512u};
    media.bytes=calloc(1,(size_t)media.size);
    if(!media.bytes)return 1;
    int result=1;
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=media.size,.logical_sector_bytes=512u,
        .physical_sector_bytes=512u,.maximum_transfer_bytes=4096u,
        .context=&media,.read_at=rd,.write_at=wr,.flush=flush,
        .allocate=alloc,.release=rel
    },ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    CHECK(vol->format(&ro,1u,"TEST",0u)==IFS_FS_ACCESS);
    CHECK(vol->format(&h,0u,"TEST",0u)==IFS_FS_UNSUPPORTED);
    CHECK(vol->format(&h,1u,"Bad:name",0u)==IFS_FS_INVALID);
    CHECK(!media.writes && !media.flushes);
    CHECK(vol->format(&h,1u,"TEST",0u)==IFS_FS_OK && media.flushes>=3u);
    CHECK(!memcmp(media.bytes,"DOS\001",4u));
    uint32_t root=word(media.bytes,2u);
    CHECK(root==(blocks+1u)/2u);
    uint32_t boot_sum=0u;
    for(unsigned i=0;i<256u;++i) {
        uint32_t next=boot_sum+word(media.bytes,i);
        boot_sum=next+(next<boot_sum?1u:0u);
    }
    CHECK(boot_sum==UINT32_MAX);
    const uint8_t *r=media.bytes+(uint64_t)root*512u;
    CHECK(r[432]==4u && !memcmp(r+433,"TEST",4u));
    uint32_t total_maps=(blocks-2u+4063u)/4064u;
    uint32_t extension=word(r,104u),extension_slot=0u;
    for(uint32_t i=0u;i<total_maps;++i) {
        uint32_t ptr;
        if(i<25u)ptr=word(r,79u+i);
        else {
            CHECK(extension>1u && extension<blocks);
            const uint8_t *ext=media.bytes+(uint64_t)extension*512u;
            ptr=word(ext,extension_slot++);
            if(extension_slot==127u){extension=word(ext,127u);extension_slot=0u;}
        }
        CHECK(ptr>1u && ptr<blocks);
        const uint8_t *map=media.bytes+(uint64_t)ptr*512u;
        uint32_t sum=0u;
        for(unsigned j=0u;j<128u;++j)sum+=word(map,j);
        CHECK(!sum);
        if(root>=2u+i*4064u && root<2u+(i+1u)*4064u) {
            const uint32_t bit=root-2u-i*4064u;
            CHECK(!(word(map,1u+bit/32u)&(UINT32_C(1)<<(bit%32u))));
        }
    }
    uint32_t variant=0u;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *session=NULL;
    char name[64];
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    CHECK(core->probe(&ro,&variant)==IFS_FS_OK && variant==1u);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK && session);
    CHECK(info.variant==1u && info.total_bytes==media.size);
    CHECK(core->stat(session,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(session,"/",0u,&entry)==IFS_FS_END);
    CHECK(vol->format(&h,1u,"TEST",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(session)==IFS_FS_OK);
    result=0;
done:
    free(media.bytes);return result;
}
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    const IfsFsDefragOpsV2 *defrag;
    if(!ifs_fs_api_v2_valid(api,"ffs"))return 1;
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    volume=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    defrag=(const IfsFsDefragOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core || !core->probe || !core->mount || !core->unmount ||
       !core->stat || !core->directory_entry || !core->read || !core->write ||
       core->create || core->remove || core->truncate ||
       !volume || !volume->query || !volume->sync ||
       !volume->format || volume->repair ||
       !defrag || defrag->header.capabilities!=IFS_FS_DEFRAG_MAP ||
       !defrag->map || defrag->plan || defrag->execute || defrag->verify ||
       defrag->recover || defrag->release_plan ||
       (api->runtime_profiles & IFS_FS_PROFILE_KERNEL) ||
       api->group_mask!=(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG))) {
        fputs("FFS Common V2 capability contract invalid\n",stderr);
        return 1;
    }
    if(format_test(core,volume,8192u) || format_test(core,volume,131072u))return 1;
    puts("FFS-DOS1-FORMAT-BOOT-CHECKSUM-BITMAP-EXTENSION-AND-MOUNT-PASS");
    puts("FFS-STRICT-COMMON-V2-CONTRACT-PASS");
    return 0;
}
