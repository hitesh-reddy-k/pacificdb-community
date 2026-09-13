@echo off
setlocal
if "%~1"=="" exit /b 64
if "%~2"=="" exit /b 64
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
if errorlevel 1 exit /b %errorlevel%
if "%~3"=="" (
  cmake -S "%~1\engine" -B "%~2" -G "Visual Studio 17 2022" -A x64
) else (
  cmake -S "%~1\engine" -B "%~2" -G "Visual Studio 17 2022" -A x64 ^
    "-DCMAKE_TOOLCHAIN_FILE=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
    "-DVCPKG_INSTALLED_DIR=%~3" -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
)
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~2" --config Release -j 1 --target db_engine pacificdb db_engine_native_shell_parser_test db_engine_socket_runtime_test db_engine_local_engine_state_test db_engine_env_config_unicode_path_test db_engine_media_upload_state_test db_engine_community_catalog_test db_engine_startup_delay_failpoint_test db_engine_structured_event_test db_engine_storage_root_guard_owner_test db_engine_lsm_manifest_test
exit /b %errorlevel%
