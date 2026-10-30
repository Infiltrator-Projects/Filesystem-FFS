/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ffs_defrag_v1.h"
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
int ifs_ffs_worker_main(int, char **);
static int run(int argc, char **argv, const IfsDefragServicesV1 *host) {
    if (ifs_bind_services(host) != 0) return 1;
    const int result = ifs_ffs_worker_main(argc, argv);
    ifs_unbind_services();
    return result;
}
static const IfsFfsDefragV1 api = {
    IFS_FFS_DEFRAG_ABI_V1, sizeof(IfsFfsDefragV1),
    "ffs", IFS_FFS_DEFRAG_CAP_RUN, run
};
__attribute__((visibility("default")))
const IfsFfsDefragV1 *infiltrator_filesystem_ffs_defrag_v1(void) {
    return &api;
}
