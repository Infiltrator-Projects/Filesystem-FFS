cmake_minimum_required(VERSION 3.20)
project(FilesystemFFSUnified VERSION 1.0.0 LANGUAGES C)
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
    GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
set(FFS "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ffs")
set(DEF "${CMAKE_CURRENT_SOURCE_DIR}/native/defrag")
add_library(ffs-unified SHARED
    "${FFS}/module/ffs_module.c"
    "${FFS}/core/ffs_core.c" "${FFS}/core/ffs_primitives.c"
    "${DEF}/affs_native.c" "${DEF}/affs_worker.c"
    compat/legacy_services.c compat/runtime_alloc.c
    module/ffs_defrag_module.c)
set_source_files_properties("${DEF}/affs_worker.c" PROPERTIES
    COMPILE_DEFINITIONS "main=ifs_ffs_worker_main")
target_include_directories(ffs-unified PRIVATE
    "${FFS}/core" "${FFS}/module" "${DEF}"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(ffs-unified PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ffs-unified PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_link_options(ffs-unified PRIVATE -Wl,-z,defs)
target_link_libraries(ffs-unified PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
set_target_properties(ffs-unified PROPERTIES PREFIX "" OUTPUT_NAME "FFS" SUFFIX ".so")
install(TARGETS ffs-unified LIBRARY DESTINATION /System/Filesystems)
