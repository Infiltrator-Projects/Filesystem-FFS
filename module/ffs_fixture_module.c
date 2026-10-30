/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ffs_fixture_v1.h"
#include "test_media.h"
_Static_assert(sizeof(IfsFfsFragmentProfile)==sizeof(LdtmFragmentProfile),"FFS fixture profile ABI");
static int format(const char *p,const char *label) {
    return p && ifs_ffs_fixture_format_internal(p,1U,label?label:"LD_FFS")==0 &&
           ifs_ffs_fixture_validate_internal(p,1U)==0 ? 0 : -1;
}
static int populate(const char *p,const IfsFfsFragmentProfile *profile) {
    return p && profile ? ifs_ffs_fixture_populate_internal(p,1U,(const LdtmFragmentProfile *)profile) : -1;
}
static int verify(const char *p,const IfsFfsFragmentProfile *profile,char *detail,size_t size) {
    return p && profile ? ifs_ffs_fixture_verify_internal(p,1U,(const LdtmFragmentProfile *)profile,detail,size) : -1;
}
static int verify_after(const char *p,const IfsFfsFragmentProfile *profile,char *detail,size_t size) {
    return p && profile ? ifs_ffs_fixture_verify_after_internal(p,1U,(const LdtmFragmentProfile *)profile,detail,size) : -1;
}
static const IfsFfsFixtureV1 api={IFS_FFS_FIXTURE_ABI_V1,sizeof(IfsFfsFixtureV1),
    "ffs",IFS_FFS_FIXTURE_CAP_FORMAT|IFS_FFS_FIXTURE_CAP_POPULATE|IFS_FFS_FIXTURE_CAP_VERIFY,
    format,populate,verify,verify_after};
__attribute__((visibility("default")))
const IfsFfsFixtureV1 *infiltrator_filesystem_ffs_fixture_v1(void) {return &api;}
