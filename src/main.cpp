#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "compiler.h"
#include "utils.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        auto options = axslcc::utils::parse_args(argc, argv);
        
        axslcc::Compiler compiler;
        compiler.initialize();
        
        try {
            compiler.compile(options);
        } catch (...) {
            compiler.finalize();
            throw;
        }
        
        compiler.finalize();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        axslcc::utils::print_help();
        return 1;
    }
}

