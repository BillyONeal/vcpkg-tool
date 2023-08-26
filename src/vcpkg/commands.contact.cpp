#include <vcpkg/base/checks.h>

#include <vcpkg/commands.contact.h>
#include <vcpkg/vcpkgcmdarguments.h>

namespace vcpkg
{
    constexpr CommandMetadata CommandContactMetadata{
        "contact",
        msgHelpContactCommand,
        {"vcpkg contact"},
        AutocompletePriority::Internal,
        AutocompleteArguments::None,
        0,
        0,
        {},
    };

    void command_contact_and_exit(const VcpkgCmdArguments& args, const Filesystem&)
    {
        (void)args.parse_arguments(CommandContactMetadata);
        msg::println(msgEmailVcpkgTeam, msg::url = "vcpkg@microsoft.com");
        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
