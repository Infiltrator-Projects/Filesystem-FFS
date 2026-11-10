/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ffs_module.h"
#include <infiltratr/filesystem_module_v2.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#include <stdio.h>
extern const IfsFsModuleApiV1 *infiltrator_filesystem_api_v1(void);
int main(void) {
    const IfsFsModuleApiV1 *a=infiltrator_filesystem_api_v1();
    const IfsFsModuleApiV2 *v2=infiltrator_filesystem_api_v2();
    if(!ifs_fs_api_v2_valid(v2,"ffs")||
       v2->base_v1!=a||
       !ifs_fs_group_v2(v2,IFS_FS_GROUP_VOLUME)||
       (v2->runtime_profiles&IFS_FS_PROFILE_KERNEL))return 1;
    if(!ifs_fs_common_api_valid(a,"ffs")||
       !(a->capabilities&IFS_FS_CAP_READ)||
       !(a->capabilities&IFS_FS_CAP_WRITE)||
       (a->capabilities & (IFS_FS_CAP_FORMAT|IFS_FS_CAP_CREATE|
                          IFS_FS_CAP_RECOVER))){
        fputs("FFS Common V1 ABI invalid\n",stderr);return 1;
    }
    puts("FFS Common V1 ABI passed");
    return 0;
}
