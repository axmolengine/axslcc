#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include "compiler.h"
#include "utils.h"

#include <exception>
#include <iostream>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <csignal>
#    include <cstdlib>
#    include <cstring>
#    include <unistd.h>
#endif

static int run_main(int argc, char** argv)
{
    try
    {
        auto options = axslcc::utils::parse_args(argc, argv);

        axslcc::Compiler compiler;
        compiler.initialize();

        try
        {
            compiler.compile(options);
        }
        catch (...)
        {
            compiler.finalize();
            throw;
        }

        compiler.finalize();
        return 0;
    }
    catch (std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << "\n\n";
        axslcc::utils::print_help();
        return 1;
    }
}

#ifndef _WIN32
static void signal_handler(int sig)
{
    const char* name = "unknown signal";
    switch (sig)
    {
    case SIGSEGV:
        name = "segmentation fault";
        break;
    case SIGABRT:
        name = "abort";
        break;
    case SIGFPE:
        name = "floating point exception";
        break;
    case SIGILL:
        name = "illegal instruction";
        break;
    default:
        break;
    }
    write(STDERR_FILENO, "error: axslcc crashed with signal: ", 37);
    write(STDERR_FILENO, name, std::strlen(name));
    write(STDERR_FILENO, "\n", 1);
    _exit(1);
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    __try
    {
        return run_main(argc, argv);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        auto exceptionCode = GetExceptionCode();
        std::cerr << "error: axslcc encountered an unrecoverable system error (exception code 0x" << std::hex
                  << exceptionCode << std::dec << ")\n";
        return static_cast<int>(exceptionCode);
    }
#else
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGILL, signal_handler);
    return run_main(argc, argv);
#endif
}
