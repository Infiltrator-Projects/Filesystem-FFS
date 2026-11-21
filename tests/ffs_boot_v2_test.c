/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read the disposable FFS DOS1 kernel/initramfs corpus using the actual
 * production Common V2 shared object, never the test fixture's parser.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define REQUIRE(x) do { if(!(x)){ \
    fprintf(stderr,"FFS-BOOT-V2 line %d: %s\n",__LINE__,#x);return 1; \
} } while(0)
enum {BS=32768u,BLOCKS=4096u};
typedef struct { FILE *file;uint64_t size,fail_at;uint32_t calls; } Disk;
static int media_read(void *ctx,uint64_t offset,void *p,uint32_t n)
{
    Disk *d=ctx;
    if(!p||!n||offset==d->fail_at||offset%512u||n%512u||
       offset>d->size||n>d->size-offset||offset>INT64_MAX)return -1;
    ++d->calls;
    return fseeko(d->file,(off_t)offset,SEEK_SET)==0 &&
           fread(p,1,n,d->file)==n?0:-1;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int open_media(Disk *d,const char *filename)
{
    struct stat st;
    d->file=fopen(filename,"rb");
    if(!d->file||fstat(fileno(d->file),&st)||!S_ISREG(st.st_mode)||st.st_size<=0)return -1;
    d->size=(uint64_t)st.st_size;
    d->fail_at=UINT64_MAX;
    return 0;
}
static IfsFsHostRuntimeV2 host_for(Disk *d)
{
    IfsFsHostRuntimeV2 h={0};
    h.abi_version=IFS_FS_HOST_ABI_V2;
    h.structure_bytes=sizeof(h);
    h.capabilities=IFS_FS_HOST_READ_ONLY;
    h.volume_bytes=d->size;
    h.logical_sector_bytes=512u;
    h.physical_sector_bytes=512u;
    h.maximum_transfer_bytes=65536u;
    h.context=d;
    h.read_at=media_read;
    h.allocate=allocate;
    h.release=release;
    return h;
}
static int compare_content(const IfsFsCoreOpsV2 *core,void *volume,
                           const char *path,const char *expected)
{
    FILE *fp=fopen(expected,"rb");
    if(!fp)return -1;
    struct stat st;
    if(fstat(fileno(fp),&st)||st.st_size<=0){fclose(fp);return -1;}
    IfsFsEntryV2 e={0};
    char name[128]={0};
    e.structure_bytes=sizeof(e);e.name=name;e.name_capacity=sizeof(name);
    if(core->stat(volume,path,&e)!=IFS_FS_OK ||
       e.type!=IFS_FS_ENTRY_FILE || e.size_bytes!=(uint64_t)st.st_size){
        fclose(fp);return -1;
    }
    uint64_t offset=0;
    uint8_t observed[4096],reference[4096];
    while(offset<(uint64_t)st.st_size) {
        size_t count=(uint64_t)st.st_size-offset<sizeof(observed)?
            (size_t)((uint64_t)st.st_size-offset):sizeof(observed);
        if(fread(reference,1,count,fp)!=count ||
           core->read(volume,path,offset,observed,(uint32_t)count)!=IFS_FS_OK||
           memcmp(observed,reference,count)!=0){fclose(fp);return -1;}
        offset+=count;
    }
    if(fclose(fp))return -1;
    return 0;
}
static int contains_entry(const IfsFsCoreOpsV2 *core,void *volume,
                          const char *directory,const char *expected)
{
    char name[128]={0};
    IfsFsEntryV2 entry={0};
    entry.structure_bytes=sizeof(entry);
    entry.name=name;entry.name_capacity=sizeof(name);
    for(uint32_t i=0;i<100u;++i){
        int rc=core->directory_entry(volume,directory,i,&entry);
        if(rc==IFS_FS_END)return 0;
        if(rc!=IFS_FS_OK)return -1;
        if(strcmp(entry.name,expected)==0)return 1;
    }
    return -1;
}
int main(int argc,char **argv)
{
    REQUIRE(argc==6);
    void *handle=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    REQUIRE(handle!=NULL);
    void *sym=dlsym(handle,IFS_FS_COMMON_EXPORT_V2);
    REQUIRE(sym!=NULL);
    IfsFsModuleApiEntryV2 resolve=NULL;
    REQUIRE(sizeof(resolve)==sizeof(sym));
    memcpy(&resolve,&sym,sizeof(resolve));
    const IfsFsModuleApiV2 *api=resolve?resolve():NULL;
    REQUIRE(ifs_fs_api_v2_valid(api,"ffs"));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    REQUIRE(core&&core->probe&&core->mount&&core->unmount&&core->stat&&
            core->directory_entry&&core->read);
    Disk d={0};
    REQUIRE(open_media(&d,argv[2])==0);
    REQUIRE(d.size==(uint64_t)BS*BLOCKS);
    IfsFsHostRuntimeV2 host=host_for(&d);
    REQUIRE(ifs_fs_host_v2_valid(&host));
    uint32_t version=UINT32_MAX;
    d.fail_at=0;
    REQUIRE(core->probe(&host,&version)==IFS_FS_IO_ERROR && version==0u);
    d.fail_at=UINT64_MAX;
    IfsFsHostRuntimeV2 invalid_host=host;
    invalid_host.abi_version=1u;
    version=UINT32_MAX;
    REQUIRE(core->probe(&invalid_host,&version)==IFS_FS_INVALID && version==0u);
    REQUIRE(core->probe(&host,&version)==IFS_FS_OK && version==1u);
    IfsFsVolumeInfoV2 info={0};info.structure_bytes=sizeof(info);
    void *volume=NULL;
    REQUIRE(core->mount(&host,&volume,&info)==IFS_FS_OK&&volume&&
            info.variant==1u&&info.block_bytes==BS);
    REQUIRE(contains_entry(core,volume,"/","System")==1);
    REQUIRE(contains_entry(core,volume,"/","vmlinuz")==1);
    REQUIRE(contains_entry(core,volume,"/System","Kernel")==1);
    REQUIRE(contains_entry(core,volume,"/System/Kernel","Images")==1);
    REQUIRE(contains_entry(core,volume,"/System/Kernel/Images","current.initrd")==1);
    /* An insufficient UTF-8 name capacity must not leave a partial
     * result in the caller-owned V2 buffer, for either STAT or LIST. */
    {
        char narrow[2]={'X','X'};
        IfsFsEntryV2 entry={0};
        entry.structure_bytes=sizeof(entry);
        entry.name=narrow;
        entry.name_capacity=sizeof(narrow);
        REQUIRE(core->stat(volume,"/vmlinuz",&entry)==IFS_FS_NO_SPACE);
        REQUIRE(narrow[0]=='X' && narrow[1]=='X' && entry.name_bytes==0u);
        REQUIRE(core->directory_entry(volume,"/",0u,&entry)==IFS_FS_NO_SPACE);
        REQUIRE(narrow[0]=='X' && narrow[1]=='X' && entry.name_bytes==0u);
    }
    REQUIRE(compare_content(core,volume,"/vmlinuz",argv[3])==0);
    REQUIRE(compare_content(core,volume,"/System/Kernel/Images/current.initrd",argv[4])==0);
    char path_name[128]={0};
    IfsFsEntryV2 missing={0};
    missing.structure_bytes=sizeof(missing);
    missing.name=path_name;missing.name_capacity=sizeof(path_name);
    REQUIRE(core->stat(volume,"/not-actually-present",&missing)==IFS_FS_NOT_FOUND);
    d.fail_at=(uint64_t)64u*BS;
    uint8_t sample=0;
    REQUIRE(core->read(volume,"/vmlinuz",0u,&sample,1u)==IFS_FS_IO_ERROR);
    d.fail_at=UINT64_MAX;
    REQUIRE(core->unmount(volume)==IFS_FS_OK);
    REQUIRE(d.calls>0u && fclose(d.file)==0);

    /* Independently corrupted disposable media must never be accepted. */
    Disk damaged={0};
    REQUIRE(open_media(&damaged,argv[5])==0);
    IfsFsHostRuntimeV2 damaged_host=host_for(&damaged);
    uint32_t damaged_variant=UINT32_MAX;
    REQUIRE(core->probe(&damaged_host,&damaged_variant)==IFS_FS_CORRUPT &&
            damaged_variant==0u);
    void *unexpected=NULL;
    IfsFsVolumeInfoV2 damaged_info={0};
    damaged_info.structure_bytes=sizeof(damaged_info);
    REQUIRE(core->mount(&damaged_host,&unexpected,&damaged_info)!=IFS_FS_OK &&
            unexpected==NULL);
    REQUIRE(fclose(damaged.file)==0);
    REQUIRE(dlclose(handle)==0);
    puts("FFS_COMMON_V2_REAL_IMAGE_NAMESPACE_AND_KERNEL_BYTES_PASS");
    return 0;
}
