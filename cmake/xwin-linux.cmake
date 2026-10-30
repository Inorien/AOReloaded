# Local toolchain: cross-compile AOReloaded on Linux with clang-cl + lld-link against the
# MSVC CRT and Windows SDK unpacked by `xwin splat` into ~/.xwin.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER   clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER       lld-link)
set(CMAKE_RC_COMPILER  llvm-rc)
set(CMAKE_MT           llvm-mt)
set(CMAKE_AR           llvm-lib)

set(CMAKE_C_COMPILER_TARGET   i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)

set(XWIN_DIR "$ENV{HOME}/.xwin" CACHE PATH "Output directory of xwin splat")

set(_xwin_includes
    "/imsvc${XWIN_DIR}/crt/include"
    "/imsvc${XWIN_DIR}/sdk/include/ucrt"
    "/imsvc${XWIN_DIR}/sdk/include/um"
    "/imsvc${XWIN_DIR}/sdk/include/shared")
list(JOIN _xwin_includes " " _xwin_includes)
set(CMAKE_C_FLAGS_INIT   "${_xwin_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_includes}")
# ... and for llvm-rc (the vendored FreeType's version resource includes windows.h).
set(CMAKE_RC_FLAGS_INIT "/I ${XWIN_DIR}/crt/include /I ${XWIN_DIR}/sdk/include/ucrt")
string(APPEND CMAKE_RC_FLAGS_INIT " /I ${XWIN_DIR}/sdk/include/um /I ${XWIN_DIR}/sdk/include/shared")

set(_xwin_libs
    "/libpath:${XWIN_DIR}/crt/lib/x86"
    "/libpath:${XWIN_DIR}/sdk/lib/um/x86"
    "/libpath:${XWIN_DIR}/sdk/lib/ucrt/x86")
list(JOIN _xwin_libs " " _xwin_libs)
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libs}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# xwin was run without --include-debug-libs: run CMake's compiler checks in Release mode
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
