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
static int cancelled(void *ctx){(void)ctx;return 1;}
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
/* Validate complete allocation paging independently against the
 * formatter's measured free count and physical volume size. */
static int allocation_test(void *session,uint64_t size,uint64_t free_expected)
{
    const IfsFsAllocationOpsV2 *ops=(const void*)
        ifs_fs_group_v2(infiltrator_filesystem_api_v2(),IFS_FS_GROUP_ALLOCATION);
    if(!ops || !ops->map)return 1;
    uint64_t at=0u,unknown=0u,free_bytes=0u,reserved=0u,allocated=0u;
    for(;;) {
        IfsFsAllocationRangeV2 page[3]={{0}};
        uint64_t next=at;
        uint32_t n=99u;
        int rc=ops->map(session,at,page,3u,&n);
        if(!ifs_fs_allocation_page_v2_valid(size,at,rc,page,3u,n,&next,&unknown))
            return 1;
        if(rc==IFS_FS_END)break;
        for(uint32_t i=0u;i<n;++i) {
            if(page[i].state==IFS_FS_SPACE_FREE)free_bytes+=page[i].length;
            if(page[i].state==IFS_FS_SPACE_RESERVED)reserved+=page[i].length;
            if(page[i].state==IFS_FS_SPACE_ALLOCATED)allocated+=page[i].length;
        }
        if(next<=at)return 1;
        at=next;
    }
    return at!=size || unknown || reserved!=1024u || !allocated ||
        free_bytes!=free_expected || free_bytes+reserved+allocated!=size;
}
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
    CHECK(vol->format(&h,3u,"TEST",0u)==IFS_FS_UNSUPPORTED);
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
    uint8_t *root_map=NULL;uint32_t root_bit=0u;
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
            root_map=media.bytes+(uint64_t)ptr*512u;root_bit=bit;
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
    ro.capabilities|=IFS_FS_HOST_CANCEL;ro.cancelled=cancelled;
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_BUSY && !session);
    ro.capabilities&=~IFS_FS_HOST_CANCEL;ro.cancelled=NULL;
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK && session);
    CHECK(info.variant==1u && info.total_bytes==media.size);
    char label[64],small[2]={'X','X'};
    uint32_t label_bytes=UINT32_MAX;
    CHECK(vol->label && !vol->set_label);
    CHECK(vol->label(session,label,sizeof(label),&label_bytes)==IFS_FS_OK &&
          label_bytes==4u && !strcmp(label,"TEST"));
    CHECK(vol->label(session,small,sizeof(small),&label_bytes)==IFS_FS_NO_SPACE &&
          !label_bytes && small[0]=='X' && small[1]=='X');
    const uint32_t extension_count=total_maps>25u?(total_maps-25u+126u)/127u:0u;
    const uint64_t expected=(uint64_t)(blocks-3u-total_maps-extension_count)*512u;
    CHECK(info.free_bytes==expected);
    info.free_bytes=0u;
    CHECK(vol->query(session,&info)==IFS_FS_OK && info.free_bytes==expected);
    CHECK(!allocation_test(session,media.size,expected));
    /* A damaged bitmap checksum cannot become a plausible free count. */
    uint8_t *first_map=media.bytes+(uint64_t)word(r,79u)*512u;
    first_map[0]^=1u;
    CHECK(vol->query(session,&info)==IFS_FS_CORRUPT && info.free_bytes==0u);
    const IfsFsAllocationOpsV2 *allocation=(const void*)
        ifs_fs_group_v2(infiltrator_filesystem_api_v2(),IFS_FS_GROUP_ALLOCATION);
    IfsFsAllocationRangeV2 bad_range={0};
    uint32_t bad_count=99u;
    CHECK(allocation && allocation->map(session,0u,&bad_range,1u,&bad_count)
        ==IFS_FS_CORRUPT && !bad_count);
    first_map[0]^=1u;
    CHECK(vol->query(session,&info)==IFS_FS_OK && info.free_bytes==expected);
    /* Keep the bitmap checksum valid while claiming its own root is free.
     * A plausible free count is insufficient proof of a safe allocation map. */
    CHECK(root_map!=NULL);
    const uint32_t slot=1u+root_bit/32u,mask=UINT32_C(1)<<(root_bit%32u);
    const uint32_t original_word=word(root_map,slot),original_sum=word(root_map,0u);
    CHECK(!(original_word&mask));
    const uint32_t altered_word=original_word|mask,altered_sum=original_sum-mask;
    for(unsigned j=0u;j<4u;++j) {
        root_map[4u*slot+j]=(uint8_t)(altered_word>>(24u-8u*j));
        root_map[j]=(uint8_t)(altered_sum>>(24u-8u*j));
    }
    bad_count=99u;
    CHECK(allocation->map(session,0u,&bad_range,1u,&bad_count)==IFS_FS_CORRUPT
          && !bad_count);
    for(unsigned j=0u;j<4u;++j) {
        root_map[4u*slot+j]=(uint8_t)(original_word>>(24u-8u*j));
        root_map[j]=(uint8_t)(original_sum>>(24u-8u*j));
    }
    CHECK(!allocation_test(session,media.size,expected));
    CHECK(core->stat(session,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(session,"/",0u,&entry)==IFS_FS_END);
    CHECK(vol->format(&h,1u,"TEST",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&h,&session,&info)==IFS_FS_OK);
    CHECK(core->create(session,"/A.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(session,"/B.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(session,"/a.bin",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS);
    uint8_t a[65536],b[65536],copy[65536];
    memset(a,0x35,sizeof(a));memset(b,0x78,sizeof(b));
    for(unsigned i=0;i<4u;++i) {
        CHECK(core->write(session,"/A.BIN",(uint64_t)i*sizeof(a),a,sizeof(a))==IFS_FS_OK);
        CHECK(core->write(session,"/B.BIN",(uint64_t)i*sizeof(b),b,sizeof(b))==IFS_FS_OK);
    }
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK);
    CHECK(core->stat(session,"/A.BIN",&entry)==IFS_FS_OK && entry.size_bytes==4u*sizeof(a));
    for(unsigned i=0;i<4u;++i) {
        CHECK(core->read(session,"/A.BIN",(uint64_t)i*sizeof(a),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(a,copy,sizeof(a)));
        CHECK(core->read(session,"/B.BIN",(uint64_t)i*sizeof(b),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(b,copy,sizeof(b)));
    }
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(infiltrator_filesystem_api_v2(),IFS_FS_GROUP_DEFRAG);
    IfsFsExtentV2 ex[2];uint32_t written=0;
    CHECK(map->map(session,"/A.BIN",127u,ex,2,&written)==IFS_FS_OK && written==2u);
    CHECK(ex[1].physical_offset!=ex[0].physical_offset+ex[0].length);
    CHECK(!allocation_test(session,media.size,info.free_bytes));
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
       !core->create || core->remove || core->truncate ||
       !volume || !volume->query || !volume->sync ||
       !volume->format || volume->repair ||
       !defrag || defrag->header.capabilities!=IFS_FS_DEFRAG_MAP ||
       !defrag->map || defrag->plan || defrag->execute || defrag->verify ||
       defrag->recover || defrag->release_plan ||
       (api->runtime_profiles & IFS_FS_PROFILE_KERNEL) ||
       api->group_mask!=(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION))) {
        fputs("FFS Common V2 capability contract invalid\n",stderr);
        return 1;
    }
    if(format_test(core,volume,8192u) || format_test(core,volume,131072u))return 1;
    puts("FFS-DOS1-FORMAT-BOOT-CHECKSUM-BITMAP-EXTENSION-AND-MOUNT-PASS");
    puts("FFS-STRICT-COMMON-V2-CONTRACT-PASS");
    return 0;
}
