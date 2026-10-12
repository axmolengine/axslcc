/****************************************************************************
 Copyright (c) 2026 Simdsoft Limited.

 https://axmol.dev/

 SPDX-License-Identifier: MIT
 ****************************************************************************/
#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace axslcc::spirv
{

// ============= SPIRV Compilation =============

// HLSL source -> SPIR-V via DXC, with the resource layout injected first.
CompileUnit compile_input(const Options& options, const Target& target);

std::string spirv_to_bytes(const std::vector<uint32_t>& spirv);

// Removes non-runtime reflection/debug extensions and validates the module
// using the statically linked SPIRV-Tools library.
std::vector<uint32_t> make_vulkan_runtime_spirv(const std::vector<uint32_t>& spirv);

} // namespace axslcc::spirv
