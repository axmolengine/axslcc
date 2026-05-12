#pragma once

#include "types.h"

namespace axslcc
{

// ============= Main Compilation Interface =============

class Compiler
{
public:
    Compiler() = default;
    ~Compiler() = default;

    // Initialize the compiler (must be called once at startup)
    void initialize();

    // Finalize the compiler (must be called once at shutdown)
    void finalize();

    // Compile shader according to options
    void compile(const Options& options);

private:
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
};

} // namespace axslcc
