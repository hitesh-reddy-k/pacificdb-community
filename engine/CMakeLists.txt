cmake_minimum_required(VERSION 3.20)

project(db_engine LANGUAGES CXX)

set(PACIFICDB_ENGINE_VERSION "11.5.0" CACHE STRING "PacificDB engine semantic version")
set(PACIFICDB_STORAGE_FORMAT_VERSION "2" CACHE STRING "PacificDB on-disk storage format version")
set(PACIFICDB_RAFT_PROTOCOL_VERSION "2" CACHE STRING "PacificDB Raft wire protocol version")
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
    OUTPUT_VARIABLE PACIFICDB_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PACIFICDB_GIT_COMMIT)
    set(PACIFICDB_GIT_COMMIT "unknown")
endif()
set(PACIFICDB_BUILD_TIMESTAMP "" CACHE STRING
    "UTC build timestamp embedded in the release identity")
if(NOT PACIFICDB_BUILD_TIMESTAMP)
    # Default to immutable source metadata. Wall-clock configure time makes two
    # builds from the same commit differ and needlessly recompiles server.cpp.
    execute_process(
        COMMAND git show -s --format=%cI HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
        OUTPUT_VARIABLE PACIFICDB_BUILD_TIMESTAMP
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT PACIFICDB_BUILD_TIMESTAMP)
        set(PACIFICDB_BUILD_TIMESTAMP "unknown")
    endif()
endif()
set(PACIFICDB_COMPILER "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
configure_file(
    ${CMAKE_SOURCE_DIR}/include/build_identity.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_LIBFUZZER "Enable libFuzzer targets" OFF)
option(PACIFICDB_ENABLE_TEST_FAILPOINTS
    "Compile destructive correctness failpoints for disposable test binaries" OFF)

if(PACIFICDB_ENABLE_TEST_FAILPOINTS)
    add_compile_definitions(PACIFICDB_TEST_FAILPOINTS)
endif()

# ── x86 intrinsics guard ────────────────────────────────────────────────────
# immintrin.h (AVX/SSE) is only available on x86_64. On ARM64/aarch64 the
# Dockerfile passes -DHAVE_IMMINTRIN=OFF so the C++ source falls back to its
# portable code path (see src/db_task_queue_sharded.cpp).
option(HAVE_IMMINTRIN "Use x86 AVX intrinsics (immintrin.h)" ON)
if(NOT HAVE_IMMINTRIN)
    add_definitions(-DNO_IMMINTRIN)
else()
    # Auto-detect: only set ON when we really are on x86_64
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
        message(STATUS "Non-x86 CPU detected (${CMAKE_SYSTEM_PROCESSOR}) — disabling immintrin")
        add_definitions(-DNO_IMMINTRIN)
    endif()
endif()
# ────────────────────────────────────────────────────────────────────────────

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be enabled together")
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    add_definitions(-DPACIFICDB_ASAN)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    add_definitions(-DPACIFICDB_TSAN)
endif()

# Detect platform
if(WIN32)
    set(PLATFORM_WINDOWS TRUE)
    add_definitions(-D_WIN32_WINNT=0x0601 -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
else()
    set(PLATFORM_LINUX TRUE)
    add_definitions(-DLINUX)
endif()

if(APPLE)
    # macOS has fsync but no fdatasync; fsync provides the stronger guarantee.
    add_compile_definitions(fdatasync=fsync)
endif()


# ------------------ COMMON ENGINE SOURCES ------------------
set(ENGINE_CORE
    src/database_engine.cpp
    src/storage.cpp
    src/storage_format_v2.cpp
    src/lsm_manifest.cpp
    src/wal.cpp
    src/raft_core.cpp
    src/db_task_queue_sharded.cpp
    src/db_task_queue_partitioned.cpp
    src/db_task_queue.cpp
    src/logger.cpp
    src/query_parser.cpp
    src/lsm.cpp
    src/config.cpp
    src/metrics.cpp
    src/exceptions.cpp
    src/extensions.cpp
    src/resource_pool.cpp
    src/connection_pool.cpp
    src/query_cancellation.cpp
    src/query_limiter.cpp
    src/memory_manager.cpp
    src/metrics_exporter.cpp
    src/wal_integrity.cpp
    src/recovery_manager.cpp
    src/storage_root_guard.cpp
    src/test_failpoint.cpp
    src/snapshot_manager.cpp
    src/snapshot_bundle.cpp
    src/garbage_collector.cpp
    src/shard_manager.cpp
    src/backup_manager.cpp
    src/env_config.cpp
)

add_executable(v11_4_apply_exact_boundary_failpoints_test
    tests/v11_4_apply_exact_boundary_failpoints.cpp
    src/test_failpoint.cpp
)
target_include_directories(v11_4_apply_exact_boundary_failpoints_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(v11_4_apply_exact_boundary_failpoints_test PRIVATE
    PACIFICDB_TEST_FAILPOINTS
)

# ------------------ COMMUNITY FEATURES ------------------
set(COMMUNITY_FEATURES
    src/community_catalog.cpp
    src/data_durability.cpp
    src/security_manager.cpp
    src/btree_index.cpp
    src/index.cpp
    src/tenant_manager.cpp
    src/ttl_engine.cpp
    src/hnsw_index.cpp
)

# ------------------ SHARED ENGINE CORE ------------------
# Compile the engine implementation once. Previously every test target rebuilt
# the same sources, multiplying release CI time and making compiler results
# depend on which executable happened to compile a source first.
find_package(OpenSSL REQUIRED)
find_library(LZ4_LIBRARY NAMES lz4 liblz4.so.1 REQUIRED)
add_compile_definitions(HAS_OPENSSL)

add_library(pacificdb_engine_core STATIC
    ${ENGINE_CORE}
    ${COMMUNITY_FEATURES}
)
target_include_directories(pacificdb_engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(pacificdb_engine_core PRIVATE
    PACIFICDB_STORAGE_FORMAT_VERSION="${PACIFICDB_STORAGE_FORMAT_VERSION}"
)
if(WIN32)
    target_link_libraries(pacificdb_engine_core PUBLIC
        Ws2_32 psapi OpenSSL::SSL OpenSSL::Crypto ${LZ4_LIBRARY}
    )
else()
    target_link_libraries(pacificdb_engine_core PUBLIC
        pthread OpenSSL::SSL OpenSSL::Crypto ${LZ4_LIBRARY}
    )
endif()

# ------------------ MAIN ENGINE (SERVER) ------------------
add_executable(db_engine src/main.cpp src/server.cpp)
target_link_libraries(db_engine PRIVATE pacificdb_engine_core)
target_include_directories(db_engine PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_executable(pacificdb src/shell.cpp)
target_include_directories(pacificdb PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pacificdb PRIVATE OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(pacificdb PRIVATE Ws2_32)
endif()

# ------------------ TEST EXECUTABLES ------------------
function(add_pacificdb_engine_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE pacificdb_engine_core)
endfunction()

add_pacificdb_engine_test(db_engine_vector_correctness_test test/test_vector_correctness.cpp)
add_pacificdb_engine_test(db_engine_lsm_compaction_test test/test_lsm_compaction.cpp)
add_pacificdb_engine_test(db_engine_strong_read_memtable_test test/test_strong_read_memtable.cpp)
add_pacificdb_engine_test(db_engine_comprehensive_test test/test_comprehensive.cpp)
add_pacificdb_engine_test(db_engine_timeout_test src/test_timeout.cpp)
add_pacificdb_engine_test(db_engine_query_limit_test src/test_query_limiter.cpp)
add_pacificdb_engine_test(db_engine_memory_test test/test_memory_manager.cpp)
add_pacificdb_engine_test(db_engine_snapshot_e2e test/snapshot_e2e.cpp)
add_pacificdb_engine_test(db_engine_snapshot_bundle_test test/snapshot_bundle_test.cpp)
add_pacificdb_engine_test(db_engine_wal_group_commit_test test/wal_group_commit_test.cpp)
add_pacificdb_engine_test(db_engine_wal_segment_recovery_test test/wal_segment_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_manifest_test test/lsm_manifest_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_checkpoint_test test/lsm_checkpoint_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_bounded_recovery_test test/lsm_bounded_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_index_bounded_recovery_test test/index_bounded_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_checkpoint_crash_driver test/lsm_checkpoint_crash_driver.cpp)
add_pacificdb_engine_test(db_engine_raft_binary_log_test test/raft_binary_log_test.cpp)
add_pacificdb_engine_test(db_engine_raft_health_recovery_test test/raft_health_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_raft_snapshot_metadata_test test/raft_snapshot_metadata_test.cpp)
add_pacificdb_engine_test(db_engine_storage_format_v2_test test/storage_format_v2_test.cpp)
add_pacificdb_engine_test(db_engine_storage_root_migration_test test/storage_root_migration_test.cpp)
add_pacificdb_engine_test(db_engine_storage_format_v2_benchmark test/storage_format_v2_benchmark.cpp)
add_pacificdb_engine_test(db_engine_partitioned_queue_test test/test_db_task_queue_partitioned.cpp)
add_pacificdb_engine_test(db_engine_work_stealing_test test/test_db_task_queue_work_stealing.cpp)
add_pacificdb_engine_test(db_engine_community_manual_operations_test
    test/community_manual_operations_test.cpp)
add_pacificdb_engine_test(db_engine_community_catalog_test
    test/community_catalog_test.cpp)

add_executable(storage_path_security_test tests/storage_path_security_test.cpp)
target_include_directories(storage_path_security_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ------------------ FUZZ TARGETS ------------------
if(ENABLE_LIBFUZZER)
    add_executable(wal_fuzz fuzz/wal_fuzz.cpp)
    target_link_libraries(wal_fuzz PRIVATE pacificdb_engine_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(wal_fuzz PRIVATE
            -fsanitize=fuzzer,address -fno-omit-frame-pointer
        )
        target_link_options(wal_fuzz PRIVATE -fsanitize=fuzzer,address)
    else()
        message(WARNING "libFuzzer builds are best supported with Clang")
    endif()
endif()

# ------------------ INSTALLERS ------------------
include(GNUInstallDirs)
install(TARGETS db_engine pacificdb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/../packaging/pacificdb-local.cmd
        DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${CMAKE_SOURCE_DIR}/../packaging/pacificdb-path.ps1
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb)
else()
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/../packaging/pacificdb-local
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES
    ${CMAKE_SOURCE_DIR}/../README.md
    ${CMAKE_SOURCE_DIR}/../COMMUNITY_SCOPE.md
    ${CMAKE_SOURCE_DIR}/../LICENSE
    ${CMAKE_SOURCE_DIR}/.env.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb
)

set(CPACK_PACKAGE_NAME "pacificdb-community")
set(CPACK_PACKAGE_VENDOR "PacificDB")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PacificDB Community distributed document database")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/hitesh-reddy-k/pacificdb-community")
set(CPACK_PACKAGE_VERSION "${PACIFICDB_ENGINE_VERSION}")
configure_file(${CMAKE_SOURCE_DIR}/../LICENSE ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_CHECKSUM SHA256)
if(WIN32)
    set(CPACK_GENERATOR NSIS)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "PacificDB Community")
    set(CPACK_NSIS_DISPLAY_NAME "PacificDB Community")
    set(CPACK_NSIS_PACKAGE_NAME "PacificDB Community")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [[
        ExecWait '\"$SYSDIR\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"$INSTDIR\\share\\pacificdb\\pacificdb-path.ps1\" add \"$INSTDIR\\bin\"' $0
        IntCmp $0 0 +2
        Abort
        SendMessage \${HWND_BROADCAST} \${WM_WININICHANGE} 0 \"STR:Environment\" /TIMEOUT=5000
    ]])
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [[
        ExecWait '\"$SYSDIR\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"$INSTDIR\\share\\pacificdb\\pacificdb-path.ps1\" remove \"$INSTDIR\\bin\"' $0
        SendMessage \${HWND_BROADCAST} \${WM_WININICHANGE} 0 \"STR:Environment\" /TIMEOUT=5000
    ]])
elseif(APPLE)
    set(CPACK_GENERATOR productbuild)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "com.pacificdb.community")
else()
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "PacificDB Community")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    string(REPLACE "-" "~" CPACK_DEBIAN_PACKAGE_VERSION
        "${PACIFICDB_ENGINE_VERSION}")
endif()
include(CPack)
