/****************************************************************************
 Copyright (c) 2019-present Axmol Engine contributors (see AUTHORS.md).

 https://axmol.dev/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace axslcc::spirv
{

// ============= SPIRV Compilation =============

CompileUnit compile_input(const Options& options, const Target& target);

std::string spirv_to_bytes(const std::vector<uint32_t>& spirv);

// Removes non-runtime reflection/debug extensions and validates the module
// using the statically linked SPIRV-Tools library.
std::vector<uint32_t> make_vulkan_runtime_spirv(const std::vector<uint32_t>& spirv);

bool compile_glsl_to_spirv(std::string_view source_text, ShaderStage stage,
                           std::vector<uint32_t>& spirv, int opt_level,
                           std::string& log);

} // namespace axslcc::spirv
