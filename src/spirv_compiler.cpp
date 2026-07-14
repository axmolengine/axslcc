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
#include <span>
#include <stdexcept>
#include <string>
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
        // generation; otherwise a shader using one sampler would expose all 22.
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
    bytes.resize_and_overwrite(spirv.size() * sizeof(uint32_t), [&](char* buf, size_t n) {
        std::memcpy(buf, spirv.data(), n);
        return n;
    });
    return bytes;
}

} // namespace axslcc::spirv
