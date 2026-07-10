#include "cross_compiler.h"
#include "spirv_compiler.h"
#include "utils.h"

#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace axslcc::cross
{

namespace
{

std::unique_ptr<spirv_cross::CompilerGLSL> make_cross_compiler(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_HLSL)
        return std::make_unique<spirv_cross::CompilerHLSL>(spirv);
    if (target.lang == axslc::SHADER_LANG_MSL)
        return std::make_unique<spirv_cross::CompilerMSL>(spirv);
    return std::make_unique<spirv_cross::CompilerGLSL>(spirv);
}

std::string make_semantic_string(const std::string& name, uint16_t index)
{
    if (name == "TEXCOORD" || name == "COLOR")
        return name + std::to_string(index);
    return name;
}

// Inject BuiltIn PointSize decoration on a SPIRV-Cross compiler
// if a vertex shader output variable named "pointSize" is found.
static void inject_pointsize(spirv_cross::CompilerGLSL* compiler)
{
    auto resources = compiler->get_shader_resources();
    auto it = std::find_if(resources.stage_outputs.begin(), resources.stage_outputs.end(),
        [&](const spirv_cross::Resource& res) {
            auto name = compiler->get_name(res.id);
            return name.find("pointSize") != std::string::npos ||
                   name.find("PointSize") != std::string::npos;
        });
    if (it != resources.stage_outputs.end())
    {
        compiler->set_decoration(it->id, spv::DecorationBuiltIn, spv::BuiltInPointSize);
    }
}

} // namespace

OutputBlob cross_compile(const Target& target, const std::vector<uint32_t>& spirv,
                         const fs::path& input)
{
    if (target.lang == axslc::SHADER_LANG_SPIRV)
        return OutputBlob{target, spirv::spirv_to_bytes(spirv), true};

    auto compiler = make_cross_compiler(target, spirv);
    inject_pointsize(compiler.get());
    auto options = compiler->get_common_options();
    options.flatten_multidimensional_arrays = true;

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

        // Strip "input." / "output." prefix from varying names for GLES linker compatibility
        auto post_resources = compiler->get_shader_resources();
        for (const auto& r : post_resources.stage_inputs)
            compiler->set_name(r.id, utils::clean_input_name(r.name));
        for (const auto& r : post_resources.stage_outputs)
            compiler->set_name(r.id, utils::clean_input_name(r.name));
    }

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
        hlsl_options.user_semantic = true;
        hlsl->set_hlsl_options(hlsl_options);

        if (uint32_t builtin = hlsl->remap_num_workgroups_builtin()) {
            hlsl->set_decoration(builtin, spv::DecorationDescriptorSet, 0);
            hlsl->set_decoration(builtin, spv::DecorationBinding, 0);
        }

        // Map vertex inputs to correct D3D semantics from original HLSL source
        if (!input.empty() && utils::is_hlsl_source(input))
        {
            auto semantics = utils::parse_hlsl_semantics(input);
            if (!semantics.empty())
            {
                auto resources = compiler->get_shader_resources();

                auto remap_one = [&](const std::string& name, uint32_t loc) {
                    auto it = semantics.find(name);
                    if (it != semantics.end())
                    {
                        std::string fullSem = make_semantic_string(it->second.first, it->second.second);
                        hlsl->add_vertex_attribute_remap({loc, fullSem});
                    }
                };

                for (const auto& res : resources.stage_inputs)
                {
                    auto& inputType = compiler->get_type(res.type_id);
                    uint32_t memberCount = static_cast<uint32_t>(inputType.member_types.size());

                    if (memberCount > 0)
                    {
                        for (uint32_t mi = 0; mi < memberCount; ++mi)
                        {
                            std::string rawName = compiler->get_member_name(inputType.self, mi);
                            remap_one(utils::clean_input_name(rawName), compiler->get_member_decoration(inputType.self, mi, spv::DecorationLocation));
                        }
                    }
                    else
                    {
                        std::string rawName = res.name.empty() ? compiler->get_fallback_name(res.id) : res.name;
                        remap_one(utils::clean_input_name(rawName), compiler->get_decoration(res.id, spv::DecorationLocation));
                    }
                }
            }
        }
    } else if (target.lang == axslc::SHADER_LANG_MSL) {
        auto* msl = static_cast<spirv_cross::CompilerMSL*>(compiler.get());
        auto msl_options = msl->get_msl_options();
        msl_options.platform = spirv_cross::CompilerMSL::Options::iOS;
        msl_options.msl_version = static_cast<uint32_t>(target.profile);
        msl->set_msl_options(msl_options);
    }

    compiler->set_common_options(options);
    std::string code = compiler->compile();
    tlx::byte_buffer bytes(code.begin(), code.end());
    bytes.push_back(0);
    return OutputBlob{target, std::move(bytes), false};
}

} // namespace axslcc::cross
