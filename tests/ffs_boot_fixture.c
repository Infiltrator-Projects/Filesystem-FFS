/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable DOS1 FFS image builder for the BOOT integration test ONLY.
 * Uses genuine Amiga FFS root, hash chains, file headers, allocation bitmap
 * and raw FFS data blocks. No production formatting or boot parser is used.
 *
 * Usage: ffs-boot-fixture IMAGE EFI_KERNEL INITRAMFS
 */
#define _POSIX_C_SOURCE 200809L
#include "../native/filesystems/ffs/core/ffs_primitives.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BS=32768, BLOCKS=4096, WORDS=BS/4, HASH=WORDS-56,
       ROOT=BLOCKS/2, BITMAP=2, DATA_START=64 };
enum { SYS=3, KERNEL_DIR=4, IMAGES=5, KERNEL_FILE=6, INITRD_FILE=7 };
static uint8_t blocks[8][BS], bitmap[BS];
static uint8_t used[BLOCKS];
static int image_fd=-1;
static uint32_t next_data=DATA_START;
static int fail(const char *msg) { fprintf(stderr,"FFS fixture: %s: %s\n",msg,strerror(errno));return 1; }
static void put(uint8_t *buf,uint32_t word,uint32_t x)
{
    uint8_t *p=buf+(size_t)word*4u;
    p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);
    p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;
}
static uint32_t get(const uint8_t *buf,uint32_t word)
{
    const uint8_t *p=buf+(size_t)word*4u;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static void fix_checksum(uint8_t *buf,uint32_t position)
{
    uint32_t sum=0;
    put(buf,position,0);
    for(uint32_t i=0;i<WORDS;++i)sum+=get(buf,i);
    put(buf,position,0u-sum);
}
static int disk_write(uint32_t n,const uint8_t *p)
{
    const off_t at=(off_t)n*BS;
    size_t done=0;
    while(done<BS) {
        ssize_t count=pwrite(image_fd,p+done,BS-done,at+(off_t)done);
        if(count<=0)return -1;
        done+=(size_t)count;
    }
    return 0;
}
static void init_name(uint8_t *p,const char *name)
{
    const size_t n=strlen(name);
    if(n>30u)abort();
    p[BS-80]=(uint8_t)n;
    memcpy(p+BS-79,name,n);
}
static void init_directory(uint32_t id,uint32_t parent,const char *name)
{
    uint8_t *p=blocks[id];
    used[id]=1;
    put(p,0,2);put(p,1,id);
    put(p,WORDS-3,parent);put(p,WORDS-1,2);
    init_name(p,name);
}
/* Insert at the head of its native Amiga case-folded FFS hash bucket.
 * Preserve an existing collision through the exact on-disk next pointer. */
static void insert(uint32_t parent,uint32_t child)
{
    /* ROOT lives at the volume midpoint. In this test builder's bounded
     * metadata table it is slot zero, not array index 2048. */
    uint8_t *dir=blocks[parent==ROOT?0u:parent],*entry=blocks[child];
    uint32_t len=entry[BS-80];
    uint32_t hash=ifs_ffs_directory_hash(entry+BS-79,len,HASH,0);
    uint32_t head=get(dir,6u+hash);
    put(entry,WORDS-4,head);
    put(dir,6u+hash,child);
}
static int add_file(uint32_t id,uint32_t parent,const char *name,
                    const char *input_path)
{
    struct stat st;
    FILE *input=fopen(input_path,"rb");
    if(!input)return fail("open boot payload");
    if(fstat(fileno(input),&st) || st.st_size<=0 || (uint64_t)st.st_size>UINT32_MAX) {
        fclose(input);return fail("invalid boot payload size");
    }
    const uint64_t chunks=((uint64_t)st.st_size+BS-1u)/BS;
    if(chunks>HASH || next_data+chunks>=ROOT-1u) {
        fclose(input);errno=ENOSPC;return fail("payload exceeds checked FFS image");
    }
    uint8_t *head=blocks[id];
    used[id]=1;
    put(head,0,2);put(head,1,id);put(head,2,(uint32_t)chunks);
    put(head,4,next_data);put(head,WORDS-47,(uint32_t)st.st_size);
    put(head,WORDS-3,parent);put(head,WORDS-1,UINT32_C(0xfffffffd));
    init_name(head,name);
    uint8_t *data=calloc(1u,BS);
    if(!data){fclose(input);return fail("payload buffer allocation");}
    for(uint32_t i=0;i<(uint32_t)chunks;++i) {
        const uint32_t block=next_data++;
        memset(data,0,BS);
        const size_t count=fread(data,1,BS,input);
        if(!count || (ferror(input)) || disk_write(block,data)) {
            free(data);fclose(input);return fail("payload media I/O");
        }
        put(head,6u+HASH-1u-i,block);
        used[block]=1;
    }
    free(data);
    if(fgetc(input)!=EOF || ferror(input)){fclose(input);return fail("payload length changed");}
    fclose(input);
    return 0;
}
int main(int argc,char **argv)
{
    if(argc!=4){fprintf(stderr,"usage: ffs-boot-fixture IMAGE KERNEL INITRD\n");return 2;}
    image_fd=open(argv[1],O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if(image_fd<0)return fail("create image");
    if(ftruncate(image_fd,(off_t)BLOCKS*BS))return fail("image size");
    used[ROOT]=used[BITMAP]=1;
    for(uint32_t i=0;i<DATA_START;++i)used[i]=1;
    uint8_t boot[BS]={0};
    memcpy(boot,"DOS",3);boot[3]=1; /* genuine DOS1 Fast File System */
    if(disk_write(0,boot))return fail("boot block");

    uint8_t *root=blocks[0];
    put(root,0,2);put(root,3,HASH);
    put(root,WORDS-50,UINT32_MAX); /* allocation map clean */
    put(root,WORDS-49,BITMAP);     /* first allocation bitmap pointer */
    put(root,WORDS-1,1);          /* FFS root secondary type */
    init_name(root,"FFSROOT");
    init_directory(SYS,ROOT,"System");
    init_directory(KERNEL_DIR,SYS,"Kernel");
    init_directory(IMAGES,KERNEL_DIR,"Images");
    if(add_file(KERNEL_FILE,ROOT,"vmlinuz",argv[2]) ||
       add_file(INITRD_FILE,IMAGES,"current.initrd",argv[3]))return 1;
    insert(ROOT,SYS);insert(ROOT,KERNEL_FILE);
    insert(SYS,KERNEL_DIR);
    insert(KERNEL_DIR,IMAGES);
    insert(IMAGES,INITRD_FILE);
    /* Block zero is the root metadata in this test builder's memory table.
     * It is stored at the real midpoint block ROOT, not logical block 0. */
    for(uint32_t id=0;id<8u;++id) {
        if(id!=0 && !used[id])continue;
        fix_checksum(blocks[id],5);
        if(disk_write(id==0?ROOT:id,blocks[id]))return fail("metadata I/O");
    }
    /* Amiga bitmap bits count physical blocks from block #2.
     * One bitmap covers this complete 4096-block image at 32 KiB blocks. */
    for(uint32_t block=2;block<BLOCKS;++block) {
        if(!used[block]) {
            uint32_t bit=block-2u;
            uint32_t word=1u+bit/32u;
            put(bitmap,word,get(bitmap,word)|(1u<<(bit%32u)));
        }
    }
    fix_checksum(bitmap,0);
    if(disk_write(BITMAP,bitmap) || fsync(image_fd))return fail("bitmap I/O");
    if(close(image_fd))return fail("close image");
    image_fd=-1;
    puts("FFS_REAL_DOS1_KERNEL_INITRAMFS_IMAGE_PASS");
    return 0;
}
