cmake_minimum_required(VERSION 3.20)

project(db_engine LANGUAGES CXX)

set(PACIFICDB_ENGINE_VERSION "1.2.0" CACHE STRING "PacificDB engine semantic version")
set(PACIFICDB_RELEASE_VERSION "${PACIFICDB_ENGINE_VERSION}")
set(PACIFICDB_RELEASE_CHANNEL "stable")
set(PACIFICDB_RELEASE_NAME "PacificDB ${PACIFICDB_ENGINE_VERSION}")
set(PACIFICDB_RELEASE_TAG "v${PACIFICDB_ENGINE_VERSION}")
set(PACIFICDB_VERSION_DISPLAY "${PACIFICDB_ENGINE_VERSION}")
if(EXISTS "${CMAKE_SOURCE_DIR}/../release.json")
    file(READ "${CMAKE_SOURCE_DIR}/../release.json" PACIFICDB_RELEASE_JSON)
    string(JSON PACIFICDB_RELEASE_BASE GET "${PACIFICDB_RELEASE_JSON}" version)
    if(PACIFICDB_ENGINE_VERSION STREQUAL PACIFICDB_RELEASE_BASE)
        string(JSON PACIFICDB_RELEASE_VERSION GET "${PACIFICDB_RELEASE_JSON}" releaseVersion)
        string(JSON PACIFICDB_RELEASE_CHANNEL GET "${PACIFICDB_RELEASE_JSON}" channel)
        string(JSON PACIFICDB_RELEASE_NAME GET "${PACIFICDB_RELEASE_JSON}" name)
        string(JSON PACIFICDB_RELEASE_TAG GET "${PACIFICDB_RELEASE_JSON}" tag)
        if(NOT PACIFICDB_RELEASE_CHANNEL STREQUAL "stable")
            set(PACIFICDB_VERSION_DISPLAY "${PACIFICDB_ENGINE_VERSION} (${PACIFICDB_RELEASE_NAME}; ${PACIFICDB_RELEASE_VERSION})")
        endif()
    endif()
endif()
set(PACIFICDB_STORAGE_FORMAT_VERSION "2" CACHE STRING "PacificDB on-disk storage format version")
set(PACIFICDB_RAFT_PROTOCOL_VERSION "2" CACHE STRING "PacificDB Raft wire protocol version")
if(NOT PACIFICDB_GIT_COMMIT)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
        OUTPUT_VARIABLE PACIFICDB_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
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

add_executable(db_engine_startup_delay_failpoint_test
    test/startup_delay_failpoint_test.cpp
    src/test_failpoint.cpp
)
target_include_directories(db_engine_startup_delay_failpoint_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(db_engine_startup_delay_failpoint_test PRIVATE
    PACIFICDB_TEST_FAILPOINTS
)
if(MSVC)
    target_compile_options(db_engine_startup_delay_failpoint_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_startup_delay_failpoint_test PRIVATE -UNDEBUG)
endif()

# ------------------ COMMUNITY FEATURES ------------------
set(COMMUNITY_FEATURES
    src/community_catalog.cpp
    src/community_query.cpp
    src/data_durability.cpp
    src/security_manager.cpp
    src/btree_index.cpp
    src/index.cpp
    src/tenant_manager.cpp
    src/ttl_engine.cpp
    src/hnsw_index.cpp
    src/vector_change_registry.cpp
    src/vector_index_catalog.cpp
    src/vector_schema.cpp
    src/vector_index_generation.cpp src/vector_index_incremental.cpp src/vector_change_journal.cpp
    src/native_vector_index_service.cpp
    src/database_engine_vector_native.cpp
    src/native_vector_rpc.cpp
)

# ------------------ SHARED ENGINE CORE ------------------
# Compile the engine implementation once. Previously every test target rebuilt
# the same sources, multiplying release CI time and making compiler results
# depend on which executable happened to compile a source first.
find_package(OpenSSL REQUIRED)
find_library(LZ4_LIBRARY NAMES lz4 liblz4.so.1 REQUIRED)
find_package(re2 CONFIG QUIET)
if(NOT TARGET re2::re2)
    find_path(RE2_INCLUDE_DIR re2/re2.h REQUIRED)
    find_library(RE2_LIBRARY NAMES re2 REQUIRED)
    add_library(pacificdb_re2 UNKNOWN IMPORTED)
    set_target_properties(pacificdb_re2 PROPERTIES
        IMPORTED_LOCATION "${RE2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${RE2_INCLUDE_DIR}")
    add_library(re2::re2 ALIAS pacificdb_re2)
endif()
add_compile_definitions(HAS_OPENSSL)

add_library(pacificdb_socket_runtime STATIC src/socket_runtime.cpp)
target_include_directories(pacificdb_socket_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
if(WIN32)
    target_link_libraries(pacificdb_socket_runtime PUBLIC Ws2_32)
endif()

add_library(pacificdb_local_engine STATIC src/local_engine.cpp)
target_include_directories(pacificdb_local_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(pacificdb_local_engine PUBLIC
    OpenSSL::Crypto pacificdb_socket_runtime)
if(WIN32)
    target_link_libraries(pacificdb_local_engine PUBLIC Advapi32)
endif()

add_library(pacificdb_structured_event STATIC src/structured_event.cpp)
target_include_directories(pacificdb_structured_event PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

add_library(pacificdb_media_upload_state STATIC src/media_upload_state.cpp)
target_include_directories(pacificdb_media_upload_state PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

add_library(pacificdb_flat_vector_core STATIC src/flat_vector_index.cpp)
target_include_directories(pacificdb_flat_vector_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(pacificdb_flat_vector_core PUBLIC pthread)
endif()
add_library(pacificdb_engine_core STATIC
    ${ENGINE_CORE}
    ${COMMUNITY_FEATURES}
)
target_link_libraries(pacificdb_engine_core PUBLIC re2::re2)

# Keep destructive failpoints out of production binaries while allowing the
# crash-consistency driver to exercise the real LSM implementation.  The
# engine core references this interface but each final executable selects
# exactly one runtime below.
add_library(pacificdb_test_failpoints_disabled STATIC src/test_failpoint.cpp)
add_library(pacificdb_lexical_core STATIC src/lexical_index.cpp)
target_include_directories(pacificdb_lexical_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pacificdb_engine_core PUBLIC pacificdb_lexical_core pacificdb_flat_vector_core)
target_include_directories(pacificdb_test_failpoints_disabled PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
add_library(pacificdb_test_failpoints_enabled STATIC src/test_failpoint.cpp)
target_include_directories(pacificdb_test_failpoints_enabled PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(pacificdb_test_failpoints_enabled PRIVATE
    PACIFICDB_TEST_FAILPOINTS
)
target_link_libraries(pacificdb_engine_core PUBLIC
    pacificdb_media_upload_state pacificdb_structured_event)
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
target_link_libraries(db_engine PRIVATE
    pacificdb_engine_core pacificdb_socket_runtime pacificdb_local_engine
    pacificdb_structured_event pacificdb_test_failpoints_disabled)
target_include_directories(db_engine PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_library(pacificdb_cli_connection STATIC src/cli_connection.cpp)
target_include_directories(pacificdb_cli_connection PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pacificdb_cli_connection PUBLIC OpenSSL::SSL pacificdb_socket_runtime)

add_executable(pacificdb src/shell.cpp src/community_shell.cpp)
target_include_directories(pacificdb PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(pacificdb PRIVATE OpenSSL::Crypto pacificdb_cli_connection)
target_link_libraries(pacificdb PRIVATE
    pacificdb_socket_runtime pacificdb_local_engine)
if(WIN32)
    target_link_libraries(pacificdb PRIVATE Ws2_32)
endif()

add_executable(db_engine_cli_connection_test test/cli_connection_test.cpp)
target_link_libraries(db_engine_cli_connection_test PRIVATE pacificdb_cli_connection)
target_include_directories(db_engine_cli_connection_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_cli_connection_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_cli_connection_test PRIVATE -UNDEBUG)
endif()

# ------------------ TEST EXECUTABLES ------------------
enable_testing()

function(add_pacificdb_engine_test target source)
    add_executable(${target} ${source})
    if(${target} STREQUAL "db_engine_lsm_checkpoint_crash_driver" OR
       ${target} STREQUAL "db_engine_native_vector_crash_test")
        target_link_libraries(${target} PRIVATE
            pacificdb_engine_core pacificdb_test_failpoints_enabled)
    else()
        target_link_libraries(${target} PRIVATE
            pacificdb_engine_core pacificdb_test_failpoints_disabled)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

add_pacificdb_engine_test(db_engine_vector_correctness_test test/test_vector_correctness.cpp)
add_pacificdb_engine_test(db_engine_vector_exact_test test/test_vector_exact.cpp)
add_pacificdb_engine_test(db_engine_vector_safety_concurrency_test test/vector_safety_concurrency_test.cpp)
add_test(NAME db_engine_vector_safety_concurrency_test COMMAND db_engine_vector_safety_concurrency_test)
add_executable(db_engine_query_request_state_test test/query_request_state_test.cpp
    src/query_limiter.cpp src/query_cancellation.cpp)
target_include_directories(db_engine_query_request_state_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_query_request_state_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_query_request_state_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_query_request_state_test PRIVATE pthread)
endif()
add_test(NAME db_engine_query_request_state_test COMMAND db_engine_query_request_state_test)
add_executable(db_engine_hnsw_core_test test/hnsw_core_test.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_hnsw_core_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_hnsw_core_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_hnsw_core_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_hnsw_core_test PRIVATE pthread)
endif()
add_test(NAME db_engine_hnsw_core_test COMMAND db_engine_hnsw_core_test)
add_executable(db_engine_vector_generation_test test/vector_index_generation_test.cpp
    src/vector_index_generation.cpp src/vector_index_incremental.cpp src/vector_change_journal.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_vector_generation_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_vector_generation_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_generation_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_vector_generation_test PRIVATE pthread)
endif()
add_test(NAME db_engine_vector_generation_test COMMAND db_engine_vector_generation_test)
add_executable(db_engine_vector_freshness_test test/vector_freshness_test.cpp)
target_include_directories(db_engine_vector_freshness_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_vector_freshness_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_freshness_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_vector_freshness_test PRIVATE pthread)
endif()
add_test(NAME db_engine_vector_freshness_test COMMAND db_engine_vector_freshness_test)
add_pacificdb_engine_test(db_engine_vector_freshness_storage_test test/vector_freshness_storage_test.cpp)
add_test(NAME db_engine_vector_freshness_storage_test COMMAND db_engine_vector_freshness_storage_test)
add_pacificdb_engine_test(db_engine_vector_index_catalog_test test/vector_index_catalog_test.cpp)
add_test(NAME db_engine_vector_index_catalog_test COMMAND db_engine_vector_index_catalog_test)
add_pacificdb_engine_test(db_engine_wal_recovery_admission_test test/wal_recovery_admission_test.cpp)
add_test(NAME db_engine_wal_recovery_admission_test COMMAND db_engine_wal_recovery_admission_test)
add_pacificdb_engine_test(db_engine_collection_vector_identity_test test/collection_vector_identity_test.cpp)
add_test(NAME db_engine_collection_vector_identity_test COMMAND db_engine_collection_vector_identity_test)
add_executable(db_engine_vector_memory_budget_test test/vector_memory_budget_test.cpp
    src/vector_index_generation.cpp src/vector_index_incremental.cpp src/vector_change_journal.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_vector_memory_budget_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_vector_memory_budget_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_memory_budget_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_vector_memory_budget_test PRIVATE pthread)
endif()
add_test(NAME db_engine_vector_memory_budget_test COMMAND db_engine_vector_memory_budget_test)
add_executable(db_engine_vector_value_test test/vector_value_test.cpp)
target_include_directories(db_engine_vector_value_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_vector_value_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_value_test PRIVATE -UNDEBUG)
endif()
add_test(NAME db_engine_vector_value_test COMMAND db_engine_vector_value_test)
add_pacificdb_engine_test(db_engine_vector_named_sparse_storage_test test/vector_named_sparse_storage_test.cpp)
add_test(NAME db_engine_vector_named_sparse_storage_test COMMAND db_engine_vector_named_sparse_storage_test)
add_pacificdb_engine_test(db_engine_vector_named_sparse_crash_test test/vector_named_sparse_crash_test.cpp)
add_test(NAME db_engine_vector_named_sparse_crash_test COMMAND db_engine_vector_named_sparse_crash_test)
set_tests_properties(db_engine_vector_named_sparse_crash_test PROPERTIES SKIP_RETURN_CODE 77)
add_executable(db_engine_native_vector_index_service_test test/native_vector_index_service_test.cpp
    src/native_vector_index_service.cpp src/vector_index_generation.cpp src/vector_index_incremental.cpp src/vector_change_journal.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_native_vector_index_service_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_native_vector_index_service_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_native_vector_index_service_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_native_vector_index_service_test PRIVATE pthread)
endif()
add_test(NAME db_engine_native_vector_index_service_test COMMAND db_engine_native_vector_index_service_test)
add_pacificdb_engine_test(db_engine_native_vector_storage_test test/native_vector_storage_test.cpp)
add_test(NAME db_engine_native_vector_storage_test COMMAND db_engine_native_vector_storage_test)
add_pacificdb_engine_test(db_engine_native_vector_crash_test test/native_vector_crash_test.cpp)
add_test(NAME db_engine_native_vector_crash_test COMMAND db_engine_native_vector_crash_test)
set_tests_properties(db_engine_native_vector_crash_test PROPERTIES SKIP_RETURN_CODE 77)
add_executable(db_engine_lexical_index_test test/lexical_index_test.cpp)
target_link_libraries(db_engine_lexical_index_test PRIVATE pacificdb_lexical_core)
target_include_directories(db_engine_lexical_index_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_lexical_index_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_lexical_index_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_lexical_index_test PRIVATE pthread)
endif()
add_test(NAME db_engine_lexical_index_test COMMAND db_engine_lexical_index_test)
add_pacificdb_engine_test(db_engine_native_hybrid_storage_test test/native_hybrid_storage_test.cpp)
add_test(NAME db_engine_native_hybrid_storage_test COMMAND db_engine_native_hybrid_storage_test)
add_executable(db_engine_hnsw_qualification tools/hnsw_qualification.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_hnsw_qualification PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(db_engine_hnsw_qualification PRIVATE pthread)
endif()
add_executable(db_engine_native_vector_glove_qualification tools/native_vector_glove_qualification.cpp)
target_link_libraries(db_engine_native_vector_glove_qualification PRIVATE pacificdb_engine_core pacificdb_test_failpoints_disabled)
add_executable(db_engine_vector_decode_profile tools/vector_decode_profile.cpp)
target_link_libraries(db_engine_vector_decode_profile PRIVATE pacificdb_engine_core pacificdb_test_failpoints_disabled)
target_compile_definitions(db_engine_vector_decode_profile PRIVATE PACIFICDB_PROFILE_INDEXED PACIFICDB_PROFILE_RAW)
add_executable(db_engine_native_vector_qualification tools/native_vector_qualification.cpp)
target_link_libraries(db_engine_native_vector_qualification PRIVATE pacificdb_engine_core pacificdb_test_failpoints_disabled)
add_pacificdb_engine_test(db_engine_native_flat_storage_test test/native_flat_storage_test.cpp)
add_test(NAME db_engine_native_flat_storage_test COMMAND db_engine_native_flat_storage_test)
add_pacificdb_engine_test(db_engine_flat_vector_index_test test/flat_vector_index_test.cpp)
add_test(NAME db_engine_flat_vector_index_test COMMAND db_engine_flat_vector_index_test)
add_pacificdb_engine_test(db_engine_native_vector_mixed_flow_test test/native_vector_mixed_flow_test.cpp)
add_test(NAME db_engine_native_vector_mixed_flow_test COMMAND db_engine_native_vector_mixed_flow_test)
target_link_libraries(db_engine_vector_generation_test PRIVATE pacificdb_test_failpoints_disabled)
target_link_libraries(db_engine_vector_memory_budget_test PRIVATE pacificdb_test_failpoints_disabled)
target_link_libraries(db_engine_native_vector_index_service_test PRIVATE pacificdb_test_failpoints_disabled)
target_link_libraries(db_engine_vector_generation_test PRIVATE pacificdb_lexical_core)
target_link_libraries(db_engine_vector_generation_test PRIVATE pacificdb_flat_vector_core)
target_link_libraries(db_engine_vector_memory_budget_test PRIVATE pacificdb_lexical_core)
target_link_libraries(db_engine_vector_memory_budget_test PRIVATE pacificdb_flat_vector_core)
target_link_libraries(db_engine_native_vector_index_service_test PRIVATE pacificdb_lexical_core)
target_link_libraries(db_engine_native_vector_index_service_test PRIVATE pacificdb_flat_vector_core)
add_pacificdb_engine_test(db_engine_vector_exact_benchmark tools/vector_exact_benchmark.cpp)
add_test(NAME db_engine_vector_exact_test COMMAND db_engine_vector_exact_test)
add_test(NAME db_engine_vector_correctness_test COMMAND db_engine_vector_correctness_test)
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
add_executable(db_engine_lsm_point_read_flush_test test/lsm_point_read_flush_test.cpp)
target_link_libraries(db_engine_lsm_point_read_flush_test PRIVATE pacificdb_engine_core)
if(MSVC)
    target_compile_options(db_engine_lsm_point_read_flush_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_lsm_point_read_flush_test PRIVATE -UNDEBUG)
endif()
add_test(NAME db_engine_lsm_point_read_flush_test COMMAND db_engine_lsm_point_read_flush_test)
add_pacificdb_engine_test(db_engine_lsm_verified_point_cache_test test/lsm_verified_point_cache_test.cpp)
add_test(NAME db_engine_lsm_verified_point_cache_test COMMAND db_engine_lsm_verified_point_cache_test)
add_pacificdb_engine_test(db_engine_native_vector_point_cache_test test/native_vector_point_cache_test.cpp)
add_test(NAME db_engine_native_vector_point_cache_test COMMAND db_engine_native_vector_point_cache_test)
add_pacificdb_engine_test(db_engine_replica_digest_test test/replica_digest_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_bounded_recovery_test test/lsm_bounded_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_index_bounded_recovery_test test/index_bounded_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_lsm_checkpoint_crash_driver test/lsm_checkpoint_crash_driver.cpp)
foreach(checkpoint_failpoint IN ITEMS
    FP_LSM_CHECKPOINT_AFTER_SST_SYNC
    FP_LSM_CHECKPOINT_AFTER_ARTIFACT_RENAME
    FP_LSM_CHECKPOINT_AFTER_MANIFEST_SYNC
    FP_LSM_CHECKPOINT_AFTER_MANIFEST_RENAME
    FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM
    FP_LSM_CHECKPOINT_AFTER_WAL_RECLAIM)
    string(TOLOWER "${checkpoint_failpoint}" checkpoint_test_name)
    add_test(
        NAME ${checkpoint_test_name}
        COMMAND db_engine_lsm_checkpoint_crash_driver --run-one ${checkpoint_failpoint}
    )
endforeach()
add_pacificdb_engine_test(db_engine_raft_binary_log_test test/raft_binary_log_test.cpp)
add_pacificdb_engine_test(db_engine_raft_health_recovery_test test/raft_health_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_raft_snapshot_metadata_test test/raft_snapshot_metadata_test.cpp)
add_pacificdb_engine_test(db_engine_storage_format_v2_test test/storage_format_v2_test.cpp)
add_pacificdb_engine_test(db_engine_storage_root_migration_test test/storage_root_migration_test.cpp)
add_pacificdb_engine_test(db_engine_partitioned_queue_test test/test_db_task_queue_partitioned.cpp)
add_pacificdb_engine_test(db_engine_work_stealing_test test/test_db_task_queue_work_stealing.cpp)
add_pacificdb_engine_test(db_engine_community_manual_operations_test
    test/community_manual_operations_test.cpp)
add_pacificdb_engine_test(db_engine_community_catalog_test
    test/community_catalog_test.cpp)
add_pacificdb_engine_test(db_engine_database_access_test
    test/database_access_test.cpp)
add_test(NAME db_engine_database_access_test COMMAND db_engine_database_access_test)
add_pacificdb_engine_test(db_engine_community_api_key_test
    test/community_api_key_test.cpp)
add_pacificdb_engine_test(db_engine_security_manager_lockout_test
    test/security_manager_lockout_test.cpp)
add_test(NAME db_engine_security_manager_lockout_test
    COMMAND db_engine_security_manager_lockout_test)
add_pacificdb_engine_test(db_engine_security_audit_test test/security_audit_test.cpp)
add_test(NAME db_engine_security_audit_test COMMAND db_engine_security_audit_test)
add_pacificdb_engine_test(db_engine_community_query_test
    test/community_query_test.cpp)
add_test(NAME db_engine_community_query_test COMMAND db_engine_community_query_test)
add_test(NAME db_engine_query_limit_test COMMAND db_engine_query_limit_test)
add_test(NAME db_engine_timeout_test COMMAND db_engine_timeout_test)
add_executable(db_engine_native_shell_parser_test
    test/native_shell_parser_test.cpp src/community_shell.cpp)
if(MSVC)
    target_compile_options(db_engine_native_shell_parser_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_native_shell_parser_test PRIVATE -UNDEBUG)
endif()
target_include_directories(db_engine_native_shell_parser_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)

add_executable(db_engine_socket_runtime_test test/socket_runtime_test.cpp)
target_link_libraries(db_engine_socket_runtime_test PRIVATE
    pacificdb_socket_runtime)
if(MSVC)
    target_compile_options(db_engine_socket_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_socket_runtime_test PRIVATE -UNDEBUG)
endif()

add_executable(db_engine_local_engine_state_test
    test/local_engine_state_test.cpp)
target_include_directories(db_engine_local_engine_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(db_engine_local_engine_state_test PRIVATE
    pacificdb_local_engine)
if(MSVC)
    target_compile_options(db_engine_local_engine_state_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_local_engine_state_test PRIVATE -UNDEBUG)
endif()

add_executable(db_engine_env_config_unicode_path_test
    test/env_config_unicode_path_test.cpp src/env_config.cpp)
target_include_directories(db_engine_env_config_unicode_path_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_env_config_unicode_path_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_env_config_unicode_path_test PRIVATE -UNDEBUG)
endif()

add_executable(db_engine_storage_root_guard_owner_test
    test/storage_root_guard_owner_test.cpp src/storage_root_guard.cpp)
target_include_directories(db_engine_storage_root_guard_owner_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_storage_root_guard_owner_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_storage_root_guard_owner_test PRIVATE -UNDEBUG)
endif()

add_executable(db_engine_structured_event_test
    test/structured_event_test.cpp)
target_link_libraries(db_engine_structured_event_test PRIVATE
    pacificdb_structured_event)
if(MSVC)
    target_compile_options(db_engine_structured_event_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_structured_event_test PRIVATE -UNDEBUG)
endif()

add_executable(db_engine_media_upload_state_test
    test/media_upload_state_test.cpp)
target_link_libraries(db_engine_media_upload_state_test PRIVATE
    pacificdb_media_upload_state)
if(MSVC)
    target_compile_options(db_engine_media_upload_state_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_media_upload_state_test PRIVATE -UNDEBUG)
endif()

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
    install(FILES ${CMAKE_SOURCE_DIR}/../packaging/pacificdb-path.ps1
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb)
endif()
install(FILES
    ${CMAKE_SOURCE_DIR}/../README.md
    ${CMAKE_SOURCE_DIR}/../COMMUNITY_SCOPE.md
    ${CMAKE_SOURCE_DIR}/../LICENSE
    ${CMAKE_SOURCE_DIR}/.env.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb
)
install(FILES ${CMAKE_SOURCE_DIR}/../site/assets/pacificdb-logo-symbol.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb
    RENAME pacificdb-logo.png)

set(CPACK_PACKAGE_NAME "pacificdb-community")
set(CPACK_PACKAGE_VENDOR "PacificDB")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PacificDB Community distributed document database")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/hitesh-reddy-k/pacificdb-community")
set(CPACK_PACKAGE_VERSION "${PACIFICDB_RELEASE_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "pacificdb-community-${PACIFICDB_RELEASE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
configure_file(${CMAKE_SOURCE_DIR}/../LICENSE ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_CHECKSUM SHA256)
if(WIN32)
    set(CPACK_GENERATOR NSIS)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "PacificDB Community")
    set(CPACK_NSIS_DISPLAY_NAME "PacificDB Community")
    set(CPACK_NSIS_PACKAGE_NAME "PacificDB Community")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # Windows installer numeric fields cannot contain a SemVer prerelease suffix.
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" PACIFICDB_NUMERIC_VERSION "${PACIFICDB_ENGINE_VERSION}")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
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
    set(CPACK_PRODUCTBUILD_VERSION "${PACIFICDB_ENGINE_VERSION}")
else()
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "PacificDB Community")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    string(REPLACE "-" "~" CPACK_DEBIAN_PACKAGE_VERSION
        "${PACIFICDB_RELEASE_VERSION}")
endif()
include(CPack)

add_pacificdb_engine_test(db_engine_tenant_auth_recovery_test test/tenant_auth_recovery_test.cpp)
add_pacificdb_engine_test(db_engine_database_metadata_failure_test test/database_metadata_failure_test.cpp)
add_test(NAME db_engine_database_metadata_failure_test COMMAND db_engine_database_metadata_failure_test)
add_pacificdb_engine_test(db_engine_raft_commit_publication_test test/raft_commit_publication_test.cpp)
add_test(NAME db_engine_raft_commit_publication_test COMMAND db_engine_raft_commit_publication_test)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME db_engine_raft_log_sync_failure_test COMMAND db_engine_raft_commit_publication_test --log-sync-failure)
    add_test(NAME db_engine_raft_directory_sync_failure_test COMMAND db_engine_raft_commit_publication_test --directory-sync-failure)
    add_test(NAME db_engine_raft_group_sync_failure_test COMMAND db_engine_raft_commit_publication_test --log-sync-failure)
    set_tests_properties(db_engine_raft_group_sync_failure_test PROPERTIES ENVIRONMENT "RAFT_LOG_GROUP_FSYNC_ENABLED=1" TIMEOUT 15)
endif()
add_test(NAME db_engine_tenant_auth_recovery_test COMMAND db_engine_tenant_auth_recovery_test)
add_test(NAME db_engine_tenant_database_access_test COMMAND db_engine_tenant_auth_recovery_test access)
set_tests_properties(db_engine_tenant_database_access_test PROPERTIES TIMEOUT 15)

add_pacificdb_engine_test(db_engine_vector_schema_storage_test test/vector_schema_storage_test.cpp)
add_test(NAME db_engine_vector_schema_storage_test COMMAND db_engine_vector_schema_storage_test)

add_executable(db_engine_hnsw_incremental_test test/hnsw_incremental_test.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_hnsw_incremental_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_hnsw_incremental_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_hnsw_incremental_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_hnsw_incremental_test PRIVATE pthread)
endif()
add_test(NAME db_engine_hnsw_incremental_test COMMAND db_engine_hnsw_incremental_test)

add_executable(db_engine_vector_change_journal_test test/vector_change_journal_test.cpp src/vector_change_journal.cpp)
target_include_directories(db_engine_vector_change_journal_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(db_engine_vector_change_journal_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_change_journal_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_vector_change_journal_test PRIVATE pthread)
endif()
add_test(NAME db_engine_vector_change_journal_test COMMAND db_engine_vector_change_journal_test)

add_executable(db_engine_vector_incremental_generation_test test/vector_incremental_generation_test.cpp
    src/vector_index_generation.cpp src/vector_index_incremental.cpp src/vector_change_journal.cpp src/hnsw_index.cpp)
target_include_directories(db_engine_vector_incremental_generation_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(db_engine_vector_incremental_generation_test PRIVATE pacificdb_test_failpoints_disabled pacificdb_lexical_core pacificdb_flat_vector_core)
if(MSVC)
    target_compile_options(db_engine_vector_incremental_generation_test PRIVATE /UNDEBUG)
else()
    target_compile_options(db_engine_vector_incremental_generation_test PRIVATE -UNDEBUG)
    target_link_libraries(db_engine_vector_incremental_generation_test PRIVATE pthread)
endif()
add_test(NAME db_engine_vector_incremental_generation_test COMMAND db_engine_vector_incremental_generation_test)

add_pacificdb_engine_test(db_engine_native_incremental_storage_test test/native_incremental_storage_test.cpp)
add_test(NAME db_engine_native_incremental_storage_test COMMAND db_engine_native_incremental_storage_test)
