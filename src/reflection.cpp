#include "reflection.h"
#include "axslc-spec.h"

#include "spirv_cross.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include "utils.h"
#include "yasio/obstream.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace axslcc::reflection
{

namespace
{

struct SamplerAbiInfo
{
    std::string name;
    int32_t reflected_binding{-1};
    uint16_t space{0};
};

constexpr VariableTypeMap kVariableTypeMap[] = {
    {spirv_cross::SPIRType::Float, 4, 1, axslc::SC_TYPE_FLOAT4},
    {spirv_cross::SPIRType::Float, 3, 1, axslc::SC_TYPE_FLOAT3},
    {spirv_cross::SPIRType::Float, 2, 1, axslc::SC_TYPE_FLOAT2},
    {spirv_cross::SPIRType::Float, 1, 1, axslc::SC_TYPE_FLOAT},
    {spirv_cross::SPIRType::Int, 4, 1, axslc::SC_TYPE_INT4},
    {spirv_cross::SPIRType::Int, 3, 1, axslc::SC_TYPE_INT3},
    {spirv_cross::SPIRType::Int, 2, 1, axslc::SC_TYPE_INT2},
    {spirv_cross::SPIRType::Int, 1, 1, axslc::SC_TYPE_INT},
    {spirv_cross::SPIRType::UShort, 4, 1, axslc::SC_TYPE_USHORT4},
    {spirv_cross::SPIRType::UShort, 2, 1, axslc::SC_TYPE_USHORT2},
    {spirv_cross::SPIRType::UByte, 4, 1, axslc::SC_TYPE_UBYTE4},
    {spirv_cross::SPIRType::Float, 4, 4, axslc::SC_TYPE_MAT4},
    {spirv_cross::SPIRType::Float, 3, 3, axslc::SC_TYPE_MAT3},
    {spirv_cross::SPIRType::Half, 4, 1, axslc::SC_TYPE_HALF4},
    {spirv_cross::SPIRType::Half, 3, 1, axslc::SC_TYPE_HALF3},
    {spirv_cross::SPIRType::Half, 2, 1, axslc::SC_TYPE_HALF2},
    {spirv_cross::SPIRType::Half, 1, 1, axslc::SC_TYPE_HALF},
};

uint16_t resolve_sc_type(const spirv_cross::SPIRType& type)
{
    for (const auto& item : kVariableTypeMap) {
        if (item.base_type == static_cast<uint32_t>(type.basetype) && item.vec_size == type.vecsize && item.columns == type.columns)
            return item.sc_type;
    }
    return axslc::SC_TYPE_FLOAT4;
}

uint16_t array_size(const spirv_cross::SPIRType& type)
{
    if (type.array.empty())
        return 1;

    uint32_t size = 1;
    for (uint32_t dim : type.array)
        size *= (std::max)(dim, 1u);
    return static_cast<uint16_t>((std::min)(size, 0xffffu));
}

template <typename T>
void write_struct(yasio::fast_obstream& out, const T& value)
{
    out.write_bytes(&value, static_cast<int>(sizeof(T)));
}

void copy_name(char* dst, size_t size, std::string_view name)
{
    std::memset(dst, 0, size);
    const auto copy_size = (std::min)(name.size(), size - 1);
    std::memcpy(dst, name.data(), copy_size);
}

std::unique_ptr<spirv_cross::CompilerGLSL> make_compiler(const Target& target, const std::vector<uint32_t>& spirv)
{
    if (target.lang == axslc::SHADER_LANG_HLSL)
        return std::make_unique<spirv_cross::CompilerHLSL>(spirv);
    if (target.lang == axslc::SHADER_LANG_MSL)
        return std::make_unique<spirv_cross::CompilerMSL>(spirv);
    return std::make_unique<spirv_cross::CompilerGLSL>(spirv);
}

uint32_t align_up(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1)
        return value;

    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t get_msl_scalar_alignment(const spirv_cross::SPIRType& type)
{
    switch (type.basetype)
    {
    case spirv_cross::SPIRType::Boolean:
    case spirv_cross::SPIRType::SByte:
    case spirv_cross::SPIRType::UByte:
        return 1;

    case spirv_cross::SPIRType::Short:
    case spirv_cross::SPIRType::UShort:
    case spirv_cross::SPIRType::Half:
        return 2;

    case spirv_cross::SPIRType::Int:
    case spirv_cross::SPIRType::UInt:
    case spirv_cross::SPIRType::Float:
        return 4;

    case spirv_cross::SPIRType::Int64:
    case spirv_cross::SPIRType::UInt64:
    case spirv_cross::SPIRType::Double:
        return 8;

    default:
        return (std::max)(type.width / 8u, 1u);
    }
}

uint32_t get_msl_type_alignment(const spirv_cross::CompilerGLSL& compiler,
                                const spirv_cross::SPIRType& type)
{
    if (type.basetype == spirv_cross::SPIRType::Struct)
    {
        uint32_t alignment = 1;

        for (const auto member_type_id : type.member_types)
        {
            alignment = (std::max)(
                alignment,
                get_msl_type_alignment(compiler, compiler.get_type(member_type_id)));
        }

        return alignment;
    }

    const uint32_t scalar_alignment = get_msl_scalar_alignment(type);

    // MSL matrices are arrays of column vectors, so their natural alignment
    // is the alignment of one column vector.
    switch (type.vecsize)
    {
    case 1:
        return scalar_alignment;

    case 2:
        return scalar_alignment * 2;

    case 3:
    case 4:
        return scalar_alignment * 4;

    default:
        return scalar_alignment;
    }
}

uint32_t get_msl_struct_size(const spirv_cross::CompilerGLSL& compiler,
                             const spirv_cross::SPIRType& type)
{
    if (type.member_types.empty())
        return 0;

    uint32_t struct_alignment = 1;

    for (const auto member_type_id : type.member_types)
    {
        struct_alignment = (std::max)(
            struct_alignment,
            get_msl_type_alignment(compiler, compiler.get_type(member_type_id)));
    }

    const auto last_index =
        static_cast<uint32_t>(type.member_types.size() - 1);

    const uint32_t last_offset =
        compiler.type_struct_member_offset(type, last_index);

    const uint32_t last_size = static_cast<uint32_t>(
        compiler.get_declared_struct_member_size(type, last_index));

    return align_up(last_offset + last_size, struct_alignment);
}

} // namespace

tlx::byte_buffer build_reflection(const Target& target, const std::vector<uint32_t>& spirv,
    ShaderStage stage, const fs::path& input,
    const std::vector<UniformBlockNameOverride>& uniform_block_names)
{
    auto compiler = target.lang == axslc::SHADER_LANG_SPIRV
        ? make_compiler(Target{axslc::SHADER_LANG_GLSL, 450, "glsl-450"}, spirv)
        : make_compiler(target, spirv);

    const bool is_glsl_target = target.lang == axslc::SHADER_LANG_ESSL || target.lang == axslc::SHADER_LANG_GLSL;

    auto pre_resources = compiler->get_shader_resources();
    const bool is_hlsl = utils::is_hlsl_source(input);
    std::unordered_map<uint32_t, SamplerAbiInfo> sampler_abi_by_id;
    for (const auto& resource : pre_resources.separate_samplers) {
        SamplerAbiInfo abi{};
        abi.name = resource.name.empty() ? compiler->get_fallback_name(resource.id) : resource.name;
        if (!compiler->has_decoration(resource.id, spv::DecorationBinding))
            throw std::runtime_error("Sampler '" + abi.name + "' was not assigned a backend binding by axslcc.");

        abi.reflected_binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        abi.space = compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0u;

        sampler_abi_by_id.emplace(resource.id, std::move(abi));
    }

    if (is_glsl_target) {
        {
            std::unordered_map<uint32_t, std::string> image_names;
            for (const auto& image : pre_resources.separate_images)
                image_names[image.id] = compiler->get_name(image.id);

            compiler->build_combined_image_samplers();

            std::unordered_map<uint32_t, uint32_t> sampler_by_image;
            for (const auto& combined : compiler->get_combined_image_samplers()) {
                auto [sampler_it, inserted] = sampler_by_image.emplace(combined.image_id, combined.sampler_id);
                if (!inserted && sampler_it->second != combined.sampler_id) {
                    const auto image_name = compiler->get_name(combined.image_id);
                    throw std::runtime_error(
                        "GLSL/ESSL backend does not support sampling one texture with multiple sampler states in Axmol: " +
                        image_name + ". Use a single sampler for that texture on GL/GLES targets.");
                }

                auto it = image_names.find(combined.image_id);
                if (it != image_names.end() && !it->second.empty())
                    compiler->set_name(combined.combined_id, it->second);

                const uint32_t binding = compiler->has_decoration(combined.image_id, spv::DecorationBinding)
                    ? compiler->get_decoration(combined.image_id, spv::DecorationBinding)
                    : 0;
                const uint32_t descriptor_set = compiler->has_decoration(combined.image_id, spv::DecorationDescriptorSet)
                    ? compiler->get_decoration(combined.image_id, spv::DecorationDescriptorSet)
                    : 0;
                compiler->set_decoration(combined.combined_id, spv::DecorationBinding, binding);
                compiler->set_decoration(combined.combined_id, spv::DecorationDescriptorSet, descriptor_set);
            }
        }
    }

    auto resources = compiler->get_shader_resources();
    if (!is_glsl_target)
        resources = pre_resources;

    yasio::fast_obstream out;

    axslc::sc_chunk_refl header{};
    copy_name(header.name, sizeof(header.name), input.filename().string());
    header.debug_info = 1;
    write_struct(out, header);

    auto write_input = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler->get_type(resource.type_id);

        auto write_one = [&](const std::string& name, const spirv_cross::SPIRType& input_type,
                            uint32_t location, std::string_view semantic_name = {}) {
            axslc::sc_refl_input input_refl{};
            copy_name(input_refl.name, sizeof(input_refl.name), name);
            input_refl.location = static_cast<int32_t>(location);
            input_refl.semantic_index = 0;
            input_refl.var_type = resolve_sc_type(input_type);

            if (!semantic_name.empty())
            {
                auto [semantic, sem_index] = utils::parse_semantic(semantic_name);
                copy_name(input_refl.semantic, sizeof(input_refl.semantic), semantic);
                input_refl.semantic_index = sem_index;
            }

            write_struct(out, input_refl);
            ++header.num_inputs;
        };

        const uint32_t member_count = static_cast<uint32_t>(type.member_types.size());
        if (member_count > 0)
        {
            for (uint32_t mi = 0; mi < member_count; ++mi)
            {
                const std::string name = compiler->get_member_name(type.self, mi);
                const std::string semantic = is_hlsl
                    ? compiler->get_member_decoration_string(type.self, mi, spv::DecorationHlslSemanticGOOGLE)
                    : std::string{};

                const auto& member_type = compiler->get_type(type.member_types[mi]);
                const uint32_t location = compiler->get_member_decoration(type.self, mi, spv::DecorationLocation);

                write_one(name, member_type, location, semantic);
            }
        }
        else
        {
            const std::string name = resource.name.empty()
                ? compiler->get_fallback_name(resource.id)
                : resource.name;

            const std::string semantic = is_hlsl
                ? compiler->get_decoration_string(resource.id, spv::DecorationHlslSemanticGOOGLE)
                : std::string{};

            const uint32_t location = compiler->get_decoration(resource.id, spv::DecorationLocation);

            write_one(name, type, location, semantic);
        }
    };

    if (stage == ShaderStage::Vertex) {
        for (const auto& resource : resources.stage_inputs)
            write_input(resource);
    }

    auto get_ubo_size = [&](const spirv_cross::SPIRType& type) -> uint32_t {
        if (target.lang == axslc::SHADER_LANG_MSL)
            return get_msl_struct_size(*compiler, type);

        return static_cast<uint32_t>(
            compiler->get_declared_struct_size(type));
    };

    auto get_member_size = [&](const spirv_cross::SPIRType& type, uint32_t index) -> uint32_t {
        return static_cast<uint32_t>(compiler->get_declared_struct_member_size(type, index));
    };

    auto write_ubo = [&](const spirv_cross::Resource& resource) {
        auto& type = compiler->get_type(resource.base_type_id);
        axslc::sc_refl_uniformbuffer ubo{};
        ubo.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        const auto descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
        std::string block_name = resource.name.empty() ? compiler->get_fallback_name(resource.base_type_id) : resource.name;
        for (const auto& item : uniform_block_names)
        {
            if (item.binding == ubo.binding && item.descriptor_set == descriptor_set)
            {
                block_name = item.name;
                break;
            }
        }
        copy_name(ubo.name, sizeof(ubo.name), block_name);
        ubo.size_bytes = get_ubo_size(type);
        ubo.array_size = array_size(compiler->get_type(resource.type_id));
        ubo.num_members = static_cast<uint16_t>(std::min<size_t>(type.member_types.size(), 0xffff));
        write_struct(out, ubo);
        ++header.num_uniform_buffers;

        for (uint32_t i = 0; i < ubo.num_members; ++i) {
            auto& member_type = compiler->get_type(type.member_types[i]);
            axslc::sc_refl_uniformbuffer_member member{};
            copy_name(member.name, sizeof(member.name), compiler->get_member_name(type.self, i));
            member.offset = static_cast<int32_t>(compiler->type_struct_member_offset(type, i));
            member.size_bytes = get_member_size(type, i);
            member.array_size = array_size(member_type);
            member.var_type = resolve_sc_type(member_type);
            write_struct(out, member);
        }
    };

    for (const auto& resource : resources.uniform_buffers)
        write_ubo(resource);

    auto write_texture = [&](const spirv_cross::Resource& resource, bool storage) {
        auto& type = compiler->get_type(resource.type_id);
        axslc::sc_refl_texture texture{};
        copy_name(texture.name, sizeof(texture.name), resource.name.empty() ? compiler->get_fallback_name(resource.id) : resource.name);
        texture.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
        texture.descriptor_set = static_cast<uint16_t>(compiler->has_decoration(resource.id, spv::DecorationDescriptorSet)
            ? compiler->get_decoration(resource.id, spv::DecorationDescriptorSet) : 0);
        texture.image_dim = static_cast<uint8_t>(type.image.dim);
        texture.multisample = type.image.ms ? 1 : 0;
        texture.arrayed = type.image.arrayed ? 1 : 0;
        texture.count = array_size(type);
        write_struct(out, texture);
        if (storage)
            ++header.num_storage_images;
        else
            ++header.num_textures;
    };

    for (const auto& resource : resources.sampled_images)
        write_texture(resource, false);

    if (!is_glsl_target) {
        for (const auto& resource : resources.separate_images)
            write_texture(resource, false);
    }

    if (!is_glsl_target) {
        for (const auto& resource : resources.separate_samplers) {
            auto& type = compiler->get_type(resource.type_id);
            axslc::sc_refl_sampler sampler{};
            const auto& abi = sampler_abi_by_id.at(resource.id);
            copy_name(sampler.name, sizeof(sampler.name), abi.name);
            sampler.binding        = abi.reflected_binding;
            sampler.space          = abi.space;
            sampler.count          = array_size(type);

            static constexpr std::string_view kPresetNames[] = {
                "LinearClamp", "LinearWrap", "LinearMirror", "LinearBorder",
                "PointClamp", "PointWrap", "PointMirror", "PointBorder",
                "LinearMipClamp", "LinearMipWrap", "LinearMipMirror", "LinearMipBorder",
                "AnisoClamp", "AnisoWrap", "AnisoMirror", "AnisoBorder",
                "ShadowCmpClamp", "ShadowCmpWrap", "ShadowCmpMirror", "ShadowCmpBorder",
                "LinearNoMipClamp", "PointNoMipClamp",
            };
            sampler.preset_index = axslc::kInvalidSamplerPreset;
            for (size_t i = 0; i < std::size(kPresetNames); ++i) {
                if (abi.space == axslc::kPresetSamplerDescriptorSet && kPresetNames[i] == abi.name) {
                    sampler.preset_index = static_cast<int16_t>(i);
                    break;
                }
            }
            sampler.flags    = axslc::SC_SAMPLER_FLAG_NONE;
            sampler.reserved = 0;

            write_struct(out, sampler);
            ++header.num_samplers;
        }
    } else {
        // For GL/GLES targets, write custom sampler entries (not built-in presets)
        // so the runtime can bind the correct native sampler objects.
        // Built-in presets use the default combined-image-sampler path.
        for (const auto& resource : pre_resources.separate_samplers) {
            const auto& abi = sampler_abi_by_id.at(resource.id);

            static constexpr std::string_view kPresetNames[] = {
                "LinearClamp", "LinearWrap", "LinearMirror", "LinearBorder",
                "PointClamp", "PointWrap", "PointMirror", "PointBorder",
                "LinearMipClamp", "LinearMipWrap", "LinearMipMirror", "LinearMipBorder",
                "AnisoClamp", "AnisoWrap", "AnisoMirror", "AnisoBorder",
                "ShadowCmpClamp", "ShadowCmpWrap", "ShadowCmpMirror", "ShadowCmpBorder",
                "LinearNoMipClamp", "PointNoMipClamp",
            };
            int16_t presetIdx = axslc::kInvalidSamplerPreset;
            for (size_t i = 0; i < std::size(kPresetNames); ++i) {
                if (abi.space == axslc::kPresetSamplerDescriptorSet && kPresetNames[i] == abi.name) {
                    presetIdx = static_cast<int16_t>(i);
                    break;
                }
            }
            if (presetIdx >= 0)
                continue;  // skip built-in presets for GL/GLES

            auto& type = compiler->get_type(resource.type_id);
            axslc::sc_refl_sampler sampler{};
            copy_name(sampler.name, sizeof(sampler.name), abi.name);
            sampler.binding     = abi.reflected_binding;
            sampler.space       = abi.space;
            sampler.count       = array_size(type);
            sampler.preset_index = axslc::kInvalidSamplerPreset;
            sampler.flags       = axslc::SC_SAMPLER_FLAG_NONE;
            sampler.reserved    = 0;

            write_struct(out, sampler);
            ++header.num_samplers;
        }
    }

    if (stage == ShaderStage::Compute) {
        for (const auto& resource : resources.storage_images)
            write_texture(resource, true);

        for (const auto& resource : resources.storage_buffers) {
            auto& type = compiler->get_type(resource.base_type_id);
            axslc::sc_refl_buffer buffer{};
            copy_name(buffer.name, sizeof(buffer.name), resource.name.empty() ? compiler->get_fallback_name(resource.base_type_id) : resource.name);
            buffer.binding = static_cast<int32_t>(compiler->get_decoration(resource.id, spv::DecorationBinding));
            buffer.size_bytes = get_ubo_size(type);
            buffer.array_stride = static_cast<uint32_t>(compiler->get_declared_struct_size_runtime_array(type, 1)
                - compiler->get_declared_struct_size_runtime_array(type, 0));
            write_struct(out, buffer);
            ++header.num_storage_buffers;
        }
    }

    std::memcpy(out.data(), &header, sizeof(header));
    return std::move(out.buffer());
}

} // namespace axslcc::reflection
