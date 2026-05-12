#include "compiler.h"
#include "spirv_compiler.h"
#include "cross_compiler.h"
#include "reflection.h"
#include "sc_writer.h"
#include "utils.h"

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
    // Step 1: Compile input to SPIRV
    CompileUnit unit = spirv::compile_input(options);

    // Step 2: Cross-compile from SPIRV to target languages
    std::vector<OutputBlob> outputs;
    std::vector<std::vector<uint8_t>> reflections;

    for (const auto& target : options.targets) {
        outputs.push_back(cross::cross_compile(target, unit.spirv));
        if (options.reflect)
            reflections.push_back(reflection::build_reflection(target, unit.spirv, unit.stage, options.input));
    }

    // Step 3: Output results
    if (options.sc) {
        sc_writer::write_sc(options, unit.stage, outputs, reflections);
    } else {
        for (const auto& output : outputs)
            utils::write_file(utils::output_path_for_target(options, output.target), output.data);
    }
}

} // namespace axslcc
