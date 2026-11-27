# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(FfsBootReader LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 header root")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
 message(FATAL_ERROR "Set COMMON_INCLUDE_DIR to the exact Common V2 headers")
endif()
add_library(ffs-boot MODULE
 ../../module/ffs_common.c
 ../../native/filesystems/ffs/module/ffs_module.c
 ../../native/filesystems/ffs/core/ffs_core.c
 ../../native/filesystems/ffs/core/ffs_primitives.c
 runtime.c)
set_target_properties(ffs-boot PROPERTIES
 PREFIX "" OUTPUT_NAME "FFS" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_compile_features(ffs-boot PRIVATE c_std_11)
target_include_directories(ffs-boot PRIVATE
 "${COMMON_INCLUDE_DIR}" ../../native/filesystems/ffs/module)
target_compile_definitions(ffs-boot PRIVATE IFS_FFS_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(ffs-boot PRIVATE
 -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin
 -fno-tree-loop-distribute-patterns -fno-stack-protector -mno-red-zone -fcf-protection=none -mgeneral-regs-only
 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(ffs-boot PRIVATE
 -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
