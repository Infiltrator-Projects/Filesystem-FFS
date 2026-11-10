/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ffs_module.h"
#include <infiltratr/filesystem_module.h>
#include <stdio.h>
extern const IfsFsModuleApiV1 *infiltrator_filesystem_api_v1(void);
int main(void) {
    const IfsFsModuleApiV1 *a=infiltrator_filesystem_api_v1();
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
