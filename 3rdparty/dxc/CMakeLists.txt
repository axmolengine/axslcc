cmake_minimum_required(VERSION 3.18)
project(dxc)

set(DXC_VERSION 1.9.2602.24)
set(DXC_URL "https://github.com/simdsoft/1kiss/releases/download/devtools/dxc-${DXC_VERSION}.7z")
set(DXC_CACHE_DIR "${CMAKE_SOURCE_DIR}/cache")
set(DXC_ARCHIVE "${DXC_CACHE_DIR}/dxc-${DXC_VERSION}.7z")
set(DXC_EXTRACT_DIR "${DXC_CACHE_DIR}/dxc-${DXC_VERSION}")
set(DXC_X_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_x")

# ---------------------------------------------------------------
# 1. Download
# ---------------------------------------------------------------
if(NOT EXISTS "${DXC_ARCHIVE}")
    file(DOWNLOAD "${DXC_URL}" "${DXC_ARCHIVE}" SHOW_PROGRESS)
endif()

# ---------------------------------------------------------------
# 2. Extract to cache/dxc-${DXC_VERSION}/
# ---------------------------------------------------------------
if(NOT EXISTS "${DXC_EXTRACT_DIR}/include")
    file(ARCHIVE_EXTRACT INPUT "${DXC_ARCHIVE}" DESTINATION "${DXC_CACHE_DIR}")
endif()

# ---------------------------------------------------------------
# 3. Copy headers to _x/include/dxc/ (package has flat *.h,
#    but axslcc expects #include <dxc/dxcapi.h>)
# ---------------------------------------------------------------
set(DXC_INCLUDE_DIR "${DXC_X_DIR}/include")
if(NOT EXISTS "${DXC_INCLUDE_DIR}/dxc")
    file(MAKE_DIRECTORY "${DXC_INCLUDE_DIR}/dxc")
    file(GLOB DXC_TOP_HDRS "${DXC_EXTRACT_DIR}/include/*.h")
    foreach(hdr ${DXC_TOP_HDRS})
        get_filename_component(name "${hdr}" NAME)
        file(COPY "${hdr}" DESTINATION "${DXC_INCLUDE_DIR}/dxc/")
    endforeach()
    # also copy subdirectories (hlsl/, Support/)
    file(COPY "${DXC_EXTRACT_DIR}/include/hlsl" DESTINATION "${DXC_INCLUDE_DIR}/")
    file(COPY "${DXC_EXTRACT_DIR}/include/Support" DESTINATION "${DXC_INCLUDE_DIR}/")
endif()

# ---------------------------------------------------------------
# 3b. Copy DXC license to _x/
# ---------------------------------------------------------------
if(NOT EXISTS "${DXC_X_DIR}/LICENSE-dxc.txt")
    file(COPY "${DXC_EXTRACT_DIR}/include/hlsl/LICENSE.txt"
         DESTINATION "${DXC_X_DIR}")
    file(RENAME "${DXC_X_DIR}/LICENSE.txt" "${DXC_X_DIR}/LICENSE-dxc.txt")
endif()

# ---------------------------------------------------------------
# 4. Copy libs to _x/lib/ (no x86)
# ---------------------------------------------------------------
set(DXC_LIB_DIR "${DXC_X_DIR}/lib")
if(NOT EXISTS "${DXC_LIB_DIR}")
    file(COPY "${DXC_EXTRACT_DIR}/lib/" DESTINATION "${DXC_LIB_DIR}")
endif()

# ---------------------------------------------------------------
# 5. Pick the right lib for current platform/arch
# ---------------------------------------------------------------
if(WIN32)
    set(_os win32)
elseif(APPLE)
    set(_os mac)
else()
    set(_os linux)
endif()

if(_os STREQUAL "mac")
    set(_lib_path "${DXC_LIB_DIR}/${_os}")
elseif(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
    set(_lib_path "${DXC_LIB_DIR}/${_os}/arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64|arm64")
    set(_lib_path "${DXC_LIB_DIR}/${_os}/arm64")
else()
    set(_lib_path "${DXC_LIB_DIR}/${_os}/x64")
endif()

message(STATUS "DXC: ${_lib_path}")

# ---------------------------------------------------------------
# 6. Locate library and runtime files
# ---------------------------------------------------------------
if(WIN32)
    find_library(DXC_LIBRARY NAMES dxcompiler PATHS "${_lib_path}" NO_DEFAULT_PATH)
    find_file(DXC_RUNTIME NAMES dxcompiler.dll PATHS "${_lib_path}" NO_DEFAULT_PATH)
    find_file(DXC_RUNTIME_DXIL NAMES dxil.dll PATHS "${_lib_path}" NO_DEFAULT_PATH)
elseif(APPLE)
    find_library(DXC_LIBRARY NAMES libdxcompiler.dylib PATHS "${_lib_path}" NO_DEFAULT_PATH)
    find_file(DXC_RUNTIME NAMES libdxcompiler.dylib PATHS "${_lib_path}" NO_DEFAULT_PATH)
else()
    find_library(DXC_LIBRARY NAMES libdxcompiler.so PATHS "${_lib_path}" NO_DEFAULT_PATH)
    find_file(DXC_RUNTIME NAMES libdxcompiler.so PATHS "${_lib_path}" NO_DEFAULT_PATH)
endif()

if(NOT DXC_LIBRARY)
    message(FATAL_ERROR "dxcompiler not found in ${_lib_path}")
endif()

# Expose to parent
set(DXC_VERSION "${DXC_VERSION}" PARENT_SCOPE)
set(DXC_INCLUDE_DIR "${DXC_INCLUDE_DIR}" PARENT_SCOPE)
set(DXC_LIBRARY "${DXC_LIBRARY}" PARENT_SCOPE)
set(DXC_RUNTIME "${DXC_RUNTIME}" PARENT_SCOPE)
set(DXC_RUNTIME_DXIL "${DXC_RUNTIME_DXIL}" PARENT_SCOPE)

# ---------------------------------------------------------------
# 7. fxc / d3dcompiler_47.dll — Windows only
# ---------------------------------------------------------------
if(WIN32)
    set(DXC_FXC_VERSION 28000.1839)
    set(DXC_FXC_URL "https://github.com/simdsoft/1kiss/releases/download/devtools/fxc-${DXC_FXC_VERSION}.7z")
    set(DXC_FXC_ARCHIVE "${DXC_CACHE_DIR}/fxc-${DXC_FXC_VERSION}.7z")
    set(DXC_FXC_EXTRACT_DIR "${DXC_CACHE_DIR}/fxc-${DXC_FXC_VERSION}")

    if(NOT EXISTS "${DXC_FXC_ARCHIVE}")
        file(DOWNLOAD "${DXC_FXC_URL}" "${DXC_FXC_ARCHIVE}" SHOW_PROGRESS)
    endif()

    if(NOT EXISTS "${DXC_FXC_EXTRACT_DIR}/x64")
        file(ARCHIVE_EXTRACT INPUT "${DXC_FXC_ARCHIVE}" DESTINATION "${DXC_CACHE_DIR}")
    endif()

    foreach(_fxc_arch x64 arm64)
        set(_fxc_target_dir "${DXC_LIB_DIR}/win32/${_fxc_arch}")
        if(NOT EXISTS "${_fxc_target_dir}/d3dcompiler_47.dll")
            file(COPY "${DXC_FXC_EXTRACT_DIR}/${_fxc_arch}/d3dcompiler_47.dll"
                 DESTINATION "${_fxc_target_dir}")
        endif()
    endforeach()

    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64|arm64")
        set(DXC_FXC_RUNTIME "${DXC_LIB_DIR}/win32/arm64/d3dcompiler_47.dll")
    else()
        set(DXC_FXC_RUNTIME "${DXC_LIB_DIR}/win32/x64/d3dcompiler_47.dll")
    endif()

    message(STATUS "DXC FXC: ${DXC_FXC_RUNTIME}")
    set(DXC_FXC_RUNTIME "${DXC_FXC_RUNTIME}" PARENT_SCOPE)
    set(DXC_FXC_VERSION "${DXC_FXC_VERSION}" PARENT_SCOPE)
endif()
