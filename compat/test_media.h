/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared Test Media FFS fixture profile; no application-specific parser. */
#ifndef IFS_FFS_TEST_MEDIA_COMPAT_H
#define IFS_FFS_TEST_MEDIA_COMPAT_H
#include <stdint.h>
#include <stddef.h>
#define LDTM_MIB UINT64_C(1048576)
#define LDTM_TARGET_FILE_COUNT 8U
typedef struct {
    uint32_t files, chunks, chunk_kib, directory_initial, directory_second;
    uint32_t file_chunks[LDTM_TARGET_FILE_COUNT];
} LdtmFragmentProfile;
uint32_t ldtm_profile_file_chunks(const LdtmFragmentProfile *, size_t);
uint64_t ldtm_profile_file_bytes(const LdtmFragmentProfile *, size_t);
uint64_t ldtm_profile_payload_bytes(const LdtmFragmentProfile *);
int ifs_ffs_fixture_format_internal(const char *, uint8_t, const char *);
int ifs_ffs_fixture_validate_internal(const char *, uint8_t);
int ifs_ffs_fixture_populate_internal(const char *, uint8_t, const LdtmFragmentProfile *);
int ifs_ffs_fixture_verify_internal(const char *, uint8_t, const LdtmFragmentProfile *, char *, size_t);
int ifs_ffs_fixture_verify_after_internal(const char *, uint8_t, const LdtmFragmentProfile *, char *, size_t);
#endif
