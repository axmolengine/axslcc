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

#include "axslc-spec.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "yasio/tlx/byte_buffer.hpp"  // reflection still uses this

namespace fs = std::filesystem;

namespace axslcc
{

// ============= Enums & Constants =============

enum class ShaderStage
{
    Vertex,
    Fragment,
    Compute
};

enum class InputLang
{
    HLSL,
    GLSL
};

enum class HlslFrontend
{
    DXC,
    Glslang
};

enum class VulkanSamplerMode
{
    Separate,
    Combined
};

// ============= Structs =============

struct ShaderInfo
{
    ShaderStage stage = ShaderStage::Vertex;
    InputLang lang = InputLang::HLSL;
};

struct Target
{
    axslc::ShaderLang lang = axslc::SHADER_LANG_GLSL;
    int profile = 0;
    std::string spec;
    std::vector<std::string_view> defines;  // options.defines + per-target builtin defines merged during setup

    // Decide whether this target's code is emitted as source text (HLSL/GLSL/MSL) rather
    // than as a binary blob (D3D bytecode or SPIR-V). The result depends on the shader
    // language and the -S hint together:
    //   - HLSL : compiled to DXBC/DXIL bytecode via FXC/DXC unless -S is given; on
    //            non-Windows hosts FXC/DXC are unavailable, so source is always kept.
    //   - SPIR-V : always emitted as a SPIR-V binary, never kept as source.
    //   - MSL/GLSL/ESSL : always kept as source text.
    // keep_source_hint mirrors the -S command line flag (Options::keep_source_hint).
    bool isKeepSource(bool keep_source_hint) const
    {
        switch (lang)
        {
        case axslc::SHADER_LANG_HLSL:
            return keep_source_hint;
        case axslc::SHADER_LANG_SPIRV:
            return false;  // always a SPIR-V binary
        default:  // MSL, GLSL, ESSL
            return true;
        }
    }
};

struct Options
{
    fs::path input;
    fs::path output;
    std::vector<std::string> defines;
    std::vector<fs::path> include_dirs;
    std::vector<Target> targets;
    bool archive = false;              // -a
    bool keep_source_hint = false;     // -S   keep HLSL source, don't compile to DXBC/DXIL (D3D targets only)
    int opt_level = 0;                 // -O0 (debug) through -O3
    InputLang input_lang = InputLang::HLSL; // -x
    HlslFrontend hlsl_frontend = HlslFrontend::DXC; // --hlsl-frontend
    VulkanSamplerMode vulkan_sampler_mode = VulkanSamplerMode::Separate; // --vulkan-samplers
    bool msl_ios = false;              // --msl-ios
    bool xlang = false;                 // true if -x was explicitly specified
    ShaderStage stage = ShaderStage::Vertex;  // detected from filename
    std::string cvar;                  // --cvar
};

struct CompileUnit
{
    ShaderStage stage = ShaderStage::Vertex;
    bool is_hlsl = false;
    std::vector<uint32_t> spirv;
};

struct OutputBlob
{
    const Target* target = nullptr;
    std::string data;
};

struct UniformBlockNameOverride
{
    int32_t binding = -1;
    uint16_t descriptor_set = 0;
    std::string name;
};

struct ScTarget
{
    uint32_t lang = 0;
    uint32_t profile = 0;
    uint32_t offset = 0;
    uint32_t stage = 0;
    std::string code;
    std::string refl;
};

struct VariableTypeMap
{
    uint32_t base_type;
    uint32_t vec_size;
    uint32_t columns;
    uint16_t sc_type;
};

} // namespace axslcc
