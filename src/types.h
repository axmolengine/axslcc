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
    bool bytecode = false;             // true if -S not specified for D3D targets, false otherwise
};

struct Options
{
    fs::path input;
    fs::path output;
    std::vector<std::string> defines;
    std::vector<fs::path> include_dirs;
    std::vector<Target> targets;
    bool archive = false;              // -a
    bool keep_source = false;          // -S   keep HLSL source, don't compile to DXBC/DXIL (D3D targets only)
    int opt_level = 0;                 // -O0 (debug) through -O3
    InputLang input_lang = InputLang::HLSL; // -x
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
