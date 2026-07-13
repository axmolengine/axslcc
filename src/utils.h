#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace axslcc::utils
{

// ============= String Utilities =============

std::string lower(std::string value);
std::vector<std::string> split(std::string_view value, char delim);
bool starts_with(std::string_view value, std::string_view prefix);

// ============= Argument Parsing =============

Target parse_target(std::string_view text);
Options parse_args(int argc, char** argv);
void print_help();
void print_version();

// ============= File I/O =============

std::string read_text_file(const fs::path& path);
void write_file(const fs::path& path, const tlx::byte_buffer& data);

// ============= Stage & Format Detection =============

ShaderInfo classify(const fs::path& input);
bool is_hlsl_source(const fs::path& input);

// ============= Path Utilities =============

fs::path output_path_for_target(const Options& options, const Target& target);

} // namespace axslcc::utils
