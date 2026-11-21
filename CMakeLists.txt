cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemFFSUnified VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
    GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
    GIT_TAG 489388567c436f70754db938ebdc4a07c10b7da0)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# Hosted and firmware profiles reuse the same native FFS block parser.
# The embedded ELF imports no libc, Linux VFS, FUSE or special EFI driver.
set(FFS_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(FFS_FIRMWARE_ELF "${FFS_FIRMWARE_DIR}/FFS.so")
add_custom_command(OUTPUT "${FFS_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
  -B "${FFS_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
 COMMAND "${CMAKE_COMMAND}" --build "${FFS_FIRMWARE_DIR}" --parallel 2
 DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c
  module/ffs_common.c
  native/filesystems/ffs/module/ffs_module.c
  native/filesystems/ffs/core/ffs_core.c
  native/filesystems/ffs/core/ffs_primitives.c
  "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(ffs-firmware-elf DEPENDS "${FFS_FIRMWARE_ELF}")

set(FFS "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ffs")
set(DEF "${CMAKE_CURRENT_SOURCE_DIR}/native/defrag")
# Production contains only Common V2 and the source-owned FFS engine.
add_library(ffs-unified SHARED
    "${FFS}/module/ffs_module.c" module/ffs_common.c
    "${FFS}/core/ffs_core.c" "${FFS}/core/ffs_primitives.c")
target_include_directories(ffs-unified PRIVATE
    "${FFS}/core" "${FFS}/module" "${DEF}"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(ffs-unified PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ffs-unified PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_link_options(ffs-unified PRIVATE
    -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map)
target_link_libraries(ffs-unified PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
set_target_properties(ffs-unified PROPERTIES PREFIX "" OUTPUT_NAME "FFS" SUFFIX ".so")
add_dependencies(ffs-unified ffs-firmware-elf)
set_property(TARGET ffs-unified APPEND PROPERTY LINK_DEPENDS "${FFS_FIRMWARE_ELF}")
add_custom_command(TARGET ffs-unified POST_BUILD
 COMMAND "${CMAKE_OBJCOPY}" --add-section
   ".infiltrator_boot=${FFS_FIRMWARE_ELF}"
   --set-section-flags .infiltrator_boot=readonly
   "$<TARGET_FILE:ffs-unified>"
 VERBATIM)
if(BUILD_TESTING)
    add_executable(ffs-common-abi-test tests/test_ffs_common.c)
    target_include_directories(ffs-common-abi-test PRIVATE
        "${FFS}/module" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(ffs-common-abi-test PRIVATE ffs-unified InfiltratrCommon::Common)
    add_test(NAME ffs-common-abi COMMAND ffs-common-abi-test)
    add_test(NAME ffs-production-v2-only
      COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_production_v2.sh"
              "$<TARGET_FILE:ffs-unified>")
    add_executable(ffs-dropin-native-test
      tests/ffs_dropin_module_test.c
      "${FFS}/module/ffs_module.c"
      "${FFS}/core/ffs_core.c" "${FFS}/core/ffs_primitives.c")
    target_include_directories(ffs-dropin-native-test PRIVATE "${FFS}/module" "${FFS}/core")
    add_test(NAME ffs-original-native-namespace COMMAND ffs-dropin-native-test)
    add_executable(ffs-core-test tests/ffs_core_test.c
      "${FFS}/core/ffs_core.c" "${FFS}/core/ffs_primitives.c")
    target_include_directories(ffs-core-test PRIVATE "${FFS}/core")
    add_test(NAME ffs-original-core COMMAND ffs-core-test)
    add_executable(ffs-primitives-test tests/ffs_primitives_test.c
      "${FFS}/core/ffs_primitives.c")
    target_include_directories(ffs-primitives-test PRIVATE "${FFS}/core")
    add_test(NAME ffs-original-primitives COMMAND ffs-primitives-test)
endif()
install(TARGETS ffs-unified LIBRARY DESTINATION /System/Filesystems)
# Install only this filesystem's illustrated PDF beside its native shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/FFS.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-FFS.pdf)
