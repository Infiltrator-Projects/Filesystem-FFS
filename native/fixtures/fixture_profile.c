/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_media.h"
uint32_t ldtm_profile_file_chunks(const LdtmFragmentProfile *p,size_t index) {
    if(!p || index>=p->files || index>=LDTM_TARGET_FILE_COUNT)return 0;
    return p->file_chunks[index] ? p->file_chunks[index] : p->chunks;
}
uint64_t ldtm_profile_file_bytes(const LdtmFragmentProfile *p,size_t index) {
    return (uint64_t)ldtm_profile_file_chunks(p,index)*(uint64_t)(p?p->chunk_kib:0U)*UINT64_C(1024);
}
uint64_t ldtm_profile_payload_bytes(const LdtmFragmentProfile *p) {
    if(!p)return 0;
    uint64_t bytes=0;
    for(size_t i=0;i<p->files;i++)bytes+=ldtm_profile_file_bytes(p,i);
    return bytes;
}
