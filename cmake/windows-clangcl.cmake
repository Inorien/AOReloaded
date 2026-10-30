# Local toolchain: build AOReloaded on Windows with Visual Studio's clang-cl + lld-link against the MSVC CRT and
# Windows SDK installed with Visual Studio (Build Tools with "C++ Clang Compiler for Windows"). The same compiler as
# xwin-linux.cmake, so the code doesn't care which of the two built it. Needs no Developer Prompt: Visual Studio is
# found with vswhere and the include / library folders are passed explicitly.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT VS_DIR)
    set(_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    execute_process(COMMAND "${_vswhere}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang
                            -property installationPath
                    OUTPUT_VARIABLE _vs_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _vs_dir)
        message(FATAL_ERROR "No Visual Studio with \"C++ Clang Compiler for Windows\" found (vswhere)")
    endif()
    file(TO_CMAKE_PATH "${_vs_dir}" _vs_dir)
    set(VS_DIR "${_vs_dir}" CACHE PATH "Visual Studio / Build Tools folder")
endif()
if(NOT MSVC_TOOLS_DIR)
    file(STRINGS "${VS_DIR}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" _msvc_ver LIMIT_COUNT 1)
    string(STRIP "${_msvc_ver}" _msvc_ver)
    set(MSVC_TOOLS_DIR "${VS_DIR}/VC/Tools/MSVC/${_msvc_ver}" CACHE PATH "MSVC tools (CRT headers, x86 libs)")
endif()
set(WINSDK_DIR "$ENV{ProgramFiles\(x86\)}/Windows Kits/10" CACHE PATH "Windows 10/11 SDK")
if(NOT WINSDK_VERSION)
    # the newest SDK that has the x86 libraries
    file(GLOB _sdk_libs "${WINSDK_DIR}/Lib/10.*")
    list(SORT _sdk_libs COMPARE NATURAL ORDER DESCENDING)
    foreach(_lib IN LISTS _sdk_libs)
        if(EXISTS "${_lib}/um/x86/kernel32.lib")
            get_filename_component(_sdk_ver "${_lib}" NAME)
            break()
        endif()
    endforeach()
    if(NOT _sdk_ver)
        message(FATAL_ERROR "No Windows SDK with x86 libraries in ${WINSDK_DIR}")
    endif()
    set(WINSDK_VERSION "${_sdk_ver}" CACHE STRING "Windows SDK version")
endif()

set(_llvm "${VS_DIR}/VC/Tools/Llvm/x64/bin")
set(CMAKE_C_COMPILER   "${_llvm}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_llvm}/clang-cl.exe")
set(CMAKE_LINKER       "${_llvm}/lld-link.exe")
set(CMAKE_RC_COMPILER  "${_llvm}/llvm-rc.exe")
set(CMAKE_MT           "${_llvm}/llvm-mt.exe")
set(CMAKE_AR           "${_llvm}/llvm-lib.exe")
if(NOT CMAKE_MAKE_PROGRAM)
    set(CMAKE_MAKE_PROGRAM "${VS_DIR}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
        CACHE FILEPATH "Ninja (Visual Studio's)")
endif()

set(CMAKE_C_COMPILER_TARGET   i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)

set(_inc "${WINSDK_DIR}/Include/${WINSDK_VERSION}")
set(_lib "${WINSDK_DIR}/Lib/${WINSDK_VERSION}")
set(_win_includes
    "/imsvc\"${MSVC_TOOLS_DIR}/include\""
    "/imsvc\"${_inc}/ucrt\""
    "/imsvc\"${_inc}/um\""
    "/imsvc\"${_inc}/shared\"")
list(JOIN _win_includes " " _win_includes)
set(CMAKE_C_FLAGS_INIT   "${_win_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_win_includes}")
# ... and for llvm-rc (the vendored FreeType's version resource includes windows.h).
set(CMAKE_RC_FLAGS_INIT "/I \"${MSVC_TOOLS_DIR}/include\" /I \"${_inc}/ucrt\" /I \"${_inc}/um\" /I \"${_inc}/shared\"")

set(_win_libs
    "/libpath:\"${MSVC_TOOLS_DIR}/lib/x86\""
    "/libpath:\"${_lib}/um/x86\""
    "/libpath:\"${_lib}/ucrt/x86\"")
list(JOIN _win_libs " " _win_libs)
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_win_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_win_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_win_libs}")

# As xwin-linux.cmake: run CMake's compiler checks in Release mode.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
