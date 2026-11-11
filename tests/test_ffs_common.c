/* SPDX-License-Identifier: GPL-3.0-or-later
 * One strict Common filesystem V2 contract over the native FFS engine.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    if(!ifs_fs_api_v2_valid(api,"ffs"))return 1;
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    volume=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !core->probe || !core->mount || !core->unmount ||
       !core->stat || !core->directory_entry || !core->read || !core->write ||
       core->create || core->remove || core->truncate ||
       !volume || !volume->query || !volume->sync ||
       volume->format || volume->repair ||
       ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG) ||
       (api->runtime_profiles & IFS_FS_PROFILE_KERNEL) ||
       api->group_mask!=(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME))) {
        fputs("FFS Common V2 capability contract invalid\n",stderr);
        return 1;
    }
    puts("FFS-STRICT-COMMON-V2-CONTRACT-PASS");
    return 0;
}
