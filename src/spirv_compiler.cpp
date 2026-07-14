#include "spirv_compiler.h"
#include "dxc_compiler.h"
#include "utils.h"

#include "axslc-spec.h"

#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/DirStackFileIncluder.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv-tools/optimizer.hpp"
#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace axslcc::spirv
{

namespace
{

using namespace std::string_view_literals;

struct GlslLocationDefine
{
    std::string_view name;
    uint32_t location;
};

// For internal migrate verification purpose only
inline constexpr GlslLocationDefine kLegacyGlslVertexDefines[16] = {
    {"POSITION"sv, 0},
    {"NORMAL"sv, 1},
    {"TEXCOORD0"sv, 2},
    {"TEXCOORD1"sv, 3},
    {"TEXCOORD2"sv, 4},
    {"TEXCOORD3"sv, 5},
    {"TEXCOORD4"sv, 6},
    {"TEXCOORD5"sv, 7},
    {"TEXCOORD6"sv, 8},
    {"TEXCOORD7"sv, 9},
    {"COLOR0"sv, 10},
    {"COLOR1"sv, 11},
    {"TANGENT"sv, 12},
    {"BINORMAL"sv, 13},
    {"BLENDINDICES"sv, 14},
    {"BLENDWEIGHT"sv, 15},
};

std::string build_preamble(std::span<const std::string_view> defines,
                           glslang::EShSource source)
{
    std::string preamble;
    if (source != glslang::EShSourceHlsl)
    {
        preamble += "#extension GL_GOOGLE_include_directive : require\n";

        for (const auto& semantic : kLegacyGlslVertexDefines)
            preamble += fmt::format("#define {} {}\n", semantic.name, semantic.location);

        for (int i = 0; i < 8; ++i)
            preamble += fmt::format("#define SV_Target{} {}\n", i, i);
    }

    for (const auto& define : defines) {
        auto eq = define.find('=');
        preamble += "#define ";
        preamble += eq == std::string_view::npos ? define : define.substr(0, eq);
        if (eq != std::string_view::npos) {
            preamble += " ";
            preamble += define.substr(eq + 1);
        }
        preamble += "\n";
    }
    return preamble;
}

std::string strip_comments(std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/')
        {
            i += 2;
            while (i < source.size() && source[i] != '\n')
                ++i;
            if (i < source.size())
                result.push_back(source[i++]);
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
            {
                if (source[i] == '\n')
                    result.push_back('\n');
                ++i;
            }
            if (i + 1 < source.size())
                i += 2;
            continue;
        }
        result.push_back(source[i++]);
    }
    return result;
}

void validate_no_explicit_resource_registers(std::string_view source)
{
    static const std::regex kResourceRegisterPattern(
        R"(:\s*register\s*\(\s*[btsu]\d*)",
        std::regex_constants::icase | std::regex_constants::optimize);

    if (std::regex_search(strip_comments(source), kResourceRegisterPattern))
        throw std::runtime_error(
            "Explicit resource register assignments are not supported. "
            "Remove ': register(...)'; resource bindings are assigned automatically by axslcc.");
}

bool is_base_include_line(std::string_view line)
{
    static const std::regex kBaseIncludePattern(
        R"(^\s*#\s*include\s*[<"]base\.hlsli[>"]\s*(?://.*)?$)",
        std::regex_constants::icase | std::regex_constants::optimize);
    return std::regex_match(line.begin(), line.end(), kBaseIncludePattern);
}

fs::path find_base_hlsli(const Options& options)
{
    std::vector<fs::path> search_dirs;
    if (!options.input.empty() && !options.input.parent_path().empty())
        search_dirs.push_back(options.input.parent_path());
    search_dirs.insert(search_dirs.end(), options.include_dirs.begin(), options.include_dirs.end());

    for (const auto& dir : search_dirs)
    {
        auto candidate = dir / "base.hlsli";
        if (fs::exists(candidate))
            return candidate;
    }

    throw std::runtime_error("failed to locate base.hlsli for Axmol built-in sampler layout");
}

std::string inline_base_hlsli(std::string source, const Options& options)
{
    std::string result;
    result.reserve(source.size());

    bool inlined = false;
    size_t line_start = 0;
    while (line_start < source.size())
    {
        const size_t line_end = source.find('\n', line_start);
        const size_t line_size = (line_end == std::string::npos ? source.size() : line_end) - line_start;
        const std::string_view line(source.data() + line_start, line_size);

        if (is_base_include_line(line))
        {
            if (!inlined)
            {
                auto base = utils::read_text_file(find_base_hlsli(options));
                result += "\n// axslcc inlined base.hlsli for resource layout\n";
                result += base;
                if (!result.empty() && result.back() != '\n')
                    result.push_back('\n');
                inlined = true;
            }
        }
        else
        {
            result.append(line);
            if (line_end != std::string::npos)
                result.push_back('\n');
        }

        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
    }

    return result;
}

uint32_t resource_array_count(std::string_view arraySuffix, std::string_view name)
{
    if (arraySuffix.empty())
        return 1;

    std::string_view value = arraySuffix;
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']')
        value = value.substr(1, value.size() - 2);

    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        throw std::runtime_error("Resource array '" + std::string(name) + "' must use a numeric literal size.");

    value = value.substr(first, last - first + 1);
    if (!std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        throw std::runtime_error(
            "Resource array '" + std::string(name) +
            "' must use a numeric literal size because axslcc assigns resource bindings before compilation.");

    uint64_t count = 0;
    for (char ch : value)
        count = count * 10 + static_cast<uint64_t>(ch - '0');

    if (count == 0 || count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Resource array '" + std::string(name) + "' has an invalid size.");

    return static_cast<uint32_t>(count);
}

uint32_t sampler_space_for_target(const Options& options, const Target& target)
{
    (void)options;
    (void)target;
    return 1u;
}

int32_t find_builtin_sampler_preset(std::string_view name)
{
    static constexpr std::string_view kPresetNames[] = {
        "LinearClamp", "LinearWrap", "LinearMirror", "LinearBorder",
        "PointClamp", "PointWrap", "PointMirror", "PointBorder",
        "LinearMipClamp", "LinearMipWrap", "LinearMipMirror", "LinearMipBorder",
        "AnisoClamp", "AnisoWrap", "AnisoMirror", "AnisoBorder",
        "ShadowCmpClamp", "ShadowCmpWrap", "ShadowCmpMirror", "ShadowCmpBorder",
        "LinearNoMipClamp", "PointNoMipClamp",
    };

    for (size_t i = 0; i < std::size(kPresetNames); ++i)
    {
        if (kPresetNames[i] == name)
            return static_cast<int32_t>(i);
    }

    return -1;
}

std::string inject_hlsl_resource_layout(std::string source, const Options& options, const Target& target)
{
    struct Allocation
    {
        uint32_t cbv{0};
        uint32_t srv{0};
        uint32_t uav{0};
    } alloc;

    if (options.stage == ShaderStage::Fragment)
        alloc.cbv = 1;
    const uint32_t samplerSpace = sampler_space_for_target(options, target);

    auto apply_regex = [](std::string text, const std::regex& pattern, auto&& replacer) {
        std::string result;
        size_t last = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
        {
            const auto& match = *it;
            result.append(text, last, static_cast<size_t>(match.position()) - last);
            result += replacer(match);
            last = static_cast<size_t>(match.position() + match.length());
        }
        result.append(text, last, std::string::npos);
        return result;
    };

    static const std::regex kCBufferPattern(
        R"(\b(cbuffer)\s+([A-Za-z_]\w*)\s*(\{))",
        std::regex_constants::optimize);
    source = apply_regex(std::move(source), kCBufferPattern, [&](const std::smatch& match) {
        const auto binding = alloc.cbv++;
        return match.str(1) + " " + match.str(2) + " : register(b" + std::to_string(binding) + ", space0) " +
               match.str(3);
    });

    static const std::regex kResourcePattern(
        R"(\b((?:RW)?Texture(?:1D|2D|3D|Cube)(?:Array)?(?:\s*<[^;{}>]+>)?|(?:RW)?(?:StructuredBuffer|ByteAddressBuffer)(?:\s*<[^;{}>]+>)?|(?:RW)?Buffer(?:\s*<[^;{}>]+>)?|Sampler(?:Comparison)?State)\s+([A-Za-z_]\w*)\s*(\[[^\]]+\])?\s*;)",
        std::regex_constants::optimize);

    source = apply_regex(std::move(source), kResourcePattern, [&](const std::smatch& match) {
        const std::string type = match.str(1);
        const std::string name = match.str(2);
        const std::string arraySuffix = match.str(3);
        const uint32_t count = resource_array_count(arraySuffix, name);

        char regClass = 't';
        uint32_t binding = 0;
        uint32_t space = 1;

        if (type.find("Sampler") != std::string::npos)
        {
            regClass = 's';
            space = samplerSpace;

            auto presetIdx = find_builtin_sampler_preset(name);
            if (presetIdx < 0)
                throw std::runtime_error(
                    "Custom sampler '" + name + "' is not supported yet.\n"
                    "Use a built-in Axmol sampler name.");
            binding = static_cast<uint32_t>(presetIdx);
        }
        else if (type.rfind("RW", 0) == 0)
        {
            regClass = 'u';
            binding = alloc.uav;
            alloc.uav += count;
        }
        else
        {
            regClass = 't';
            binding = alloc.srv;
            alloc.srv += count;
        }

        return type + " " + name + arraySuffix + " : register(" + regClass + std::to_string(binding) + ", space" +
               std::to_string(space) + ");";
    });

    return source;
}

bool compile_to_spirv(const Options& options, std::span<const std::string_view> defines,
    std::string_view source_text, EShLanguage stage,
    glslang::EShSource source, std::vector<uint32_t>& spirv, std::string& log)
{
    glslang::TShader shader(stage);
    std::string input_name = options.input.string();
    std::string preamble = build_preamble(defines, source);
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
        for (const auto& define : defines) {
        auto eq = define.find('=');
        processes.push_back(fmt::format("D{}", eq == std::string_view::npos ? define : define.substr(0, eq)));
    }
    shader.addProcesses(processes);

    DirStackFileIncluder includer;
    includer.pushExternalDirectory(options.input.parent_path().empty() ? "." : options.input.parent_path().string());
    for (const auto& include_dir : options.include_dirs)
        includer.pushExternalDirectory(include_dir.string());

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
    spv_options.validate = false;
    switch (options.opt_level) {
    case 0:
        spv_options.generateDebugInfo = true;
        spv_options.disableOptimizer = true;
        spv_options.emitNonSemanticShaderDebugInfo = true;
        break;
    case 1:
        spv_options.disableOptimizer = false;
        spv_options.optimizeSize = true;
        break;
    case 2:
        spv_options.disableOptimizer = false;
        break;
    case 3:
        spv_options.disableOptimizer = false;
        spv_options.validate = true;
        break;
    }
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &spv_options);
    log = logger.getAllMessages();
    return !spirv.empty();
}

} // namespace

std::vector<uint32_t> make_vulkan_runtime_spirv(const std::vector<uint32_t>& spirv)
{
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    optimizer.RegisterPass(spvtools::CreateStripNonSemanticInfoPass());

    std::string diagnostics;
    auto consume_message = [&diagnostics](spv_message_level_t, const char*, const spv_position_t& position,
                                          const char* message) {
        diagnostics += fmt::format("{}: {}\n", position.index, message ? message : "unknown SPIR-V error");
    };
    optimizer.SetMessageConsumer(consume_message);

    std::vector<uint32_t> runtime_spirv;
    if (!optimizer.Run(spirv.data(), spirv.size(), &runtime_spirv))
        throw std::runtime_error(fmt::format("failed to strip non-runtime SPIR-V metadata\n{}", diagnostics));

    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    validator.SetMessageConsumer(consume_message);
    if (!validator.Validate(runtime_spirv))
        throw std::runtime_error(fmt::format("Vulkan runtime SPIR-V validation failed\n{}", diagnostics));

    return runtime_spirv;
}

CompileUnit compile_input(const Options& options, const Target& target)
{
    std::string source_text = utils::read_text_file(options.input);
    glslang::EShSource source = (options.input_lang == InputLang::HLSL) ? glslang::EShSourceHlsl : glslang::EShSourceGlsl;

    if (options.input_lang == InputLang::HLSL)
    {
        source_text = inline_base_hlsli(std::move(source_text), options);
        validate_no_explicit_resource_registers(source_text);
        source_text = inject_hlsl_resource_layout(std::move(source_text), options, target);
    }

    std::vector<EShLanguage> candidates;
    switch (options.stage) {
    case ShaderStage::Vertex:
        candidates.push_back(EShLangVertex);
        break;
    case ShaderStage::Fragment:
        candidates.push_back(EShLangFragment);
        break;
    case ShaderStage::Compute:
        candidates.push_back(EShLangCompute);
        break;
    }

    if (options.input_lang == InputLang::HLSL && options.hlsl_frontend == HlslFrontend::DXC)
    {
        auto result = dxc::compile_spirv(source_text, options, target);
        if (result.object.size() % sizeof(uint32_t) != 0)
            throw std::runtime_error("DXC produced a malformed SPIR-V object");

        CompileUnit unit;
        unit.stage = options.stage;
        unit.is_hlsl = true;
        unit.spirv.resize(result.object.size() / sizeof(uint32_t));
        std::memcpy(unit.spirv.data(), result.object.data(), result.object.size());

        // DXC preserves all globals from base.hlsli as entry-point interface
        // variables. Remove unused sampler presets before reflection/layout
        // generation; otherwise a shader using one sampler would expose all preset samplers.
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
        optimizer.RegisterPassFromFlag("--eliminate-dead-variables", false);
        std::vector<uint32_t> optimized;
        if (!optimizer.Run(unit.spirv.data(), unit.spirv.size(), &optimized))
            throw std::runtime_error("failed to eliminate unused DXC SPIR-V resources");
        unit.spirv = std::move(optimized);
        return unit;
    }

    std::string combined_log;
    for (EShLanguage stage : candidates) {
        std::vector<uint32_t> spirv;
        std::string log;
        if (compile_to_spirv(options, target.defines, source_text, stage, source, spirv, log)) {
            CompileUnit unit;
            switch (stage) {
            case EShLangVertex:
                unit.stage = ShaderStage::Vertex;
                break;
            case EShLangFragment:
                unit.stage = ShaderStage::Fragment;
                break;
            case EShLangCompute:
                unit.stage = ShaderStage::Compute;
                break;
            default:
                throw std::runtime_error("unsupported shader stage");
            }
            unit.is_hlsl = (source == glslang::EShSourceHlsl);
            unit.spirv = std::move(spirv);
            return unit;
        }

        combined_log += fmt::format("Stage {} failed:\n{}\n",
            stage == EShLangVertex ? "vertex" : stage == EShLangFragment ? "fragment" : "compute", log);
    }

    throw std::runtime_error(fmt::format("compilation failed\n{}", combined_log));
}

bool compile_glsl_to_spirv(std::string_view source_text, ShaderStage stage,
                           std::vector<uint32_t>& spirv, int opt_level,
                           std::string& log)
{
    EShLanguage eshStage;
    switch (stage) {
    case ShaderStage::Vertex:
        eshStage = EShLangVertex;
        break;
    case ShaderStage::Fragment:
        eshStage = EShLangFragment;
        break;
    case ShaderStage::Compute:
        eshStage = EShLangCompute;
        break;
    default:
        eshStage = EShLangFragment;
        break;
    }

    glslang::TShader shader(eshStage);
    const char* source_ptr = source_text.data();
    int source_len = static_cast<int>(source_text.size());
    const char* name_ptr = "glsl_src";

    shader.setStringsWithLengthsAndNames(&source_ptr, &source_len, &name_ptr, 1);
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceGlsl, eshStage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);

    EShMessages messages = static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
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

    const glslang::TIntermediate* intermediate = program.getIntermediate(eshStage);
    if (!intermediate) {
        log = "glslang did not produce an intermediate representation from GLSL";
        return false;
    }

    glslang::SpvOptions spv_options;
    spv_options.validate = false;
    switch (opt_level) {
    case 0:
        spv_options.generateDebugInfo = true;
        spv_options.disableOptimizer = true;
        spv_options.emitNonSemanticShaderDebugInfo = true;
        break;
    case 1:
        spv_options.disableOptimizer = false;
        spv_options.optimizeSize = true;
        break;
    case 2:
        spv_options.disableOptimizer = false;
        break;
    case 3:
        spv_options.disableOptimizer = false;
        spv_options.validate = true;
        break;
    }
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &spv_options);
    log = logger.getAllMessages();
    return !spirv.empty();
}

std::string spirv_to_bytes(const std::vector<uint32_t>& spirv)
{
    std::string bytes;
    bytes.reserve(spirv.size() * sizeof(uint32_t));
    bytes.append(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
    return bytes;
}

} // namespace axslcc::spirv
