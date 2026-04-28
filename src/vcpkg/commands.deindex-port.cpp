#include <vcpkg/base/checks.h>
#include <vcpkg/base/files.h>

#include <vcpkg/baseline-manipulation.h>
#include <vcpkg/commands.deindex-port.h>
#include <vcpkg/registries.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>

using namespace vcpkg;

namespace vcpkg
{
    constexpr CommandMetadata CommandDeindexPortMetadata{
        "x-deindex-port",
        msgCmdDeindexPortSynopsis,
        {msgCmdDeindexPortExample1, "vcpkg x-deindex-port zlib"},
        Undocumented,
        AutocompletePriority::Public,
        1,
        1,
        {},
        nullptr,
    };

    void command_deindex_port_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths)
    {
        const auto parsed_args = args.parse_arguments(CommandDeindexPortMetadata);
        const auto& port_name = parsed_args.command_arguments[0];

        auto& fs = paths.get_filesystem();
        const auto baseline_path = paths.builtin_registry_versions / "baseline.json";
        if (!fs.exists(baseline_path, IgnoreErrors{}))
        {
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgAddVersionFileNotFound, msg::path = baseline_path);
        }

        const auto port_directory = paths.builtin_ports_directory() / port_name;
        Checks::msg_check_exit(VCPKG_LINE_INFO,
                               fs.exists(port_directory, IgnoreErrors{}),
                               msgPortDoesNotExist,
                               msg::package_name = port_name);

        auto baseline_map = get_builtin_baseline(paths).value_or_exit(VCPKG_LINE_INFO);
        Checks::msg_check_exit(VCPKG_LINE_INFO,
                               baseline_map.erase(port_name) != 0,
                               msgDeindexPortBaselineMissingEntry,
                               msg::package_name = port_name,
                               msg::path = baseline_path);

        fs.remove_all(port_directory, VCPKG_LINE_INFO);
        msg::println(Color::success, msgDeindexPortRemovedPortDirectory, msg::path = port_directory);

        write_json_file(fs, serialize_baseline(baseline_map), baseline_path);
        msg::println(Color::success,
                     msgDeindexPortRemovedBaselineEntry,
                     msg::package_name = port_name,
                     msg::path = baseline_path);

        Checks::exit_success(VCPKG_LINE_INFO);
    }
} // namespace vcpkg