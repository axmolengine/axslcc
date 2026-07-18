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
void write_file(const fs::path& path, std::string_view data);

// ============= Stage & Format Detection =============

ShaderInfo classify(const fs::path& input);
bool is_hlsl_source(const fs::path& input);

// ============= Vertex Attribute Semantics =============

// Parses a full semantic string into {base name, index}.
// Returns {"", 0} for an empty semantic.
std::pair<std::string, uint16_t> parse_semantic(std::string_view semantic);

// ============= Path Utilities =============

fs::path output_path_for_target(const Options& options, const Target& target);

} // namespace axslcc::utils
