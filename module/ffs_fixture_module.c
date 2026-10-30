/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ffs_fixture_v1.h"
#include "test_media.h"
int ifs_bind_fixture_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
_Static_assert(sizeof(IfsFfsFragmentProfile)==sizeof(LdtmFragmentProfile),"FFS fixture profile ABI");
static int format(const char *p,const char *label,const IfsDefragServicesV1 *host) {
    if(!p || ifs_bind_fixture_services(host)!=0)return -1;
    int r=ifs_ffs_fixture_format_internal(p,1U,label?label:"LD_FFS");
    if(!r)r=ifs_ffs_fixture_validate_internal(p,1U);
    ifs_unbind_services();
    return r;
}
static int populate(const char *p,const IfsFfsFragmentProfile *profile,const IfsDefragServicesV1 *host) {
    if(!p || !profile || ifs_bind_fixture_services(host)!=0)return -1;
    int r=ifs_ffs_fixture_populate_internal(p,1U,(const LdtmFragmentProfile *)profile);
    ifs_unbind_services();
    return r;
}
static int verify(const char *p,const IfsFfsFragmentProfile *profile,char *detail,size_t size,const IfsDefragServicesV1 *host) {
    if(!p || !profile || ifs_bind_fixture_services(host)!=0)return -1;
    int r=ifs_ffs_fixture_verify_internal(p,1U,(const LdtmFragmentProfile *)profile,detail,size);
    ifs_unbind_services();
    return r;
}
static int verify_after(const char *p,const IfsFfsFragmentProfile *profile,char *detail,size_t size,const IfsDefragServicesV1 *host) {
    if(!p || !profile || ifs_bind_fixture_services(host)!=0)return -1;
    int r=ifs_ffs_fixture_verify_after_internal(p,1U,(const LdtmFragmentProfile *)profile,detail,size);
    ifs_unbind_services();
    return r;
}
static const IfsFfsFixtureV1 api={IFS_FFS_FIXTURE_ABI_V1,sizeof(IfsFfsFixtureV1),
    "ffs",IFS_FFS_FIXTURE_CAP_FORMAT|IFS_FFS_FIXTURE_CAP_POPULATE|IFS_FFS_FIXTURE_CAP_VERIFY,
    format,populate,verify,verify_after};
__attribute__((visibility("default")))
const IfsFfsFixtureV1 *infiltrator_filesystem_ffs_fixture_v1(void) {return &api;}
