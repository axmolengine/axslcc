#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "axslc-spec.h"

#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/DirStackFileIncluder.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "yasio/obstream.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using axslc::ShaderLang;

constexpr uint16_t kScMajor = 3;
constexpr uint16_t kScMinor = 7;

struct Target
{
    ShaderLang lang = axslc::SHADER_LANG_GLSL;
    int profile = 0;
    std::string spec;
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
};

struct CompileUnit
{
    EShLanguage stage = EShLangVertex;
    glslang::EShSource source = glslang::EShSourceGlsl;
    std::vector<uint32_t> spirv;
};

struct OutputBlob
{
    Target target;
    std::vector<uint8_t> data;
    bool binary = false;
};

struct ScTarget
{
    uint32_t lang = 0;
    uint32_t profile = 0;
    uint32_t offset = 0;
    uint32_t stage = 0;
    std::vector<uint8_t> code;
    std::vector<uint8_t> refl;
    bool binary = false;
};

struct VariableTypeMap
{
    spirv_cross::SPIRType::BaseType base_type;
    uint32_t vec_size;
    uint32_t columns;
    uint16_t sc_type;
};

constexpr VariableTypeMap kVariableTypeMap[] = {
    {spirv_cross::SPIRType::Float, 4, 1, axslc::SC_TYPE_FLOAT4},
    {spirv_cross::SPIRType::Float, 3, 1, axslc::SC_TYPE_FLOAT3},
    {spirv_cross::SPIRType::Float, 2, 1, axslc::SC_TYPE_FLOAT2},
    {spirv_cross::SPIRType::Float, 1, 1, axslc::SC_TYPE_FLOAT},
    {spirv_cross::SPIRType::Int, 4, 1, axslc::SC_TYPE_INT4},
    {spirv_cross::SPIRType::Int, 3, 1, axslc::SC_TYPE_INT3},
    {spirv_cross::SPIRType::Int, 2, 1, axslc::SC_TYPE_INT2},
    {spirv_cross::SPIRType::Int, 1, 1, axslc::SC_TYPE_INT},
    {spirv_cross::SPIRType::UShort, 4, 1, axslc::SC_TYPE_USHORT4},
    {spirv_cross::SPIRType::UShort, 2, 1, axslc::SC_TYPE_USHORT2},
    {spirv_cross::SPIRType::UByte, 4, 1, axslc::SC_TYPE_UBYTE4},
    {spirv_cross::SPIRType::Float, 4, 4, axslc::SC_TYPE_MAT4},
    {spirv_cross::SPIRType::Float, 3, 3, axslc::SC_TYPE_MAT3},
    {spirv_cross::SPIRType::Half, 4, 1, axslc::SC_TYPE_HALF4},
    {spirv_cross::SPIRType::Half, 3, 1, axslc::SC_TYPE_HALF3},
    {spirv_cross::SPIRType::Half, 2, 1, axslc::SC_TYPE_HALF2},
    {spirv_cross::SPIRType::Half, 1, 1, axslc::SC_TYPE_HALF},
};

constexpr std::string_view kAttribNames[] = {
    "POSITION",
    "NORMAL",
    "TEXCOORD0",
    "TEXCOORD1",
    "TEXCOORD2",
    "TEXCOORD3",
    "TEXCOORD4",
    "TEXCOORD5",
    "TEXCOORD6",
    "TEXCOORD7",
    "COLOR0",
    "COLOR1",
    "COLOR2",
    "COLOR3",
    "TANGENT",
    "BINORMAL",
    "BLENDINDICES",
    "BLENDWEIGHT",
};

constexpr std::string_view kSemanticNames[] = {
    "POSITION",
    "NORMAL",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "TEXCOORD",
    "COLOR",
    "COLOR",
    "COLOR",
    "COLOR",
    "TANGENT",
    "BINORMAL",
    "BLENDINDICES",
    "BLENDWEIGHT",
};

constexpr uint16_t kSemanticIndices[] = {
    0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 0, 0, 0, 0,
};

void print_help()
{
    std::cerr
        << "axslcc - Axmol shader compiler\n\n"
        << "Usage:\n"
        << "  axslcc --input <file> [--output <path>] --target=<lang-profile>[;<lang-profile>] [--sc] [--reflect]\n"
        << "  axslcc --input <glsl> --output <path> --target=hlsl-51 --migrate\n\n"
        << "Targets:\n"
        << "  hlsl-50, hlsl-51, gles-300, glsl-330, glsl-450, spirv-100\n\n"
        << "Options:\n"
        << "  --input <file>      Input HLSL 5.1 or GLSL file\n"
        << "  --output <path>     Output file or basename. Defaults to input stem\n"
        << "  --target <targets>  Semicolon-separated output targets\n"
        << "  --sc                Write one Axmol .sc file containing all targets\n"
        << "  --reflect           Include reflection data in .sc output\n"
        << "  --migrate           GLSL to HLSL migration mode\n"
        << "  -DNAME[=VALUE]      Preprocessor define\n"
        << "  -I<dir>             Include directory\n";
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> split(std::string_view value, char delim)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(delim, start);
        if (end == std::string_view::npos)
            end = value.size();
        if (end > start)
            parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
        if (end == value.size())
            break;
    }
    return parts;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void require_value(int argc, char** argv, int& i, std::string_view option, std::string& out)
{
    if (i + 1 >= argc)
        throw std::runtime_error(std::string(option) + " requires a value");
    out = argv[++i];
}

Target parse_target(std::string_view text)
{
    auto dash = text.find('-');
    if (dash == std::string_view::npos)
        throw std::runtime_error("invalid target '" + std::string(text) + "'");

    std::string lang = lower(std::string(text.substr(0, dash)));
    int profile = std::stoi(std::string(text.substr(dash + 1)));

    Target target;
    target.profile = profile;
    target.spec = lang + "-" + std::to_string(profile);

    if (lang == "hlsl" && (profile == 50 || profile == 51)) {
        target.lang = axslc::SHADER_LANG_HLSL;
    } else if (lang == "gles" && profile == 300) {
        target.lang = axslc::SHADER_LANG_ESSL;
    } else if (lang == "glsl" && (profile == 330 || profile == 450)) {
        target.lang = axslc::SHADER_LANG_GLSL;
    } else if (lang == "spirv" && profile == 100) {
        target.lang = axslc::SHADER_LANG_SPIRV;
    } else {
        throw std::runtime_error("unsupported target '" + std::string(text) + "'");
    }

    return target;
}

Options parse_args(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else if (arg == "--input") {
            require_value(argc, argv, i, "--input", value);
            options.input = value;
        } else if (starts_with(arg, "--input=")) {
            options.input = arg.substr(8);
        } else if (arg == "--output") {
            require_value(argc, argv, i, "--output", value);
            options.output = value;
        } else if (starts_with(arg, "--output=")) {
            options.output = arg.substr(9);
        } else if (arg == "--target") {
            require_value(argc, argv, i, "--target", value);
            for (const auto& item : split(value, ';'))
                options.targets.push_back(parse_target(item));
        } else if (starts_with(arg, "--target=")) {
            for (const auto& item : split(arg.substr(9), ';'))
                options.targets.push_back(parse_target(item));
        } else if (arg == "--sc") {
            options.sc = true;
        } else if (arg == "--reflect") {
            options.reflect = true;
        } else if (arg == "--migrate") {
            options.migrate = true;
        } else if (starts_with(arg, "-D")) {
            if (arg.size() == 2) {
                require_value(argc, argv, i, "-D", value);
                options.defines.push_back(value);
            } else {
                options.defines.push_back(arg.substr(2));
            }
        } else if (starts_with(arg, "-I")) {
            if (arg.size() == 2) {
                require_value(argc, argv, i, "-I", value);
                options.include_dirs.emplace_back(value);
            } else {
                options.include_dirs.emplace_back(arg.substr(2));
            }
        } else {
            throw std::runtime_error("unknown option '" + arg + "'");
        }
    }

    if (options.input.empty())
        throw std::runtime_error("--input is required");
    if (options.targets.empty())
        throw std::runtime_error("--target is required");
    if (options.reflect && !options.sc)
        throw std::runtime_error("--reflect is only valid with --sc");
    if (!fs::exists(options.input))
        throw std::runtime_error("input file does not exist: " + options.input.string());

    if (options.output.empty())
        options.output = options.input.parent_path() / options.input.stem();

    if (options.migrate) {
        auto ext = lower(options.input.extension().string());
        if (ext == ".hlsl" || ext == ".fx")
            throw std::runtime_error("--migrate expects GLSL input");
        for (const auto& target : options.targets) {
            if (target.lang != axslc::SHADER_LANG_HLSL)
                throw std::runtime_error("--migrate only supports HLSL output targets");
        }
    }

    return options;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("failed to open input: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::vector<uint8_t>& data)
{
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("failed to open output: " + path.string());
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::optional<EShLanguage> stage_from_name(const fs::path& input)
{
    std::string ext = lower(input.extension().string());
    std::string stem = lower(input.stem().string());

    if (ext == ".vert" || ext == ".vs" || ext == ".vsh" || stem.ends_with("_vs") || stem.ends_with(".vs"))
        return EShLangVertex;
    if (ext == ".frag" || ext == ".fs" || ext == ".fsh" || ext == ".ps" || stem.ends_with("_fs") || stem.ends_with("_ps"))
        return EShLangFragment;
    if (ext == ".comp" || ext == ".cs" || stem.ends_with("_cs"))
        return EShLangCompute;

    return std::nullopt;
}

glslang::EShSource source_from_name(const fs::path& input)
{
    std::string ext = lower(input.extension().string());
    if (ext == ".hlsl" || ext == ".fx")
        return glslang::EShSourceHlsl;
    return glslang::EShSourceGlsl;
}

uint32_t sc_lang(const Target& target)
{
    switch (target.lang) {
    case axslc::SHADER_LANG_ESSL:
        return SC_LANG_GLES;
    case axslc::SHADER_LANG_HLSL:
        return SC_LANG_HLSL;
    case axslc::SHADER_LANG_GLSL:
        return SC_LANG_GLSL;
    case axslc::SHADER_LANG_SPIRV:
        return SC_LANG_SPIRV;
    default:
        return 0;
    }
}

uint32_t sc_stage(EShLanguage stage)
{
    switch (stage) {
    case EShLangVertex:
        return SC_STAGE_VERTEX;
    case EShLangFragment:
        return SC_STAGE_FRAGMENT;
    case EShLangCompute:
        return SC_STAGE_COMPUTE;
    default:
        return 0;
    }
}

std::string build_preamble(const Options& options)
{
    std::string preamble = "#extension GL_GOOGLE_include_directive : require\n";
    for (size_t i = 0; i < std::size(kAttribNames); ++i) {
        preamble += "#define ";
        preamble += kAttribNames[i];
        preamble += " ";
        preamble += std::to_string(i);
        preamble += "\n";
    }
    for (int i = 0; i < 8; ++i)
        preamble += "#define SV_Target" + std::to_string(i) + " " + std::to_string(i) + "\n";

    for (const auto& define : options.defines) {
        auto eq = define.find('=');
        preamble += "#define ";
        preamble += eq == std::string::npos ? define : define.substr(0, eq);
        if (eq != std::string::npos) {
            preamble += " ";
            preamble += define.substr(eq + 1);
        }
        preamble += "\n";
    }
    return preamble;
}

bool compile_to_spirv(const Options& options, std::string_view source_text, EShLanguage stage,
    glslang::EShSource source, std::vector<uint32_t>& spirv, std::string& log)
{
    glslang::TShader shader(stage);
    std::string input_name = options.input.string();
    std::string preamble = build_preamble(options);
    const char* source_ptr = source_text.data();
    int source_len = static_cast<int>(source_text.size());
    const char* name_ptr = input_name.c_str();

    shader.setStringsWithLengthsAndNames(&source_ptr, &source_len, &name_ptr, 1);
    shader.setPreamble(preamble.c_str());
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    shader.setEnvInput(source, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);

    std::vector<std::string> processes;
    for (const auto& define : options.defines) {
        auto eq = define.find('=');
        processes.push_back("D" + (eq == std::string::npos ? define : define.substr(0, eq)));
    }
    shader.addProcesses(processes);

    DirStackFileIncluder includer;
    includer.pushExternalLocalDirectory(options.input.parent_path().empty() ? "." : options.input.parent_path().string());
    for (const auto& include_dir : options.include_dirs)
        includer.pushExternalLocalDirectory(include_dir.string());

    EShMessages messages = static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
    if (source == glslang::EShSourceHlsl)
        messages = static_cast<EShMessages>(messages | EShMsgReadHlsl);

    if (!shader.parse(GetDefaultResources(), 100, false, messages, includer)) {
        log = shader.getInfoLog();
        if (const char* debug = shader.getInfoDebugLog(); debug && *debug) {
            log += "\n";
            log += debug;
        }
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        log = program.getInfoLog();
        if (const char* debug = program.getInfoDebugLog(); debug && *debug) {
            log += "\n";
            log += debug;
        }
        return false;
    }

    const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
    if (!intermediate) {
        log = "glslang did not produce an intermediate representation";
        return false;
    }

    glslang::SpvOptions spv_options;
    spv_options.generateDebugInfo = true;
    spv_options.validate = false;
    spv_options.emitNonSemanticShaderDebugInfo = true;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &spv_options);
    log = logger.getAllMessages();
    return !spirv.empty();
}

CompileUnit compile_input(const Options& options)
{
    std::string source_text = read_text_file(options.input);
    glslang::EShSource source = source_from_name(options.input);
    std::vector<EShLanguage> candidates;

    if (auto stage = stage_from_name(options.input)) {
        candidates.push_back(*stage);
    } else {
        candidates = {EShLangVertex, EShLangFragment, EShLangCompute};
    }

    std::string combined_log;
    for (EShLanguage stage : candidates) {
        std::vector<uint32_t> spirv;
        std::string log;
        if (compile_to_spirv(options, source_text, stage, source, spirv, log))
            return CompileUnit{stage, source, std::move(spirv)};

        combined_log += "Stage ";
        combined_log += stage == EShLangVertex ? "vertex" : stage == EShLangFragment ? "fragment" : "compute";
        combined_log += " failed:\n";
        combined_log += log;
        combined_log += "\n";
    }

    throw std::runtime_error("compilation failed\n" + combined_log);
}

std::vector<uint8_t> spirv_to_bytes(const std::vector<uint32_t>& spirv)
{
    std::vector<uint8_t> bytes(spirv.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), spirv.data(), bytes.size());
    return bytes;
}

uint16_t resolve_sc_type(const spirv_cross::SPIRType& type)
{
    for (const auto& item : kVariableTypeMap) {
        if (item.base_type == type.basetype && item.vec_size == type.vecsize && item.columns == type.columns)
            return item.sc_type;
    }
    return axslc::SC_TYPE_FLOAT4;
}

uint16_t array_size(const spirv_cross::SPIRType& type)
{
    if (type.array.empty())
        return 1;

    uint32_t size = 1;
    for (uint32_t dim : type.array)
        size *= std::max(dim, 1u);
    return static_cast<uint16_t>(std::min(size, 0xffffu));
}

template <typename T>
void write_struct(yasio::fast_obstream& out, const T& value)
{
    out.write_bytes(&value, static_cast<int>(sizeof(T)));
}

void copy_name(char (&dst)[SC_NAME_LEN], const std::string& name)
{
    std::memset(dst, 0, SC_NAME_LEN);
    std::strncpy(dst, name.c_str(), SC_NAME_LEN - 1);
}

std::vector<uint8_t> build_reflection(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources,
    EShLanguage stage, const fs::path& input)
{
    yasio::fast_obstream out;

    axslc::sc_chunk_refl header{};
    copy_name(header.name, input.filename().string());
    header.debug_info = 1;
    write_struct(out, header);

    auto write_input = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler.get_type(resource.type_id);
        axslc::sc_refl_input input_refl{};
        copy_name(input_refl.name, resource.name.empty() ? compiler.get_fallback_name(resource.id) : resource.name);
        uint32_t location = compiler.get_decoration(resource.id, spv::DecorationLocation);
        std::string semantic = location < std::size(kSemanticNames) ? std::string(kSemanticNames[location]) : "ATTRIB";
        copy_name(input_refl.semantic, semantic);
        input_refl.location = static_cast<int32_t>(location);
        input_refl.semantic_index = location < std::size(kSemanticIndices) ? kSemanticIndices[location] : 0;
        input_refl.var_type = resolve_sc_type(type);
        write_struct(out, input_refl);
        ++header.num_inputs;
    };

    if (stage == EShLangVertex) {
        for (const auto& resource : resources.stage_inputs)
            write_input(resource);
    }

    auto write_ubo = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler.get_type(resource.base_type_id);
        axslc::sc_refl_uniformbuffer ubo{};
        copy_name(ubo.name, resource.name.empty() ? compiler.get_fallback_name(resource.base_type_id) : resource.name);
        ubo.binding = static_cast<int32_t>(compiler.get_decoration(resource.id, spv::DecorationBinding));
        ubo.size_bytes = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
        ubo.array_size = array_size(compiler.get_type(resource.type_id));
        ubo.num_members = static_cast<uint16_t>(std::min<size_t>(type.member_types.size(), 0xffff));
        write_struct(out, ubo);
        ++header.num_uniform_buffers;

        for (uint32_t i = 0; i < ubo.num_members; ++i) {
            auto& member_type = compiler.get_type(type.member_types[i]);
            axslc::sc_refl_uniformbuffer_member member{};
            copy_name(member.name, compiler.get_member_name(type.self, i));
            member.offset = static_cast<int32_t>(compiler.type_struct_member_offset(type, i));
            member.size_bytes = static_cast<uint32_t>(compiler.get_declared_struct_member_size(type, i));
            member.array_size = array_size(member_type);
            member.var_type = resolve_sc_type(member_type);
            write_struct(out, member);
        }
    };

    for (const auto& resource : resources.uniform_buffers)
        write_ubo(resource);

    auto write_texture = [&](const spirv_cross::Resource& resource, bool storage) {
        auto& type = compiler.get_type(resource.type_id);
        axslc::sc_refl_texture texture{};
        copy_name(texture.name, resource.name.empty() ? compiler.get_fallback_name(resource.id) : resource.name);
        texture.binding = static_cast<int32_t>(compiler.get_decoration(resource.id, spv::DecorationBinding));
        texture.image_dim = static_cast<uint8_t>(type.image.dim);
        texture.multisample = type.image.ms ? 1 : 0;
        texture.arrayed = type.image.arrayed ? 1 : 0;
        texture.count = static_cast<uint8_t>(std::min<uint16_t>(array_size(type), 255));
        texture.sampler_slot = 0;
        write_struct(out, texture);
        if (storage)
            ++header.num_storage_images;
        else
            ++header.num_textures;
    };

    for (const auto& resource : resources.sampled_images)
        write_texture(resource, false);

    if (stage == EShLangCompute) {
        for (const auto& resource : resources.storage_images)
            write_texture(resource, true);

        for (const auto& resource : resources.storage_buffers) {
            auto& type = compiler.get_type(resource.base_type_id);
            axslc::sc_refl_buffer buffer{};
            copy_name(buffer.name, resource.name.empty() ? compiler.get_fallback_name(resource.base_type_id) : resource.name);
            buffer.binding = static_cast<int32_t>(compiler.get_decoration(resource.id, spv::DecorationBinding));
            buffer.size_bytes = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
            buffer.array_stride = static_cast<uint32_t>(compiler.get_declared_struct_size_runtime_array(type, 1)
                - compiler.get_declared_struct_size_runtime_array(type, 0));
            write_struct(out, buffer);
            ++header.num_storage_buffers;
        }
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return std::vector<uint8_t>(out.data(), out.data() + out.length());
}

std::unique_ptr<spirv_cross::CompilerGLSL> make_cross_compiler(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_HLSL)
        return std::make_unique<spirv_cross::CompilerHLSL>(spirv);
    return std::make_unique<spirv_cross::CompilerGLSL>(spirv);
}

OutputBlob cross_compile(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_SPIRV)
        return OutputBlob{target, spirv_to_bytes(spirv), true};

    auto compiler = make_cross_compiler(target, spirv);
    auto options = compiler->get_common_options();
    options.flatten_multidimensional_arrays = true;

    if (target.lang == axslc::SHADER_LANG_ESSL) {
        options.es = true;
        options.version = target.profile;
    } else if (target.lang == axslc::SHADER_LANG_GLSL) {
        options.es = false;
        options.version = target.profile;
        options.enable_420pack_extension = false;
    } else if (target.lang == axslc::SHADER_LANG_HLSL) {
        auto* hlsl = static_cast<spirv_cross::CompilerHLSL*>(compiler.get());
        auto hlsl_options = hlsl->get_hlsl_options();
        hlsl_options.shader_model = target.profile;
        hlsl_options.point_size_compat = true;
        hlsl_options.point_coord_compat = true;
        hlsl_options.flatten_matrix_vertex_input_semantics = true;
        hlsl->set_hlsl_options(hlsl_options);

        if (uint32_t builtin = hlsl->remap_num_workgroups_builtin()) {
            hlsl->set_decoration(builtin, spv::DecorationDescriptorSet, 0);
            hlsl->set_decoration(builtin, spv::DecorationBinding, 0);
        }

        for (uint32_t i = 0; i < std::size(kAttribNames); ++i)
            hlsl->add_vertex_attribute_remap({i, std::string(kAttribNames[i])});
    }

    compiler->set_common_options(options);
    std::string code = compiler->compile();
    std::vector<uint8_t> bytes(code.begin(), code.end());
    bytes.push_back(0);
    return OutputBlob{target, std::move(bytes), false};
}

std::vector<uint8_t> reflection_for_target(const Target& target, const std::vector<uint32_t>& spirv, EShLanguage stage,
    const fs::path& input)
{
    auto compiler = make_cross_compiler(target.lang == axslc::SHADER_LANG_SPIRV ? Target{axslc::SHADER_LANG_GLSL, 450, "glsl-450"} : target,
        spirv);
    return build_reflection(*compiler, compiler->get_shader_resources(), stage, input);
}

fs::path output_path_for_target(const Options& options, const Target& target)
{
    fs::path out = options.output;
    if (options.sc)
        return out.extension() == ".sc" ? out : out.replace_extension(".sc");

    std::string ext;
    switch (target.lang) {
    case axslc::SHADER_LANG_HLSL:
        ext = ".hlsl" + std::to_string(target.profile);
        break;
    case axslc::SHADER_LANG_ESSL:
        ext = ".gles" + std::to_string(target.profile);
        break;
    case axslc::SHADER_LANG_GLSL:
        ext = ".glsl" + std::to_string(target.profile);
        break;
    case axslc::SHADER_LANG_SPIRV:
        ext = ".spv";
        break;
    default:
        ext = ".out";
        break;
    }
    return out.replace_extension(ext);
}

void write_sc(const Options& options, EShLanguage stage, const std::vector<OutputBlob>& outputs,
    const std::vector<std::vector<uint8_t>>& reflections)
{
    std::vector<ScTarget> targets;
    targets.reserve(outputs.size());

    for (size_t i = 0; i < outputs.size(); ++i) {
        ScTarget item;
        item.lang = sc_lang(outputs[i].target);
        item.profile = static_cast<uint32_t>(outputs[i].target.profile);
        item.stage = sc_stage(stage);
        item.code = outputs[i].data;
        item.binary = outputs[i].binary;
        if (i < reflections.size())
            item.refl = reflections[i];
        targets.push_back(std::move(item));
    }

    yasio::fast_obstream out;
    out.write<uint32_t>(SC_CHUNK);
    const auto sc_size_offset = out.length();
    out.write<uint32_t>(0);

    axslc::sc_chunk header{};
    header.major = kScMajor;
    header.minor = kScMinor;
    header.num_targets = static_cast<uint16_t>(targets.size());
    write_struct(out, header);

    const auto entries_offset = out.length();
    for (size_t i = 0; i < targets.size(); ++i) {
        axslc::sc_target_entry dummy{};
        write_struct(out, dummy);
    }

    for (auto& target : targets) {
        target.offset = static_cast<uint32_t>(out.length());

        const uint32_t code_size = static_cast<uint32_t>(target.code.size());
        const uint32_t refl_size = static_cast<uint32_t>(target.refl.size());
        const uint32_t stage_size = sizeof(uint32_t) + 8 + code_size + (refl_size ? 8 + refl_size : 0);

        out.write<uint32_t>(SC_CHUNK_STAG);
        out.write<uint32_t>(stage_size);
        out.write<uint32_t>(target.stage);
        out.write<uint32_t>(target.binary ? SC_CHUNK_DATA : SC_CHUNK_CODE);
        out.write<uint32_t>(code_size);
        out.write_bytes(target.code.data(), static_cast<int>(target.code.size()));

        if (!target.refl.empty()) {
            out.write<uint32_t>(SC_CHUNK_REFL);
            out.write<uint32_t>(refl_size);
            out.write_bytes(target.refl.data(), static_cast<int>(target.refl.size()));
        }
    }

    uint32_t sc_size = static_cast<uint32_t>(out.length());
    out.pwrite<uint32_t>(static_cast<ptrdiff_t>(sc_size_offset), sc_size);

    for (size_t i = 0; i < targets.size(); ++i) {
        axslc::sc_target_entry entry{targets[i].lang, targets[i].profile, targets[i].offset};
        std::memcpy(out.data() + entries_offset + i * sizeof(entry), &entry, sizeof(entry));
    }

    std::vector<uint8_t> bytes(out.data(), out.data() + out.length());
    write_file(output_path_for_target(options, outputs.front().target), bytes);
}

void run(const Options& options)
{
    glslang::InitializeProcess();

    CompileUnit unit;
    try {
        unit = compile_input(options);
    } catch (...) {
        glslang::FinalizeProcess();
        throw;
    }

    std::vector<OutputBlob> outputs;
    std::vector<std::vector<uint8_t>> reflections;

    for (const auto& target : options.targets) {
        outputs.push_back(cross_compile(target, unit.spirv));
        if (options.reflect)
            reflections.push_back(reflection_for_target(target, unit.spirv, unit.stage, options.input));
    }

    if (options.sc) {
        write_sc(options, unit.stage, outputs, reflections);
    } else {
        for (const auto& output : outputs)
            write_file(output_path_for_target(options, output.target), output.data);
    }

    glslang::FinalizeProcess();
}
} // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parse_args(argc, argv);
        run(options);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        print_help();
        return 1;
    }
}
