/****************************************************************************
 Copyright (c) 2026 Simdsoft Limited.

 https://axmol.dev/

 SPDX-License-Identifier: MIT
 ****************************************************************************/
#include "compiler.h"
#include "spirv_compiler.h"
#include "cross_compiler.h"
#include "reflection.h"
#include "sc_writer.h"
#include "utils.h"

#include "dxc_compiler.h"
#ifdef _WIN32
#    include "fxc_compiler.h"
#endif

#include <iostream>

namespace axslcc
{

void compile(const Options& options)
{
    auto stage = options.stage;

    std::vector<OutputBlob> outputs;
    std::vector<tlx::byte_buffer> reflections;

    auto targets = options.targets;

#ifdef _WIN32
    // Expand d3d12 into SM 5.1 (FXC/DXBC) + SM 6.0 (DXC/DXIL) when bytecode is requested.
    if (!options.keep_source_hint)
    {
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (targets[i].spec == "d3d12")
            {
                Target sm51  = targets[i];
                sm51.profile = 51;
                sm51.spec    = "d3d12-sm51";

                Target sm60  = targets[i];
                sm60.profile = 60;
                sm60.spec    = "d3d12-sm60";

                targets[i] = sm51;
                targets.insert(targets.begin() + static_cast<ptrdiff_t>(i) + 1, std::move(sm60));
                ++i;
            }
        }
    }
#endif

    for (const auto& target : targets)
    {
        OutputBlob blob;
        blob.target = &target;

        // Unified binary path: HLSL -> DXC -> SPIR-V -> SPIRV-Cross -> clean HLSL -> DXC/FXC -> bytecode
        // SPIRV-Cross strips [[vk::builtin(...)]] annotations that DXC/FXC don't recognize
        CompileUnit unit;
        try
        {
            unit = spirv::compile_input(options, target);
        }
        catch (const std::exception& e)
        {
            std::cerr << "axslcc: error compiling " << options.input.filename().string() << " for target "
                      << target.spec << ":\n  " << e.what() << std::endl;
            throw;
        }

        if (!target.isKeepSource(options.keep_source_hint))
        {  // D3D HLSL bytecode or SPIR-V binary
            if (target.lang == axslc::SHADER_LANG_HLSL)
            {
                auto hlslSource = cross::cross_compile(target, unit.spirv, options);

                if (target.profile <= 51)
                {
#ifdef _WIN32
                    auto fxcResult = fxc::compile_hlsl(hlslSource, options, target);
                    blob.data      = std::move(fxcResult.dxbc);
#else
                    throw std::runtime_error("axslcc: fxc not available on non-windows platforms");
#endif
                }
                else
                {
                    auto dxcResult = dxc::compile_source(hlslSource, options, target);
                    blob.data      = std::move(dxcResult.object);
                }

                if (options.archive)
                    reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
            }
            else if (target.lang == axslc::SHADER_LANG_SPIRV)
            {
                if (options.archive)
                    reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
                blob.data = spirv::spirv_to_bytes(spirv::make_vulkan_runtime_spirv(unit.spirv));
            }
        }
        else
        {  // Keep source text (GLSL, ESSL, MSL, or HLSL with -S)
            std::vector<UniformBlockNameOverride> uniformBlockNames;
            blob.data = cross::cross_compile(target, unit.spirv, options, &uniformBlockNames);

            if (options.archive)
                reflections.push_back(
                    reflection::build_reflection(target, unit.spirv, unit.stage, options.input, uniformBlockNames));
        }

        outputs.push_back(std::move(blob));
    }

    if (options.archive)
    {
        sc_writer::write_archive(options, stage, outputs, reflections);
    }
    else
    {
        for (const auto& output : outputs)
            utils::write_file(utils::output_path_for_target(options, *output.target), output.data);
    }
}

}  // namespace axslcc
