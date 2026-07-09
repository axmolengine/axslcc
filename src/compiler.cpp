#include "compiler.h"
#include "spirv_compiler.h"
#include "cross_compiler.h"
#include "reflection.h"
#include "sc_writer.h"
#include "utils.h"

#ifdef _WIN32
#include "dxc_compiler.h"
#endif

#include "glslang/Public/ShaderLang.h"

#include <iostream>

namespace axslcc
{

#ifdef _WIN32
bool g_inDxcCompile = false;
#endif

void Compiler::initialize()
{
    glslang::InitializeProcess();
}

void Compiler::finalize()
{
    glslang::FinalizeProcess();
}

void Compiler::compile(const Options& options)
{
    auto stage_opt = utils::stage_from_name(options.input);
    if (!stage_opt)
        throw std::runtime_error("Cannot determine shader stage: " + options.input.string());
    auto stage = *stage_opt;

    CompileUnit unit = spirv::compile_input(options);

#ifdef _WIN32
    bool hasHlslTarget = false;
    std::vector<uint8_t> dxilBytes;
    for (const auto& t : options.targets) {
        if (t.lang == axslc::SHADER_LANG_HLSL) { hasHlslTarget = true; break; }
    }

    if (hasHlslTarget && utils::is_hlsl_source(options.input))
    {
        g_inDxcCompile = true;
        try
        {
            auto dxcResult = dxc::compile_hlsl(options);
            dxilBytes = std::move(dxcResult.dxil);
        }
        catch (const std::exception& e)
        {
            std::cerr << "[dxc] " << options.input.filename().string()
                      << ": " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[dxc] " << options.input.filename().string()
                      << ": unknown error, falling back to SPIR-V" << std::endl;
        }
        g_inDxcCompile = false;
    }
#endif

    std::vector<OutputBlob> outputs;
    std::vector<std::vector<uint8_t>> reflections;

    for (const auto& target : options.targets) {
        auto blob = cross::cross_compile(target, unit.spirv, options.input);

#ifdef _WIN32
        if (target.lang == axslc::SHADER_LANG_HLSL && !dxilBytes.empty())
        {
            blob.data = dxilBytes;
            blob.binary = true;
        }
#endif

        outputs.push_back(std::move(blob));
        if (options.reflect)
            reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
    }

    if (options.sc) {
        sc_writer::write_sc(options, unit.stage, outputs, reflections);
    } else {
        for (const auto& output : outputs)
            utils::write_file(utils::output_path_for_target(options, output.target), output.data);
    }
}

} // namespace axslcc
