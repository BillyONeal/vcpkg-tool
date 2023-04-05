#include <vcpkg/base/fwd/message_sinks.h>

#include <vcpkg/base/downloads.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/optional.h>
#include <vcpkg/base/system.process.h>

#include <vcpkg/archives.h>
#include <vcpkg/commands.bootstrap-standalone.h>
#include <vcpkg/commands.version.h>
#include <vcpkg/tools.h>
#include <vcpkg/vcpkgcmdarguments.h>

#include <string>

#if defined(VCPKG_BUILD_BOOTSTRAP_FILES)
#include <cmrc/cmrc.hpp>

CMRC_DECLARE(bootstrap_resources);

namespace
{
    using namespace vcpkg;

    void extract_cmrc(Filesystem& fs,
                      cmrc::embedded_filesystem& embedded_filesystem,
                      const Path& base,
                      std::string& root)
    {
        const auto original_root_size = root.size();
        for (auto&& entry : embedded_filesystem.iterate_directory(root))
        {
            const std::string& filename = entry.filename();
            if (!root.empty())
            {
                root.push_back('/');
            }

            root.append(filename);
            auto relative_path = base / root;
            relative_path.make_preferred();
            if (entry.is_directory())
            {
                fs.create_directory(relative_path, VCPKG_LINE_INFO);
                extract_cmrc(fs, embedded_filesystem, base, root);
            }
            else
            {
                const auto file = embedded_filesystem.open(root);
                const auto begin = file.begin();
                const auto end = file.end();
                fs.write_contents(
                    relative_path, StringView{begin, static_cast<std::size_t>(end - begin)}, VCPKG_LINE_INFO);
            }

            root.resize(original_root_size);
        }
    }
}

#endif // ^^^ VCPKG_BUILD_BOOTSTRAP_FILES

namespace vcpkg::Commands
{
    void BootstrapStandaloneCommand::perform_and_exit(const VcpkgCmdArguments& args, Filesystem& fs) const
    {
#if defined(VCPKG_BUILD_BOOTSTRAP_FILES)
        const auto maybe_vcpkg_root_env = args.vcpkg_root_dir_env.get();
        if (!maybe_vcpkg_root_env)
        {
            Checks::msg_exit_with_message(VCPKG_LINE_INFO, msgVcpkgRootRequired);
        }

        const auto& vcpkg_root = fs.almost_canonical(*maybe_vcpkg_root_env, VCPKG_LINE_INFO);
        fs.create_directories(vcpkg_root, VCPKG_LINE_INFO);
        fs.write_contents(vcpkg_root / ".vcpkg-root", StringView{}, VCPKG_LINE_INFO);
#if defined(_WIN32)
        static constexpr StringLiteral exe_name{"vcpkg.exe"};
#else
        static constexpr StringLiteral exe_name{"vcpkg"};
#endif
        const auto exe_path = vcpkg_root / exe_name;
        if (!fs.is_regular_file(exe_path))
        {
            fs.copy_file(get_exe_path_of_current_process(), exe_path, CopyOptions::none, VCPKG_LINE_INFO);
        }

        auto embedded_filesystem = cmrc::bootstrap_resources::get_filesystem();
        std::string root;
        extract_cmrc(fs, embedded_filesystem, vcpkg_root, root);
        Checks::exit_success(VCPKG_LINE_INFO);
#else  // ^^^ VCPKG_BUILD_BOOTSTRAP_FILES / !VCPKG_BUILD_BOOTSTRAP_FILES vvv
        (void)args;
        (void)fs;
        Checks::msg_exit_with_message(VCPKG_LINE_INFO, msgBootstrapUnavailable);
#endif // ^^^ !VCPKG_BUILD_BOOTSTRAP_FILES
    }
}
