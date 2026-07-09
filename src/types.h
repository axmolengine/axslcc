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

// ============= Structs =============

struct Target
{
    axslc::ShaderLang lang = axslc::SHADER_LANG_GLSL;
    int profile = 0;
    std::string spec;
    std::vector<std::string> defines;  // per-target builtin preprocessor defines
};

struct Options
{
    fs::path input;
    fs::path output;
    std::vector<std::string> defines;
    std::vector<fs::path> include_dirs;
    std::vector<Target> targets;
    bool sc = false;
    bool reflect = false;
    bool migrate = false;
    bool dxil = false;      // Compile HLSL output to DXIL via DXC (Windows only)
    bool dxcReflect = false; // Use DXC reflection for REFL (Windows only, validation)
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
    bool binary = false;
};

struct ScTarget
{
    uint32_t lang = 0;
    uint32_t profile = 0;
    uint32_t offset = 0;
    uint32_t stage = 0;
    tlx::byte_buffer code;
    tlx::byte_buffer refl;
    bool binary = false;
};

struct VariableTypeMap
{
    uint32_t base_type;
    uint32_t vec_size;
    uint32_t columns;
    uint16_t sc_type;
};

} // namespace axslcc
