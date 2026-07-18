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
#include "sc_writer.h"
#include "utils.h"
#include "version.h"

#include "yasio/obstream.hpp"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace axslcc::sc_writer
{

namespace
{

constexpr uint16_t kScMajor = AXSLCC_MAJOR;
constexpr uint16_t kScMinor = AXSLCC_MINOR;

uint32_t sc_lang(const Target& target)
{
    return static_cast<uint32_t>(target.lang);
}

uint32_t sc_stage(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Vertex:
        return SC_STAGE_VERTEX;
    case ShaderStage::Fragment:
        return SC_STAGE_FRAGMENT;
    case ShaderStage::Compute:
        return SC_STAGE_COMPUTE;
    default:
        throw std::runtime_error("unknown shader stage");
    }
}

template <typename T>
void write_struct(yasio::fast_obstream& out, const T& value)
{
    out.write_bytes(&value, static_cast<int>(sizeof(T)));
}

} // namespace

void write_archive(const Options& options,
                      ShaderStage stage,
                      const std::vector<OutputBlob>& outputs,
    const std::vector<tlx::byte_buffer>& reflections)
{
    std::vector<ScTarget> targets;
    targets.reserve(outputs.size());

    for (size_t i = 0; i < outputs.size(); ++i) {
        ScTarget item;
        item.lang = sc_lang(*outputs[i].target);
        item.profile = static_cast<uint32_t>(outputs[i].target->profile);
        if (!outputs[i].target->isKeepSource(options.keep_source_hint))
            item.profile |= SC_BYTECODE_FLAG;
        item.stage = sc_stage(stage);
        item.code = outputs[i].data;
        if (i < reflections.size()) {
            auto& ref = reflections[i];
            item.refl.assign(reinterpret_cast<const char*>(ref.data()), ref.size());
        }
        targets.push_back(std::move(item));
    }

    if (targets.size() > (std::numeric_limits<uint16_t>::max)())
        throw std::runtime_error("too many shader targets for .sc archive");

    yasio::fast_obstream out;
    out.write<uint32_t>(SC_CHUNK);
    const auto sc_size_offset = out.length();
    out.write<uint32_t>(0);

    axslc::sc_chunk header{};
    header.major = kScMajor;
    header.minor = kScMinor;
    header.num_targets = static_cast<uint16_t>(targets.size());
    write_struct(out, header);

    const auto entries_offset = out.length();
    for (size_t i = 0; i < targets.size(); ++i) {
        axslc::sc_target_entry dummy{};
        write_struct(out, dummy);
    }

    for (auto& target : targets) {
        if (out.length() > (std::numeric_limits<uint32_t>::max)())
            throw std::runtime_error(".sc archive offset exceeds 32-bit limit");
        target.offset = static_cast<uint32_t>(out.length());

        if (target.code.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            target.refl.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            throw std::runtime_error(".sc archive blob exceeds supported size");

        const uint32_t code_size = static_cast<uint32_t>(target.code.size());
        const uint32_t refl_size = static_cast<uint32_t>(target.refl.size());
        if (code_size > (std::numeric_limits<uint32_t>::max)() - sizeof(uint32_t) - 8 ||
            (refl_size && code_size > (std::numeric_limits<uint32_t>::max)() - sizeof(uint32_t) - 16 - refl_size))
            throw std::runtime_error(".sc archive stage chunk exceeds 32-bit limit");
        const uint32_t stage_size = sizeof(uint32_t) + 8 + code_size + (refl_size ? 8 + refl_size : 0);

        out.write<uint32_t>(SC_CHUNK_STAG);
        out.write<uint32_t>(stage_size);
        out.write<uint32_t>(target.stage);
        out.write<uint32_t>(SC_CHUNK_CODE);
        out.write<uint32_t>(code_size);
        out.write_bytes(target.code.data(), static_cast<int>(target.code.size()));

        if (!target.refl.empty()) {
            out.write<uint32_t>(SC_CHUNK_REFL);
            out.write<uint32_t>(refl_size);
            out.write_bytes(target.refl.data(), static_cast<int>(target.refl.size()));
        }
    }

    if (out.length() > (std::numeric_limits<uint32_t>::max)())
        throw std::runtime_error(".sc archive exceeds 32-bit limit");

    uint32_t sc_size = static_cast<uint32_t>(out.length());
    out.pwrite<uint32_t>(static_cast<ptrdiff_t>(sc_size_offset), sc_size);

    for (size_t i = 0; i < targets.size(); ++i) {
        axslc::sc_target_entry entry{targets[i].lang, targets[i].profile, targets[i].offset};
        std::memcpy(out.data() + entries_offset + i * sizeof(entry), &entry, sizeof(entry));
    }

    auto buf = out.buffer();
    utils::write_file(utils::output_path_for_target(options, *outputs.front().target),
                       std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
}

} // namespace axslcc::sc_writer
