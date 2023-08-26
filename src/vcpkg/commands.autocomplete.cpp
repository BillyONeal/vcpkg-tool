#include <vcpkg/base/files.h>
#include <vcpkg/base/lineinfo.h>
#include <vcpkg/base/messages.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/util.h>

#include <vcpkg/commands.autocomplete.h>
#include <vcpkg/commands.h>
#include <vcpkg/commands.integrate.h>
#include <vcpkg/metrics.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/registries.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkglib.h>
#include <vcpkg/vcpkgpaths.h>

#include <string>
#include <vector>

using namespace vcpkg;

namespace
{
    [[noreturn]] void output_sorted_results_and_exit(const LineInfo& line_info, std::vector<std::string>&& results)
    {
        Util::sort(results);
        msg::write_unlocalized_text_to_stdout(Color::none, Strings::join("\n", results));
        Checks::exit_success(line_info);
    }

    std::vector<std::string> combine_port_with_triplets(StringView port, View<TripletFile> triplets)
    {
        return Util::fmap(triplets,
                          [&](const TripletFile& triplet) { return fmt::format("{}:{}", port, triplet.name); });
    }

    std::vector<std::string> builtin_port_names(const VcpkgPaths& paths)
    {
        return Util::fmap(
            paths.get_filesystem().get_directories_non_recursive(paths.builtin_ports_directory(), IgnoreErrors{}),
            [](const Path& p) { return p.filename().to_string(); });
    }

    std::vector<std::string> known_reachable_port_names_no_network(const VcpkgPaths& paths)
    {
        return paths.make_registry_set()->get_all_known_reachable_port_names_no_network();
    }

    std::vector<std::string> installed_port_specs(const VcpkgPaths& paths)
    {
        const StatusParagraphs status_db = database_load_check(paths.get_filesystem(), paths.installed());
        auto installed_packages = get_installed_ports(status_db);

        return Util::fmap(installed_packages, [](auto&& pgh) -> std::string { return pgh.spec().to_string(); });
    }
} // unnamed namespace

namespace vcpkg
{
    constexpr CommandMetadata CommandAutocompleteMetadata{
        "autocomplete",
        {/*Intentionally undocumented*/},
        {},
        AutocompletePriority::Never,
        AutocompleteArguments::None,
        0,
        SIZE_MAX,
        {},
    };

    void command_autocomplete_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths)
    {
        g_should_send_metrics = false;

        const auto all_commands_metadata = get_all_commands_metadata();

        auto&& command_arguments = args.get_forwardable_arguments();
        // Handles vcpkg <command>
        if (command_arguments.size() <= 1)
        {
            StringView requested_command = "";
            if (command_arguments.size() == 1)
            {
                requested_command = command_arguments[0];
            }

            std::vector<std::string> results;

            // First try public commands
            for (auto&& metadata : all_commands_metadata)
            {
                if (metadata->autocomplete_priority == AutocompletePriority::Public &&
                    Strings::case_insensitive_ascii_starts_with(metadata->name, requested_command))
                {
                    results.push_back(metadata->name.to_string());
                }
            }

            // If no public commands match, try intenral commands
            if (results.empty())
            {
                for (auto&& metadata : all_commands_metadata)
                {
                    if (metadata->autocomplete_priority == AutocompletePriority::Internal &&
                        Strings::case_insensitive_ascii_starts_with(metadata->name, requested_command))
                    {
                        results.push_back(metadata->name.to_string());
                    }
                }
            }

            output_sorted_results_and_exit(VCPKG_LINE_INFO, std::move(results));
        }

        // command_arguments.size() >= 2
        const auto& command_name = command_arguments[0];

        // Handles vcpkg install package:<triplet>
        if (Strings::case_insensitive_ascii_equals(command_name, "install"))
        {
            StringView last_arg = command_arguments.back();
            auto colon = Util::find(last_arg, ':');
            if (colon != last_arg.end())
            {
                StringView port_name{last_arg.begin(), colon};
                StringView triplet_prefix{colon + 1, last_arg.end()};
                // TODO: Support autocomplete for ports in --overlay-ports
                auto maybe_port =
                    Paragraphs::try_load_port(paths.get_filesystem(), paths.builtin_ports_directory() / port_name);
                if (!maybe_port)
                {
                    Checks::exit_success(VCPKG_LINE_INFO);
                }

                auto triplets = paths.get_triplet_db().available_triplets;
                Util::erase_remove_if(triplets, [&](const TripletFile& tf) {
                    return !Strings::case_insensitive_ascii_starts_with(tf.name, triplet_prefix);
                });

                auto result = combine_port_with_triplets(port_name, triplets);

                output_sorted_results_and_exit(VCPKG_LINE_INFO, std::move(result));
            }
        }

        const bool second_arg_is_port =
            command_arguments.size() > 2 && Strings::case_insensitive_ascii_equals(command_arguments[1], "port");

        for (auto&& metadata : all_commands_metadata)
        {
            if (Strings::case_insensitive_ascii_equals(command_name, metadata->name))
            {
                StringView prefix = command_arguments.back();
                std::vector<std::string> results;

                const bool is_option = Strings::starts_with(prefix, "-");
                if (is_option)
                {
                    for (const auto& s : metadata->options.switches)
                    {
                        results.push_back(Strings::concat("--", s.name));
                    }
                    for (const auto& s : metadata->options.settings)
                    {
                        results.push_back(Strings::concat("--", s.name));
                    }
                    for (const auto& s : metadata->options.multisettings)
                    {
                        results.push_back(Strings::concat("--", s.name));
                    }

                    Util::erase_remove_if(results, [&](const std::string& s) {
                        return !Strings::case_insensitive_ascii_starts_with(s, prefix);
                    });

                    output_sorted_results_and_exit(VCPKG_LINE_INFO, std::move(results));
                }

                switch (metadata->autocomplete_arguments)
                {
                    case AutocompleteArguments::None: Checks::exit_success(VCPKG_LINE_INFO);
                    case AutocompleteArguments::BuiltinPortNames:
                    default: Checks::unreachable(VCPKG_LINE_INFO);
                }

                else
                {
                    // if (metadata->valid_arguments != nullptr)
                    //{
                    //     results = metadata->valid_arguments(paths);
                    // }
                }

                if (Strings::case_insensitive_ascii_equals(metadata->name, "install") && results.size() == 1 &&
                    !is_option)
                {
                    const auto port_at_each_triplet =
                        combine_port_with_triplets(results[0], paths.get_triplet_db().available_triplets);
                    Util::Vectors::append(&results, port_at_each_triplet);
                }

                output_sorted_results_and_exit(VCPKG_LINE_INFO, std::move(results));
            }
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }
} // namespace vcpkg
