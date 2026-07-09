#include "spirv_compiler.h"
#include "utils.h"

#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/DirStackFileIncluder.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace axslcc::spirv
{

namespace
{

constexpr std::string_view kAttribNames[] = {
    "POSITION", "NORMAL", "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3",
    "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "COLOR0", "COLOR1",
    "COLOR2", "COLOR3", "TANGENT", "BINORMAL", "BLENDINDICES", "BLENDWEIGHT",
};

std::string build_preamble(const Options& options, glslang::EShSource source)
{
    std::string preamble;
    if (source != glslang::EShSourceHlsl) {
        preamble += "#extension GL_GOOGLE_include_directive : require\n";
        for (size_t i = 0; i < std::size(kAttribNames); ++i) {
            preamble += "#define ";
            preamble += kAttribNames[i];
            preamble += " ";
            preamble += std::to_string(i);
            preamble += "\n";
        }
        for (int i = 0; i < 8; ++i)
            preamble += "#define SV_Target" + std::to_string(i) + " " + std::to_string(i) + "\n";
    }

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
    std::string preamble = build_preamble(options, source);
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

} // namespace

CompileUnit compile_input(const Options& options)
{
    if (auto stage_opt = utils::stage_from_name(options.input); !stage_opt) {
        throw std::runtime_error("cannot determine shader stage from filename '" +
                                 options.input.string() + "' (expected _vs/_ps/_cs suffix or .vert/.frag/.comp extension)");
    }

    std::string source_text = utils::read_text_file(options.input);
    glslang::EShSource source = utils::is_hlsl_source(options.input) ? glslang::EShSourceHlsl : glslang::EShSourceGlsl;
    std::vector<EShLanguage> candidates;

    if (auto stage = utils::stage_from_name(options.input)) {
        switch (*stage) {
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
    } else {
        candidates = {EShLangVertex, EShLangFragment, EShLangCompute};
    }

    std::string combined_log;
    for (EShLanguage stage : candidates) {
        std::vector<uint32_t> spirv;
        std::string log;
        if (compile_to_spirv(options, source_text, stage, source, spirv, log)) {
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

        combined_log += "Stage ";
        combined_log += stage == EShLangVertex ? "vertex" : stage == EShLangFragment ? "fragment" : "compute";
        combined_log += " failed:\n";
        combined_log += log;
        combined_log += "\n";
    }

    throw std::runtime_error("compilation failed\n" + combined_log);
}

tlx::byte_buffer spirv_to_bytes(const std::vector<uint32_t>& spirv)
{
    tlx::byte_buffer bytes(spirv.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), spirv.data(), bytes.size());
    return bytes;
}

} // namespace axslcc::spirv
