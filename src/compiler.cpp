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
        throw std::runtime_error("Cannot determine shader stage from filename: " + options.input.string());
    auto stage = *stage_opt;

    // Always compile to SPIR-V first (needed for non-HLSL targets and for REFL data)
    CompileUnit spirvUnit = spirv::compile_input(options);

#ifdef _WIN32
    // On Windows, use DXC to compile HLSL → DXIL for HLSL targets
    // This avoids the SPIR-V roundtrip for D3D12 bytecode (semantic preservation)
    bool hasHlslTarget = false;
    std::vector<uint8_t> dxilBytes;
    for (const auto& t : options.targets) {
        if (t.lang == axslc::SHADER_LANG_HLSL) {
            hasHlslTarget = true;
            break;
        }
    }

    if (hasHlslTarget && utils::is_hlsl_source(options.input))
    {
        try
        {
            auto dxcResult = dxc::compile_hlsl(options);
            dxilBytes = std::move(dxcResult.dxil);
        }
        catch (const std::exception& e)
        {
            std::cerr << "[dxc] WARNING: DXC compilation failed ("
                      << options.input.string() << "): " << e.what()
                      << " - falling back to SPIR-V HLSL output" << std::endl;
        }
    }
#endif

    // Build outputs and reflections (all from SPIR-V path; D3D12 targets optionally use DXC DXIL)
    std::vector<OutputBlob> outputs;
    std::vector<std::vector<uint8_t>> reflections;

    for (const auto& target : options.targets)
    {
        OutputBlob blob = cross::cross_compile(target, spirvUnit.spirv);

#ifdef _WIN32
        // Replace HLSL output with DXC-compiled DXIL (if available)
        if (target.lang == axslc::SHADER_LANG_HLSL && !dxilBytes.empty())
        {
            blob.data = dxilBytes;
            blob.binary = true;
        }
#endif

        outputs.push_back(std::move(blob));

        if (options.reflect)
            reflections.push_back(reflection::build_reflection(target, spirvUnit.spirv, spirvUnit.stage, options.input));
    }

    if (options.sc) {
        sc_writer::write_sc(options, spirvUnit.stage, outputs, reflections);
    } else {
        for (const auto& output : outputs)
            utils::write_file(utils::output_path_for_target(options, output.target), output.data);
    }
}

} // namespace axslcc
