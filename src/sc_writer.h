#pragma once

#include "types.h"

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace axslcc::sc_writer
{

// ============= Archive multi shader targets Format Output =============

void write_archive(const Options& options, ShaderStage stage, const std::vector<OutputBlob>& outputs,
              const std::vector<tlx::byte_buffer>& reflections);

} // namespace axslcc::sc_writer
