#include "compiler.h"
#include "spirv_compiler.h"
#include "cross_compiler.h"
#include "reflection.h"
#include "sc_writer.h"
#include "utils.h"

#ifdef _WIN32
#include "dxc_compiler.h"
#include "fxc_compiler.h"
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
        throw std::runtime_error("Cannot determine shader stage: " + options.input.string());
    auto stage = *stage_opt;

    std::vector<OutputBlob> outputs;
    std::vector<tlx::byte_buffer> reflections;

    auto targets = options.targets;

#ifdef _WIN32
    if (options.dxbc)
    {
        for (auto& t : targets)
        {
            if (t.lang == axslc::SHADER_LANG_HLSL)
                t.binary = true;
        }

        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (targets[i].spec == "d3d12")
            {
                Target sm51 = targets[i];
                sm51.profile = 51;
                sm51.spec = "d3d12-sm51";

                Target sm60 = targets[i];
                sm60.profile = 60;
                sm60.spec = "d3d12-sm60";

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
        blob.target = target;

#ifdef _WIN32
        if (target.binary)
        {
            if (target.spec == "d3d11")
            {
                // d3d11: SPIRV-Cross converts SM5.1->SM5.0 compatible HLSL, then FXC compiles to DXBC
                CompileUnit unit;
                unit = spirv::compile_input(options, target);

                auto crossBlob = cross::cross_compile(target, unit.spirv, options.input);
                std::string hlslSource(reinterpret_cast<const char*>(crossBlob.data.data()),
                                       crossBlob.data.size());

                auto all_defines = options.defines;
                all_defines.insert(all_defines.end(), target.defines.begin(), target.defines.end());

                auto fxcResult = fxc::compile_hlsl(hlslSource, stage,
                                                     options.include_dirs, all_defines,
                                                     target.profile, options.input);
                blob.data = std::move(fxcResult.dxbc);
                blob.binary = true;

                if (options.reflect)
                    reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
            }
            else
            {
                // d3d12: bypass SPIRV-Cross, compile raw HLSL directly
                auto source = utils::read_text_file(options.input);

                auto all_defines = options.defines;
                all_defines.insert(all_defines.end(), target.defines.begin(), target.defines.end());

                if (target.profile <= 51)
                {
                    auto fxcResult = fxc::compile_hlsl(source, stage,
                                                         options.include_dirs, all_defines,
                                                         target.profile, options.input);
                    blob.data = std::move(fxcResult.dxbc);
                    blob.binary = true;
                }
                else
                {
                    auto dxcResult = dxc::compile_source(source, stage,
                                                          options.include_dirs, all_defines,
                                                          target.profile, options.input);
                    blob.data = std::move(dxcResult.dxil);
                    blob.binary = true;
                }

                if (options.reflect)
                {
                    auto reflUnit = spirv::compile_input(options, target);
                    reflections.push_back(reflection::build_reflection(target, reflUnit.spirv, reflUnit.stage, options.input));
                }
            }
        }
        else
#endif
        {
            CompileUnit unit;
            try {
                unit = spirv::compile_input(options, target);
            } catch (const std::exception& e) {
                std::cerr << "axslcc: error compiling " << options.input.filename().string()
                          << " for target " << target.spec << ":\n  " << e.what() << std::endl;
                throw;
            }

            blob = cross::cross_compile(target, unit.spirv, options.input);

            if (options.reflect)
                reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
        }

        outputs.push_back(std::move(blob));
    }

    if (options.sc) {
        sc_writer::write_sc(options, stage, outputs, reflections);
    } else {
        for (const auto& output : outputs)
            utils::write_file(utils::output_path_for_target(options, output.target), output.data);
    }
}

} // namespace axslcc
