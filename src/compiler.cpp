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
        throw std::runtime_error("Cannot determine shader stage: " + options.input.string());
    auto stage = *stage_opt;

    CompileUnit unit;
    try {
        unit = spirv::compile_input(options);
    }
    catch (const std::exception& e) {
        std::cerr << "axslcc: error compiling " << options.input.filename().string()
                  << ":\n  " << e.what() << std::endl;
        throw;
    }

    std::vector<OutputBlob> outputs;
    std::vector<tlx::byte_buffer> reflections;

    for (const auto& target : options.targets) {
        auto blob = cross::cross_compile(target, unit.spirv, options.input);

#ifdef _WIN32
        // Optional: compile SPIRV-Cross HLSL output to DXIL bytecode
        if (options.dxil && target.lang == axslc::SHADER_LANG_HLSL && !blob.binary)
        {
            try
            {
                std::string hlslSource(reinterpret_cast<const char*>(blob.data.data()), blob.data.size() - 1);
                auto dxcResult = dxc::compile_source(hlslSource, unit.stage,
                                                      options.include_dirs, options.defines);
                blob.data = std::move(dxcResult.dxil);
                blob.binary = true;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[dxc] --dxil failed for " << options.input.filename().string()
                          << ": " << e.what() << std::endl;
            }
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
