cmake_minimum_required(VERSION 3.23..4.4)
project(axslcc)

message(STATUS "=== Generator: ${CMAKE_GENERATOR} ===")

# SPIRV-Tools is used for the post-SPIR-V passes only (dead-resource elimination,
# runtime-metadata stripping and Vulkan validation). These pins match what axslcc
# built against while SPIRV-Tools came in through glslang's External/ directory.
set(_SPIRV_TOOLS_VER "2ec8457ab33d539b6f1fecc998360c0b8b05ed4f")   # v2026.2
set(_SPIRV_HEADERS_VER "1a22b167081842915a1c78a0b5b5a353a23284aa") # vulkan-sdk-1.4.350.0

 if (WIN32)
    set (CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
endif()

function(remove_compile_options DEST_VAR COMPILER_FLAGS FLAGS)
        separate_arguments(FLAGS)
        foreach(FLAG ${FLAGS})
            string(REPLACE "${FLAG}" "" COMPILER_FLAGS "${COMPILER_FLAGS}")
        endforeach()
        set(${DEST_VAR} ${COMPILER_FLAGS} PARENT_SCOPE)
endfunction()

if(WIN32)
    option(STATIC_LINK "Statically link with system standard libraries. Affects release build only" ON)
else()
    option(STATIC_LINK "Statically link with system standard libraries. Affects release build only" OFF)
endif()
# SPIRV-Tools is consumed as a static library only: axslcc links SPIRV-Tools-opt
# for the post-SPIR-V passes. The command-line tools, tests and install rules of
# the vendored copy are never used.
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "Skip building the executable and tests along with the library" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "Skip building tests along with the library" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "Enable error on warning" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "Skip installation" FORCE)

set(AXSLCC_DXC_ROOT "" CACHE PATH "DXC distribution root (contains include/dxc and lib/bin)")
if (WIN32)
    option(ENABLE_D3D11_COMPILER "Use Direct3D11 compiler (d3dcompiler.lib + d3dcompiler_47.dll)" ON)
endif()

function(source_group_by_dir ROOT_DIR SOURCES)
    get_filename_component(ROOT_DIR_ABS "${ROOT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(FILE_PATH IN LISTS ${SOURCES})
        get_filename_component(FILE_PATH_ABS "${FILE_PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(FILE_DIR "${FILE_PATH_ABS}" DIRECTORY)
        file(RELATIVE_PATH GROUP_DIR "${ROOT_DIR_ABS}" "${FILE_DIR}")
        string(REPLACE "/" "\\" GROUP_NAME "${GROUP_DIR}")
        source_group("${GROUP_NAME}" FILES "${FILE_PATH_ABS}")
    endforeach()
endfunction()

# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if (MSVC)
  remove_definitions(-D_MBCS)
  add_definitions(-DUNICODE -D_UNICODE)

	remove_compile_options(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" "/GR")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} /wd4530")
	
	if (STATIC_LINK)
		remove_compile_options(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}" "/MD")
		remove_compile_options(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}" "/MD")
		set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
		set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
	endif()
endif()

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

find_program(GIT_COMMAND NAMES git REQUIRED)
if (NOT (IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/3rdparty/spirv-cross"))
    message(STATUS "Downloading SPIRV-Cross ...")
    execute_process(COMMAND ${GIT_COMMAND} clone "https://github.com/KhronosGroup/SPIRV-Cross.git" "${CMAKE_CURRENT_LIST_DIR}/3rdparty/spirv-cross")
endif()
set(SPIRV_CROSS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/3rdparty/spirv-cross")

set(SPIRV_CROSS_PATCH "${CMAKE_CURRENT_LIST_DIR}/spirv-cross-row-major-store.patch")
if (NOT EXISTS "${SPIRV_CROSS_PATCH}")
    message(FATAL_ERROR "Missing SPIRV-Cross patch: ${SPIRV_CROSS_PATCH}")
endif()

execute_process(
    COMMAND ${GIT_COMMAND} -C "${SPIRV_CROSS_SOURCE_DIR}" apply --check "${SPIRV_CROSS_PATCH}"
    RESULT_VARIABLE SPIRV_CROSS_PATCH_CHECK)
if (SPIRV_CROSS_PATCH_CHECK EQUAL 0)
    execute_process(
        COMMAND ${GIT_COMMAND} -C "${SPIRV_CROSS_SOURCE_DIR}" apply "${SPIRV_CROSS_PATCH}"
        RESULT_VARIABLE SPIRV_CROSS_PATCH_APPLY)
    if (NOT SPIRV_CROSS_PATCH_APPLY EQUAL 0)
        message(FATAL_ERROR "Failed to apply SPIRV-Cross patch")
    endif()
else()
    execute_process(
        COMMAND ${GIT_COMMAND} -C "${SPIRV_CROSS_SOURCE_DIR}" apply --reverse --check "${SPIRV_CROSS_PATCH}"
        RESULT_VARIABLE SPIRV_CROSS_PATCH_REVERSE_CHECK)
    if (NOT SPIRV_CROSS_PATCH_REVERSE_CHECK EQUAL 0)
        message(FATAL_ERROR "SPIRV-Cross source is incompatible with ${SPIRV_CROSS_PATCH}")
    endif()
    message(STATUS "SPIRV-Cross row-major store patch already applied")
endif()

set(SPIRV_TOOLS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/3rdparty/spirv-tools")
if (NOT (IS_DIRECTORY "${SPIRV_TOOLS_SOURCE_DIR}"))
    message(STATUS "Downloading SPIRV-Tools ...")
    execute_process(COMMAND ${GIT_COMMAND} clone "https://github.com/KhronosGroup/SPIRV-Tools.git"
                    "${SPIRV_TOOLS_SOURCE_DIR}")
    execute_process(COMMAND ${GIT_COMMAND} -C "${SPIRV_TOOLS_SOURCE_DIR}" checkout ${_SPIRV_TOOLS_VER}
                    RESULT_VARIABLE SPIRV_TOOLS_CHECKOUT)
    if(NOT SPIRV_TOOLS_CHECKOUT EQUAL 0)
        message(FATAL_ERROR "Failed to check out SPIRV-Tools ${_SPIRV_TOOLS_VER}")
    endif()
endif()

# SPIRV-Tools reads the SPIR-V grammar JSON out of its own external/ directory.
set(SPIRV_HEADERS_SOURCE_DIR "${SPIRV_TOOLS_SOURCE_DIR}/external/spirv-headers")
if (NOT (IS_DIRECTORY "${SPIRV_HEADERS_SOURCE_DIR}"))
    message(STATUS "Downloading SPIRV-Headers ...")
    file(MAKE_DIRECTORY "${SPIRV_TOOLS_SOURCE_DIR}/external")
    execute_process(COMMAND ${GIT_COMMAND} clone "https://github.com/KhronosGroup/SPIRV-Headers.git"
                    "${SPIRV_HEADERS_SOURCE_DIR}")
    execute_process(COMMAND ${GIT_COMMAND} -C "${SPIRV_HEADERS_SOURCE_DIR}" checkout ${_SPIRV_HEADERS_VER}
                    RESULT_VARIABLE SPIRV_HEADERS_CHECKOUT)
    if(NOT SPIRV_HEADERS_CHECKOUT EQUAL 0)
        message(FATAL_ERROR "Failed to check out SPIRV-Headers ${_SPIRV_HEADERS_VER}")
    endif()
endif()

add_subdirectory(3rdparty/dxc)
add_subdirectory(3rdparty/spirv-tools)
add_subdirectory(3rdparty/spirv-cross)

set(axslcc_SOURCES 
    src/axslc-spec.h
    src/types.h
    src/utils.h
    src/utils.cpp
    src/spirv_compiler.h
    src/spirv_compiler.cpp
    src/cross_compiler.h
    src/cross_compiler.cpp
    src/reflection.h
    src/reflection.cpp
    src/sc_writer.h
    src/sc_writer.cpp
    src/compiler.h
    src/compiler.cpp
    src/dxc_compiler.h
    src/dxc_compiler.cpp
    src/main.cpp
    3rdparty/yasio/yasio/yasio.natvis
)

if(WIN32)
    # FXC bytecode compilation backend (Windows only)
    list(APPEND axslcc_SOURCES
        src/fxc_compiler.h
        src/fxc_compiler.cpp
    )
endif()

add_subdirectory(3rdparty/fmt)

add_executable(axslcc ${axslcc_SOURCES})
target_link_libraries(axslcc PRIVATE
    SPIRV-Tools-opt
    spirv-cross-glsl
    spirv-cross-hlsl
    spirv-cross-msl
    fmt-header-only
)

target_include_directories(axslcc PRIVATE 3rdparty/yasio)

# DXC is provided by 3rdparty/dxc (auto-downloaded prebuilt)
target_include_directories(axslcc PRIVATE "${DXC_INCLUDE_DIR}")
target_link_libraries(axslcc PRIVATE "${DXC_LIBRARY}")

if(WIN32)
    target_link_libraries(axslcc PRIVATE dxguid d3dcompiler)
elseif(APPLE)
    set_target_properties(axslcc PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
else()
    set_target_properties(axslcc PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

if(DXC_RUNTIME)
    add_custom_command(TARGET axslcc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DXC_RUNTIME}" "$<TARGET_FILE_DIR:axslcc>")
else()
    message(WARNING "DXC runtime library was not found for packaging; axslcc may require a system runtime")
endif()

if(WIN32 AND DXC_RUNTIME_DXIL)
    add_custom_command(TARGET axslcc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DXC_RUNTIME_DXIL}" "$<TARGET_FILE_DIR:axslcc>")
endif()

# ------------------------------------------------------------------------------
# Helper Function: get_all_targets
# Description: 
#   A helper function to collect all targets (including those in subdirectories)
#   into a list variable. This solves the limitation of CMake's native target listing.
# Parameters:
#   result - Output variable name to store the list of all targets
# ------------------------------------------------------------------------------
function(get_all_targets result)
    # Initialize an empty list to store targets
    set(targets "")
    
    # Define a recursive function to collect targets from all directories
    function(_get_all_targets_recursive dir)
        # Get targets in the current directory
        get_property(current_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        
        # Add current directory's targets to the global list
        list(APPEND targets ${current_targets})
        
        # Get subdirectories of the current directory
        get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
        
        # Recursively process all subdirectories
        foreach(subdir IN LISTS subdirs)
            _get_all_targets_recursive(${subdir})
        endforeach()
        
        # Update the global targets list (PARENT_SCOPE to pass up)
        set(targets ${targets} PARENT_SCOPE)
    endfunction()

    # Start recursive collection from the top-level source directory
    _get_all_targets_recursive(${CMAKE_SOURCE_DIR})
    
    # Pass the collected targets to the output variable
    set(${result} ${targets} PARENT_SCOPE)
endfunction()

 # Get the list of all targets defined in the current CMake context
get_all_targets(all_targets)

# Iterate through each target in the collected list
foreach(target IN LISTS all_targets)
    # Check if the target is an IMPORTED target (external dependency)
    get_target_property(is_imported ${target} IMPORTED)
    
    # Skip imported targets to prevent modifying external libraries
    if(NOT is_imported)
        # Set C++ standard to 20 for the target (C++20)
        set_target_properties(${target} PROPERTIES
            C_STANDARD ${CMAKE_C_STANDARD}
            CXX_STANDARD ${CMAKE_CXX_STANDARD}
            # Enforce the standard (disable compiler-specific extensions)
            CXX_STANDARD_REQUIRED ON
        )
        
        # Optional: Print log to confirm the setting (comment out if not needed)
        message(STATUS "Set C++${CMAKE_CXX_STANDARD} standard for target: ${target}")
    endif()
endforeach()

if(MSVC_IDE)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT axslcc)
endif()

# ------------------------------------------------------------------------------
# Generate README.md with version info
# ------------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" _axslcc_ver_line
     REGEX "^#define AXSLCC_VERSION_STRING ")
string(REGEX REPLACE "^#define AXSLCC_VERSION_STRING \"(.+)\"" "\\1"
       AXSLCC_VERSION "${_axslcc_ver_line}")

# DXC_FXC_VERSION is only set on Windows
if(NOT DXC_FXC_VERSION)
    set(DXC_FXC_VERSION "N/A (non-Windows)")
endif()

configure_file("${CMAKE_SOURCE_DIR}/cmake/README.md.in"
               "${CMAKE_CURRENT_BINARY_DIR}/README.md" @ONLY)

# ------------------------------------------------------------------------------
# Install rules — all under COMPONENT axslcc so users can do:
#   cmake --install build --component axslcc
# ------------------------------------------------------------------------------
install(TARGETS axslcc RUNTIME DESTINATION bin COMPONENT axslcc)

if(DXC_RUNTIME)
    install(FILES "${DXC_RUNTIME}" DESTINATION bin COMPONENT axslcc)
endif()
if(WIN32 AND DXC_RUNTIME_DXIL)
    install(FILES "${DXC_RUNTIME_DXIL}" DESTINATION bin COMPONENT axslcc)
endif()
if(WIN32 AND DXC_FXC_RUNTIME)
    install(FILES "${DXC_FXC_RUNTIME}" DESTINATION bin COMPONENT axslcc)
endif()

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/README.md"
        DESTINATION . COMPONENT axslcc)

# License files
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION . COMPONENT axslcc)
install(FILES "${CMAKE_SOURCE_DIR}/3rdparty/dxc/LICENSE-fxc.txt"
        DESTINATION . COMPONENT axslcc)

# DXC license (copied to _x/ by 3rdparty/dxc/CMakeLists.txt)
set(_dxc_license "${CMAKE_SOURCE_DIR}/3rdparty/dxc/_x/LICENSE-dxc.txt")
if(EXISTS "${_dxc_license}")
    install(FILES "${_dxc_license}"
            DESTINATION . COMPONENT axslcc)
endif()

# SPIRV-Tools / SPIRV-Headers / spirv-cross / yasio licenses
install(FILES "${SPIRV_TOOLS_SOURCE_DIR}/LICENSE"
        DESTINATION . COMPONENT axslcc
        RENAME "LICENSE-spirv-tools.txt")
install(FILES "${SPIRV_HEADERS_SOURCE_DIR}/LICENSE"
        DESTINATION . COMPONENT axslcc
        RENAME "LICENSE-spirv-headers.txt")
install(FILES "${CMAKE_SOURCE_DIR}/3rdparty/spirv-cross/LICENSE"
        DESTINATION . COMPONENT axslcc
        RENAME "LICENSE-spirv-cross.txt")
install(FILES "${CMAKE_SOURCE_DIR}/3rdparty/yasio/LICENSE"
        DESTINATION . COMPONENT axslcc
        RENAME "LICENSE-yasio.txt")
