#pragma once

#include "axslc-spec.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "yasio/tlx/byte_buffer.hpp"

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
    std::vector<std::string> defines;  // per-target builtin preprocessor defines

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
    Target target;
    tlx::byte_buffer data;
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
    tlx::byte_buffer code;
    tlx::byte_buffer refl;
};

struct VariableTypeMap
{
    uint32_t base_type;
    uint32_t vec_size;
    uint32_t columns;
    uint16_t sc_type;
};

} // namespace axslcc
