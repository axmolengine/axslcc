#include "cross_compiler.h"
#include "spirv_compiler.h"
#include "utils.h"

#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"

#include <memory>

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

} // namespace

OutputBlob cross_compile(const Target& target, const std::vector<uint32_t>& spirv,
                         const fs::path& input)
{
    if (target.lang == axslc::SHADER_LANG_SPIRV)
        return OutputBlob{target, spirv::spirv_to_bytes(spirv), true};

    auto compiler = make_cross_compiler(target, spirv);
    auto options = compiler->get_common_options();
    options.flatten_multidimensional_arrays = true;

    if (target.lang == axslc::SHADER_LANG_ESSL || target.lang == axslc::SHADER_LANG_GLSL) {
        compiler->build_combined_image_samplers();
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
                for (const auto& res : resources.stage_inputs)
                {
                    auto& type = compiler->get_type(res.type_id);
                    for (uint32_t mi = 0; mi < static_cast<uint32_t>(type.member_types.size()); ++mi)
                    {
                        std::string rawName = compiler->get_member_name(type.self, mi);
                        std::string memName = utils::clean_input_name(rawName);

                        auto it = semantics.find(memName);
                        if (it != semantics.end())
                        {
                            uint32_t loc = compiler->get_member_decoration(type.self, mi, spv::DecorationLocation);
                            std::string fullSem = make_semantic_string(it->second.first, it->second.second);
                            hlsl->add_vertex_attribute_remap({loc, fullSem});
                        }
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
    std::vector<uint8_t> bytes(code.begin(), code.end());
    bytes.push_back(0);
    return OutputBlob{target, std::move(bytes), false};
}

} // namespace axslcc::cross
