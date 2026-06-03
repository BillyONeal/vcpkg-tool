#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/message_sinks.h>

#include <vcpkg/binarycaching.h>

using namespace vcpkg;

namespace vcpkg::Checks
{
    void on_final_cleanup_and_exit() { }
}

namespace
{
#if defined(_WIN32)
    constexpr StringLiteral default_cache_path = "C:\\default";
#else
    constexpr StringLiteral default_cache_path = "/default";
#endif
}

// avoid -Wmissing-prototypes
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    auto begin = reinterpret_cast<const char*>(data);
    const std::string input(begin, size);
    std::vector<std::string> args;
    args.push_back(input);

    SinkBufferedDiagnosticContext context{null_sink};
    (void)parse_binary_provider_configs(context, default_cache_path, input, {});
    (void)parse_binary_provider_configs(context, default_cache_path, {}, args);
    (void)parse_binary_provider_configs(context, default_cache_path, input, args);
    return 0;
}
