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

#include "glslang/Public/ShaderLang.h"

#include "spirv_glsl.hpp"

#include <fmt/format.h>
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
        if (options.input_lang == InputLang::HLSL && options.hlsl_frontend == HlslFrontend::Glslang &&
            target.lang == axslc::SHADER_LANG_SPIRV && options.vulkan_sampler_mode == VulkanSamplerMode::Separate)
        {
            throw std::runtime_error(
                "--hlsl-frontend glslang cannot emit Vulkan separate samplers; use DXC or --vulkan-samplers combined");
        }

        OutputBlob blob;
        blob.target = target;

        // Unified binary path: HLSL -> glslang -> SPIR-V -> SPIRV-Cross -> clean HLSL -> DXC/FXC -> binary
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
                auto crossBlob = cross::cross_compile(target, unit.spirv, options.input);
                std::string hlslSource(reinterpret_cast<const char*>(crossBlob.data.data()), crossBlob.data.size());

                auto all_defines = options.defines;
                all_defines.insert(all_defines.end(), target.defines.begin(), target.defines.end());

                if (target.profile <= 51)
                {
                    auto fxcResult = fxc::compile_hlsl(hlslSource, stage, options.include_dirs, all_defines,
                                                       target.profile, options.opt_level, options.input);
                    blob.data      = std::move(fxcResult.dxbc);
                }
                else
                {
                    auto dxcResult = dxc::compile_source(hlslSource, stage, options.include_dirs, all_defines,
                                                         target.profile, options.opt_level, options.input);
                    blob.data      = std::move(dxcResult.object);
                }

                if (options.archive)
                    reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
            }
            else if (target.lang == axslc::SHADER_LANG_SPIRV)
            {
                if (options.vulkan_sampler_mode == VulkanSamplerMode::Separate)
                {
                    if (options.archive)
                        reflections.push_back(
                            reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
                    auto runtimeSpirv = spirv::make_vulkan_runtime_spirv(unit.spirv);
                    blob.data         = spirv::spirv_to_bytes(runtimeSpirv);
                    outputs.push_back(std::move(blob));
                    continue;
                }

                // Round-trip SPIR-V through GLSL to combine SamplerState declarations
                // from base.hlsli into combined image samplers. The raw glslang SPIR-V
                // has separate OpTypeSampler descriptors (s0..s21) that conflict with
                // OpTypeImage descriptors (t0..tN) at the same DescriptorSet/Binding.
                //
                // Steps:
                // 1. Use SPIRV-Cross to get combined sampler binding info from images
                // 2. Cross-compile to Vulkan GLSL (which merges separate images+samplers)
                // 3. Post-process GLSL text to add layout(binding=N, set=M) qualifiers
                //    (SPIRV-Cross doesn't emit them for combined sampler2D)
                // 4. Re-compile GLSL to SPIR-V via glslang

                // Step 1: Extract binding info from original SPIR-V images
                auto reflCompiler = std::make_unique<spirv_cross::CompilerGLSL>(unit.spirv);
                auto preRes       = reflCompiler->get_shader_resources();
                std::unordered_map<spirv_cross::VariableID, std::string> imageNameMap;
                for (const auto& img : preRes.separate_images)
                    imageNameMap[img.id] = reflCompiler->get_name(img.id);
                reflCompiler->build_combined_image_samplers();
                // name -> {set, binding}
                std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> bindInfo;
                for (const auto& c : reflCompiler->get_combined_image_samplers())
                {
                    auto it = imageNameMap.find(c.image_id);
                    if (it == imageNameMap.end())
                        continue;
                    uint32_t binding     = reflCompiler->has_decoration(c.image_id, spv::DecorationBinding)
                                               ? reflCompiler->get_decoration(c.image_id, spv::DecorationBinding)
                                               : 0;
                    uint32_t descSet     = reflCompiler->has_decoration(c.image_id, spv::DecorationDescriptorSet)
                                               ? reflCompiler->get_decoration(c.image_id, spv::DecorationDescriptorSet)
                                               : 0;
                    bindInfo[it->second] = {descSet, binding};
                }

                // Step 2: Cross-compile to Vulkan GLSL
                Target glslTarget{axslc::SHADER_LANG_GLSL, 450, "glsl-450"};
                auto glslOutput = cross::cross_compile(glslTarget, unit.spirv, options.input);
                auto glslSize   = glslOutput.data.size();
                while (glslSize > 0 && glslOutput.data[glslSize - 1] == 0)
                    --glslSize;
                std::string glslSource(reinterpret_cast<const char*>(glslOutput.data.data()), glslSize);

                // Step 3: Post-process GLSL - prepend layout(binding=, set=) to combined sampler2D
                for (const auto& [name, info] : bindInfo)
                {
                    auto [descSet, binding] = info;
                    // Find the declaration "uniform <type> NAME;" or "uniform <type> NAME[N];"
                    // by searching for NAME followed by ; or [, then looking backwards for "uniform "
                    size_t namePos = glslSource.find(name + ";");
                    if (namePos == std::string::npos)
                        namePos = glslSource.find(name + "[");
                    if (namePos != std::string::npos)
                    {
                        size_t uniformPos = glslSource.rfind("uniform ", namePos);
                        if (uniformPos != std::string::npos && (namePos - uniformPos) < 80)
                        {
                            glslSource.insert(uniformPos, fmt::format("layout(set = {}, binding = {}) ", descSet, binding));
                        }
                    }
                }

                // Step 4: Re-compile GLSL to SPIR-V
                std::vector<uint32_t> combinedSpirv;
                std::string log;
                if (!spirv::compile_glsl_to_spirv(glslSource, unit.stage, combinedSpirv, options.opt_level, log))
                {
                    std::cerr << "axslcc: error re-compiling GLSL to SPIR-V for " << options.input.filename().string()
                              << ":\n  " << log << std::endl;
                    throw std::runtime_error("SPIR-V round-trip compilation failed");
                }

                if (options.archive)
                    reflections.push_back(
                        reflection::build_reflection(target, combinedSpirv, unit.stage, options.input));

                auto runtimeSpirv = spirv::make_vulkan_runtime_spirv(combinedSpirv);
                blob.data         = spirv::spirv_to_bytes(runtimeSpirv);
                blob.target       = target;
            }
        }
        else
        {  // Keep source text (GLSL, ESSL, MSL, or HLSL with -S)
            std::vector<UniformBlockNameOverride> uniformBlockNames;
            blob = cross::cross_compile(target, unit.spirv, options.input, &uniformBlockNames);

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
            utils::write_file(utils::output_path_for_target(options, output.target), output.data);
    }
}

}  // namespace axslcc
