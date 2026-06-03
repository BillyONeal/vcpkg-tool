#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/message_sinks.h>

#include <vcpkg/binarycaching.h>

using namespace vcpkg;

namespace vcpkg::Checks
{
    void on_final_cleanup_and_exit() { }
}

// avoid -Wmissing-prototypes
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    auto begin = reinterpret_cast<const char*>(data);
    SinkBufferedDiagnosticContext context{null_sink};
    (void)parse_download_configuration(context, std::string(begin, size));
    return 0;
}
