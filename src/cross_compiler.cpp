/****************************************************************************
 Copyright (c) 2019-present Axmol Engine contributors (see AUTHORS.md).

 https://axmol.dev/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#include "cross_compiler.h"
#include "spirv_compiler.h"
#include "utils.h"

#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace axslcc::cross
{

namespace
{

constexpr uint32_t kD3D11SamplerSlotCount = 16;

bool is_d3d11_target(const Target& target)
{
    return target.lang == axslc::SHADER_LANG_HLSL && target.profile == 50;
}

uint32_t get_decoration_or_zero(spirv_cross::CompilerGLSL& compiler, spirv_cross::ID id, spv::Decoration decoration)
{
    return compiler.has_decoration(id, decoration) ? compiler.get_decoration(id, decoration) : 0;
}

uint32_t sampler_array_count(spirv_cross::CompilerGLSL& compiler, const spirv_cross::Resource& sampler)
{
    auto& type = compiler.get_type(sampler.type_id);
    return type.array.empty() ? 1 : type.array.front();
}

void compact_d3d11_sampler_bindings(spirv_cross::CompilerGLSL& compiler)
{
    auto resources = compiler.get_shader_resources();
    std::vector<spirv_cross::Resource> samplers(resources.separate_samplers.begin(),
                                                resources.separate_samplers.end());

    std::sort(samplers.begin(), samplers.end(), [&](const auto& a, const auto& b) {
        const auto a_set = get_decoration_or_zero(compiler, a.id, spv::DecorationDescriptorSet);
        const auto b_set = get_decoration_or_zero(compiler, b.id, spv::DecorationDescriptorSet);
        if (a_set != b_set)
            return a_set < b_set;

        const auto a_binding = get_decoration_or_zero(compiler, a.id, spv::DecorationBinding);
        const auto b_binding = get_decoration_or_zero(compiler, b.id, spv::DecorationBinding);
        if (a_binding != b_binding)
            return a_binding < b_binding;

        return a.name < b.name;
    });

    uint32_t slot = 0;
    for (const auto& sampler : samplers)
    {
        const auto count = sampler_array_count(compiler, sampler);
        if (slot + count > kD3D11SamplerSlotCount)
            throw std::runtime_error(fmt::format("D3D11 supports at most {} active samplers per shader stage",
                                                 kD3D11SamplerSlotCount));

        compiler.set_decoration(sampler.id, spv::DecorationBinding, slot);
        slot += count;
    }
}

std::unique_ptr<spirv_cross::CompilerGLSL> make_cross_compiler(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_HLSL)
        return std::make_unique<spirv_cross::CompilerHLSL>(spirv);
    if (target.lang == axslc::SHADER_LANG_MSL)
        return std::make_unique<spirv_cross::CompilerMSL>(spirv);
    return std::make_unique<spirv_cross::CompilerGLSL>(spirv);
}

} // namespace

std::string cross_compile(const Target& target, const std::vector<uint32_t>& spirv,
                          const Options& options,
                          std::vector<UniformBlockNameOverride>* uniform_block_names)
{
    auto compiler = make_cross_compiler(target, spirv);
    auto spv_options = compiler->get_common_options();
    spv_options.flatten_multidimensional_arrays = true;

    if (target.lang == axslc::SHADER_LANG_ESSL || target.lang == axslc::SHADER_LANG_GLSL) {
        // Save original separate image names before building combined samplers
        auto pre_resources = compiler->get_shader_resources();
        std::unordered_map<uint32_t, std::string> image_names;
        for (const auto& img : pre_resources.separate_images)
            image_names[img.id] = compiler->get_name(img.id);

        compiler->build_combined_image_samplers();

        // Restore original image names for combined samplers (SPIRV-Cross
        // auto-generates names like _255, but axmol C++ looks up e.g. "u_tex0")
        for (const auto& combined : compiler->get_combined_image_samplers()) {
            auto it = image_names.find(combined.image_id);
            if (it != image_names.end() && !it->second.empty())
                compiler->set_name(combined.combined_id, it->second);
        }

        // GLSL 3.3/ESSL 3.0 link stage interfaces by name. Use DXC's exact
        // UserSemantic decoration as that name; no generated naming convention
        // becomes part of the shader ABI. Keep vertex attributes on their raw
        // compiler-local names so COLOR0 input/output cannot collide.
        if (utils::is_hlsl_source(options.input))
        {
            auto post_resources = compiler->get_shader_resources();
            const auto model = compiler->get_execution_model();
            const auto apply_semantic_name = [&](const spirv_cross::Resource& resource) {
                auto semantic = compiler->get_decoration_string(
                    resource.id, spv::DecorationHlslSemanticGOOGLE);
                if (!semantic.empty())
                    compiler->set_name(resource.id, semantic);
            };

            if (model == spv::ExecutionModelVertex)
            {
                for (const auto& r : post_resources.stage_inputs)
                    compiler->unset_decoration(r.id, spv::DecorationHlslSemanticGOOGLE);
                for (const auto& r : post_resources.stage_outputs)
                    apply_semantic_name(r);
            }
            else if (model == spv::ExecutionModelFragment)
            {
                for (const auto& r : post_resources.stage_inputs)
                    apply_semantic_name(r);
            }
        }
    }

    if (target.lang == axslc::SHADER_LANG_ESSL) {
        spv_options.es = true;
        spv_options.version = target.profile;
    } else if (target.lang == axslc::SHADER_LANG_GLSL) {
        spv_options.es = false;
        spv_options.version = target.profile;
        spv_options.enable_420pack_extension = false;
    } else if (target.lang == axslc::SHADER_LANG_HLSL) {
        auto* hlsl = static_cast<spirv_cross::CompilerHLSL*>(compiler.get());
        auto hlsl_options = hlsl->get_hlsl_options();
        hlsl_options.shader_model = target.profile;
        hlsl_options.point_size_compat = true;
        hlsl_options.point_coord_compat = true;
        hlsl_options.flatten_matrix_vertex_input_semantics = true;
        hlsl_options.user_semantic = false;
        hlsl->set_hlsl_options(hlsl_options);

        if (uint32_t builtin = hlsl->remap_num_workgroups_builtin()) {
            hlsl->set_decoration(builtin, spv::DecorationDescriptorSet, 0);
            hlsl->set_decoration(builtin, spv::DecorationBinding, 0);
        }

        if (is_d3d11_target(target))
            compact_d3d11_sampler_bindings(*compiler);

        auto resources = compiler->get_shader_resources();
        const auto model = compiler->get_execution_model();

        const auto require_semantic = [&](std::string_view semantic, std::string_view input_name) {
            if (semantic.empty())
            {
                throw std::runtime_error(fmt::format(
                    "missing HLSL semantic for vertex input '{}' when emitting HLSL target",
                    input_name));
            }

            return utils::parse_semantic(semantic);
        };

        if (model == spv::ExecutionModelVertex) {
            for (const auto& r : resources.stage_inputs) {
                auto& type = compiler->get_type(r.type_id);
                uint32_t base_loc = compiler->get_decoration(r.id, spv::DecorationLocation);
                auto sem = compiler->get_decoration_string(r.id, spv::DecorationHlslSemanticGOOGLE);
                auto input_name = compiler->get_name(r.id);
                if (input_name.empty())
                    input_name = compiler->get_fallback_name(r.id);

                if (type.columns > 1 && type.columns <= 4) {
                    auto [name, idx] = require_semantic(sem, input_name);
                    for (uint32_t col = 0; col < type.columns; ++col)
                        hlsl->add_vertex_attribute_remap({
                            base_loc + col,
                            fmt::format("{}{}", name, idx + col)
                        });
                } else if (type.member_types.size() > 0) {
                    bool all_same_float4 = true;
                    bool any_extra_sem = false;
                    std::string base_name;
                    uint16_t base_idx = 0;

                    for (uint32_t mi = 0; mi < type.member_types.size(); ++mi) {
                        auto& mtype = compiler->get_type(type.member_types[mi]);
                        if (mtype.vecsize != 4 || mtype.columns != 1) {
                            all_same_float4 = false;
                            break;
                        }

                        auto msem = compiler->get_member_decoration_string(
                            type.self, mi, spv::DecorationHlslSemanticGOOGLE);
                        if (mi > 0 && !msem.empty())
                            any_extra_sem = true;
                        if (mi == 0)
                        {
                            auto member_name = compiler->get_member_name(type.self, mi);
                            std::tie(base_name, base_idx) = require_semantic(msem, member_name);
                        }
                    }

                    if (all_same_float4 && type.member_types.size() == 4 && !any_extra_sem) {
                        for (uint32_t mi = 0; mi < type.member_types.size(); ++mi) {
                            uint32_t loc = compiler->get_member_decoration(
                                type.self, mi, spv::DecorationLocation);
                            hlsl->add_vertex_attribute_remap({
                                loc,
                                fmt::format("{}{}", base_name, base_idx + mi)
                            });
                        }
                    } else {
                        for (uint32_t mi = 0; mi < type.member_types.size(); ++mi) {
                            uint32_t loc = compiler->get_member_decoration(
                                type.self, mi, spv::DecorationLocation);
                            auto msem = compiler->get_member_decoration_string(
                                type.self, mi, spv::DecorationHlslSemanticGOOGLE);
                            auto member_name = compiler->get_member_name(type.self, mi);
                            auto [name, idx] = require_semantic(msem, member_name);
                            hlsl->add_vertex_attribute_remap({
                                loc,
                                fmt::format("{}{}", name, idx)
                            });
                        }
                    }
                } else {
                    auto [name, idx] = require_semantic(sem, input_name);
                    hlsl->add_vertex_attribute_remap({
                        base_loc,
                        fmt::format("{}{}", name, idx)
                    });
                }
            }
        }

    } else if (target.lang == axslc::SHADER_LANG_MSL) {
        auto* msl = static_cast<spirv_cross::CompilerMSL*>(compiler.get());
        auto msl_options = msl->get_msl_options();
        msl_options.platform = options.msl_ios
            ? spirv_cross::CompilerMSL::Options::iOS
            : spirv_cross::CompilerMSL::Options::macOS;
        msl_options.ios_support_base_vertex_instance = true;
        msl_options.enable_decoration_binding = true;
        msl_options.enable_base_index_zero = true;
        msl_options.msl_version = static_cast<uint32_t>(target.profile);
        msl->set_msl_options(msl_options);
    }

    compiler->set_common_options(spv_options);
    std::string code = compiler->compile();

    if (uniform_block_names &&
        (target.lang == axslc::SHADER_LANG_ESSL || target.lang == axslc::SHADER_LANG_GLSL))
    {
        uniform_block_names->clear();
        for (const auto& resource : compiler->get_shader_resources().uniform_buffers)
        {
            UniformBlockNameOverride item;
            item.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
            item.descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
                ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
            item.name = compiler->get_remapped_declared_block_name(resource.id);
            uniform_block_names->push_back(std::move(item));
        }
    }

    return code;
}

} // namespace axslcc::cross
