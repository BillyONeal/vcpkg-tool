#pragma once

#include <vcpkg/base/fwd/stringview.h>

#include <vcpkg/commands.interface.h>

#define STRINGIFY(...) #__VA_ARGS__
#define MACRO_TO_STRING(X) STRINGIFY(X)

#if !defined(VCPKG_TOOL_VERSION)
#error VCPKG_TOOL_VERSION must be defined
#endif

#define VCPKG_TOOL_VERSION_AS_STRING MACRO_TO_STRING(VCPKG_TOOL_VERSION)

#if !defined(VCPKG_BASE_VERSION)
#error VCPKG_BASE_VERSION must be defined
#endif

#define VCPKG_BASE_VERSION_AS_STRING MACRO_TO_STRING(VCPKG_BASE_VERSION)

namespace vcpkg::Commands::Version
{
    extern const StringLiteral version;
    void perform_and_exit(const VcpkgCmdArguments& args, Filesystem& fs);

    struct VersionCommand : BasicCommand
    {
        virtual void perform_and_exit(const VcpkgCmdArguments& args, Filesystem& fs) const override;
    };
}
