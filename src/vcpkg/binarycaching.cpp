#include <vcpkg/base/api-stable-format.h>
#include <vcpkg/base/checks.h>
#include <vcpkg/base/chrono.h>
#include <vcpkg/base/contractual-constants.h>
#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/downloads.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/message_sinks.h>
#include <vcpkg/base/messages.h>
#include <vcpkg/base/parallel-algorithms.h>
#include <vcpkg/base/parse.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/system.debug.h>
#include <vcpkg/base/system.h>
#include <vcpkg/base/system.process.h>
#include <vcpkg/base/util.h>
#include <vcpkg/base/xmlserializer.h>

#include <vcpkg/archives.h>
#include <vcpkg/binarycaching.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/documentation.h>
#include <vcpkg/metrics.h>
#include <vcpkg/tools.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>

#include <memory>
#include <utility>

using namespace vcpkg;

namespace
{
    // The length of an ABI in the binary cache
    static constexpr size_t ABI_LENGTH = 64;

    FeedReference make_feedref(const PackageSpec& spec, const Version& version, StringView abi_tag, StringView prefix)
    {
        return {Strings::concat(prefix, spec.dir()), format_version_for_feedref(version.text, abi_tag)};
    }
    FeedReference make_feedref(const BinaryPackageReadInfo& info, StringView prefix)
    {
        return make_feedref(info.spec, info.version, info.package_abi, prefix);
    }

    bool clean_prepare_dir(DiagnosticContext& context, const Filesystem& fs, const Path& dir)
    {
        if (fs.remove_all(context, dir) && fs.create_directories(context, dir))
        {
            return true;
        }

        context.report(DiagnosticLine{DiagKind::Note, dir, msg::format(msgWhileClearingThis)});
        return false;
    }

#ifdef _WIN32
    bool directory_last_write_time(DiagnosticContext& context, const Filesystem& fs, const Path& dir)
    {
        auto now = fs.file_time_now();
        auto maybe_paths = fs.try_get_files_recursive(context, dir);
        if (auto paths = maybe_paths.get())
        {
            bool all_success = true;
            for (auto&& path : *paths)
            {
                if (!fs.last_write_time(context, path, now))
                {
                    all_success = false;
                }
            }

            if (all_success)
            {
                return true;
            }
        }

        return false;
    }
#endif // ^^^ _WIN32

    Path make_temp_archive_path(const Path& buildtrees, const PackageSpec& spec, const std::string& abi)
    {
        return buildtrees / fmt::format("{}_{}.zip", spec.name(), abi);
    }

    Path files_archive_parent_path(const std::string& abi) { return Path(abi.substr(0, 2)); }
    Path files_archive_subpath(const std::string& abi) { return files_archive_parent_path(abi) / (abi + ".zip"); }

    struct FilesWriteBinaryProvider : IWriteBinaryProvider
    {
        FilesWriteBinaryProvider(Path&& dir) : m_dir(std::move(dir)) { }

        bool push_success(DiagnosticContext& context,
                          const Filesystem& fs,
                          const BinaryPackageWriteInfo& request) override
        {
            const auto& zip_path = request.zip_path.value_or_exit(VCPKG_LINE_INFO);
            const auto archive_parent_path = m_dir / files_archive_parent_path(request.package_abi);
            fs.create_directories(archive_parent_path, IgnoreErrors{});
            const auto archive_path = archive_parent_path / (request.package_abi + ".zip");
            const auto archive_temp_path = Path(fmt::format("{}.{}", archive_path.native(), get_process_id()));
            std::error_code ec;
            if (request.unique_write_provider)
            {
                fs.rename_or_delete(zip_path, archive_path, ec);
            }

            if (!request.unique_write_provider || (ec && ec == std::make_error_condition(std::errc::cross_device_link)))
            {
                // either we need to make a copy or the rename failed because buildtrees and the binary
                // cache write target are on different filesystems, copy to a sibling in that directory and rename
                // into place
                // First copy to temporary location to avoid race between different vcpkg instances trying to upload
                // the same archive, e.g. if 2 machines try to upload to a shared binary cache.
                fs.copy_file(zip_path, archive_temp_path, CopyOptions::overwrite_existing, ec);
                if (!ec)
                {
                    fs.rename_or_delete(archive_temp_path, archive_path, ec);
                }
            }

            if (ec)
            {
                context.report(DiagnosticLine{DiagKind::Warning,
                                              msg::format(msgFailedToStoreBinaryCache, msg::path = archive_path)
                                                  .append_raw('\n')
                                                  .append_raw(ec.message())});
                return false;
            }

            return true;
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

    private:
        Path m_dir;
    };

    enum class RemoveWhen
    {
        nothing,
        always,
    };

    struct ZipResource
    {
        ZipResource(Path&& p, RemoveWhen t) : path(std::move(p)), to_remove(t) { }

        Path path;
        RemoveWhen to_remove;
    };

    // This middleware class contains logic for BinaryProviders that operate on zip files.
    // Derived classes must implement:
    // - acquire_zips()
    // - IReadBinaryProvider::precheck()
    struct ZipReadBinaryProvider : IReadBinaryProvider
    {
        ZipReadBinaryProvider(const ZipTool& zip) : m_zip(zip) { }

        struct UnzipJob
        {
            const Path* package_dir;
            const ZipResource* zip_resource;
            uint64_t zip_size;
            size_t action_idx;
            FullyBufferedDiagnosticContext fbdc;
            bool success = false;
        };

        void fetch(DiagnosticContext& context,
                   const Filesystem& fs,
                   View<const InstallPlanAction*> actions,
                   Span<RestoreResult> out_status) const override
        {
            const ElapsedTimer timer;
            std::vector<Optional<ZipResource>> zip_paths(actions.size(), nullopt);
            acquire_zips(context, fs, actions, zip_paths);
            std::vector<UnzipJob> jobs;
            jobs.reserve(actions.size());
            for (size_t i = 0; i < actions.size(); ++i)
            {
                if (auto zip_resource = zip_paths[i].get())
                {
                    jobs.push_back(
                        {&actions[i]->package_dir, zip_resource, fs.file_size(zip_resource->path, IgnoreErrors{}), i});
                }
            }

            std::sort(
                jobs.begin(), jobs.end(), [](const UnzipJob& l, const UnzipJob& r) { return l.zip_size > r.zip_size; });

            parallel_for_each(jobs, [this, &fs, &out_status](UnzipJob& job) {
                WarningDiagnosticContext wdc{job.fbdc};
                if (clean_prepare_dir(wdc, fs, *job.package_dir))
                {
                    auto cmd = m_zip.decompress_zip_archive_cmd(*job.package_dir, job.zip_resource->path);
                    auto maybe_output = cmd_execute_and_capture_output(wdc, cmd);
                    if (check_zero_exit_code(wdc, cmd, maybe_output)
#ifdef _WIN32
                        // On windows the ziptool does restore file times, we don't want that because this breaks file
                        // time based change detection.
                        && directory_last_write_time(wdc, fs, *job.package_dir)
#endif // ^^^ _WIN32
                    )
                    {
                        out_status[job.action_idx] = RestoreResult::restored;
                        job.success = true;
                    }
                    else
                    {
                        wdc.report(DiagnosticLine{
                            DiagKind::Note, job.zip_resource->path, msg::format(msgWhileExtractingThisArchive)});
                    }
                }

                if (job.zip_resource->to_remove == RemoveWhen::always)
                {
                    fs.remove(job.zip_resource->path, IgnoreErrors{});
                }
            });

            for (auto&& job : jobs)
            {
                job.fbdc.print_to(out_sink);
                if (Debug::g_debugging && job.success)
                {
                    console_diagnostic_context.report(
                        DiagnosticLine{DiagKind::Note,
                                       job.zip_resource->path,
                                       msg::format(msgExtractedInto, msg::path = *job.package_dir)});
                }
            }
        }

        // For every action denoted by actions, at corresponding indicies in out_zips, stores a ZipResource indicating
        // the downloaded location.
        //
        // Leaving an Optional disengaged indicates that the cache does not contain the requested zip.
        //
        // Note that as this API can't fail, only warnings or lower will be emitted to `context`.
        virtual void acquire_zips(DiagnosticContext& context,
                                  const Filesystem& fs,
                                  View<const InstallPlanAction*> actions,
                                  Span<Optional<ZipResource>> out_zips) const = 0;

    protected:
        ZipTool m_zip;
    };

    struct FilesReadBinaryProvider : ZipReadBinaryProvider
    {
        FilesReadBinaryProvider(const ZipTool& zip, Path&& dir) : ZipReadBinaryProvider(zip), m_dir(std::move(dir)) { }

        void acquire_zips(DiagnosticContext&,
                          const Filesystem& fs,
                          View<const InstallPlanAction*> actions,
                          Span<Optional<ZipResource>> out_zip_paths) const override
        {
            for (size_t i = 0; i < actions.size(); ++i)
            {
                const auto& abi_tag = actions[i]->package_abi_or_exit(VCPKG_LINE_INFO);
                auto archive_path = m_dir / files_archive_subpath(abi_tag);
                if (fs.exists(archive_path, IgnoreErrors{}))
                {
                    out_zip_paths[i].emplace(std::move(archive_path), RemoveWhen::nothing);
                }
            }
        }

        void precheck(DiagnosticContext&,
                      const Filesystem& fs,
                      View<const InstallPlanAction*> actions,
                      Span<CacheAvailability> cache_status) const override
        {
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                const auto& action = *actions[idx];
                const auto& abi_tag = action.package_abi_or_exit(VCPKG_LINE_INFO);

                bool any_available = false;
                if (fs.exists(m_dir / files_archive_subpath(abi_tag), IgnoreErrors{}))
                {
                    any_available = true;
                }

                cache_status[idx] = any_available ? CacheAvailability::available : CacheAvailability::unavailable;
            }
        }
        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromFiles,
                               msg::count = count,
                               msg::elapsed = ElapsedTime(elapsed),
                               msg::path = m_dir);
        }

    private:
        Path m_dir;
    };

    struct HTTPPutBinaryProvider : IWriteBinaryProvider
    {
        HTTPPutBinaryProvider(UrlTemplate&& url, const std::vector<std::string>& secrets)
            : m_url(std::move(url)), m_secrets(secrets)
        {
        }

        bool push_success(DiagnosticContext& context, const Filesystem&, const BinaryPackageWriteInfo& request) override
        {
            if (!request.zip_path) return false;
            const auto& zip_path = *request.zip_path.get();
            auto url = m_url.instantiate_variables(request);
            WarningDiagnosticContext wdc{context};
            return store_to_asset_cache(wdc, url, SanitizedUrl{url, m_secrets}, m_url.headers, zip_path);
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

    private:
        UrlTemplate m_url;
        std::vector<std::string> m_secrets;
    };

    struct HttpGetBinaryProvider : ZipReadBinaryProvider
    {
        HttpGetBinaryProvider(ZipTool zip,
                              const Path& buildtrees,
                              UrlTemplate&& url_template,
                              const std::vector<std::string>& secrets)
            : ZipReadBinaryProvider(std::move(zip))
            , m_buildtrees(buildtrees)
            , m_url_template(std::move(url_template))
            , m_secrets(secrets)
        {
        }

        void acquire_zips(DiagnosticContext& context,
                          const Filesystem&,
                          View<const InstallPlanAction*> actions,
                          Span<Optional<ZipResource>> out_zip_paths) const override
        {
            std::vector<std::pair<std::string, Path>> url_paths;
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                auto&& action = *actions[idx];
                auto read_info = BinaryPackageReadInfo{action};
                url_paths.emplace_back(m_url_template.instantiate_variables(read_info),
                                       make_temp_archive_path(m_buildtrees, read_info.spec, read_info.package_abi));
            }

            WarningDiagnosticContext wdc{context};
            auto codes = download_files_no_cache(wdc, url_paths, m_url_template.headers);
            for (size_t i = 0; i < codes.size(); ++i)
            {
                if (codes[i] == 200)
                {
                    out_zip_paths[i].emplace(std::move(url_paths[i].second), RemoveWhen::always);
                }
            }
        }

        void precheck(DiagnosticContext& context,
                      const Filesystem&,
                      View<const InstallPlanAction*> actions,
                      Span<CacheAvailability> out_status) const override
        {
            std::vector<std::string> urls;
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                urls.push_back(m_url_template.instantiate_variables(BinaryPackageReadInfo{*actions[idx]}));
            }

            WarningDiagnosticContext wdc{context};
            auto codes = url_heads(wdc, urls, {});
            for (size_t i = 0; i < codes.size(); ++i)
            {
                out_status[i] = codes[i] == 200 ? CacheAvailability::available : CacheAvailability::unavailable;
            }

            for (size_t i = codes.size(); i < out_status.size(); ++i)
            {
                out_status[i] = CacheAvailability::unavailable;
            }
        }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromHTTP, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        Path m_buildtrees;
        UrlTemplate m_url_template;
        std::vector<std::string> m_secrets;
    };

    struct AzureBlobPutBinaryProvider : IWriteBinaryProvider
    {
        AzureBlobPutBinaryProvider(UrlTemplate&& url, const std::vector<std::string>& secrets)
            : m_url(std::move(url)), m_secrets(secrets)
        {
        }

        bool push_success(DiagnosticContext& context,
                          const Filesystem& fs,
                          const BinaryPackageWriteInfo& request) override
        {
            if (!request.zip_path) return false;

            const auto& zip_path = *request.zip_path.get();

            const auto file_size = fs.file_size(zip_path, VCPKG_LINE_INFO);
            if (file_size == 0) return false;

            // cf.
            // https://learn.microsoft.com/en-us/rest/api/storageservices/understanding-block-blobs--append-blobs--and-page-blobs?toc=%2Fazure%2Fstorage%2Fblobs%2Ftoc.json
            constexpr size_t max_single_write = 5000000000u;
            bool use_azcopy = file_size > max_single_write;

            WarningDiagnosticContext wdc{context};
            auto url = m_url.instantiate_variables(request);
            return use_azcopy ? azcopy_to_asset_cache(wdc, url, SanitizedUrl{url, m_secrets}, zip_path)
                              : store_to_asset_cache(wdc, url, SanitizedUrl{url, m_secrets}, m_url.headers, zip_path);
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

    private:
        UrlTemplate m_url;
        std::vector<std::string> m_secrets;
    };

    struct NuGetSource
    {
        StringLiteral option;
        std::string value;
    };

    NuGetSource nuget_sources_arg(View<std::string> sources) { return {"-Source", Strings::join(";", sources)}; }
    NuGetSource nuget_configfile_arg(const Path& config_path) { return {"-ConfigFile", config_path.native()}; }

    struct NuGetToolTools
    {
        Path nuget_tool;
#ifndef _WIN32
        Path mono_tool;
#endif
    };

    Optional<NuGetToolTools> get_nuget_tool_tools(DiagnosticContext& context,
                                                  const Filesystem& fs,
                                                  const ToolCache& cache)
    {
        if (auto nuget_tool = cache.get_tool_path(context, fs, Tools::NUGET))
        {
#ifdef _WIN32
            return NuGetToolTools{*nuget_tool};
#else
            if (auto mono_tool = cache.get_tool_path(context, fs, Tools::MONO))
            {
                return NuGetToolTools{*nuget_tool, *mono_tool};
            }
#endif
        }

        return nullopt;
    }

    struct NuGetTool
    {
        NuGetTool(NuGetToolTools&& nuget_tools, long timeout, bool interactive, bool use_nuget_cache)
            : m_timeout(std::to_string(timeout)), m_interactive(interactive), m_use_nuget_cache(use_nuget_cache)
        {
#ifndef _WIN32
            m_cmd.string_arg(std::move(nuget_tools.mono_tool));
#endif
            m_cmd.string_arg(std::move(nuget_tools.nuget_tool));
        }

        bool push(DiagnosticContext& context, const Path& nupkg_path, const NuGetSource& src) const
        {
            if (run_nuget_commandline(context, push_cmd(nupkg_path, src)))
            {
                return true;
            }

            context.report(DiagnosticLine{DiagKind::Note, msg::format(msgWhilePushingNuGetPackage)});
            return false;
        }
        bool pack(DiagnosticContext& context, const Path& nuspec_path, const Path& out_dir) const
        {
            if (run_nuget_commandline(context, pack_cmd(nuspec_path, out_dir)))
            {
                return true;
            }

            context.report(DiagnosticLine{DiagKind::Note, msg::format(msgWhilePackingNuGetPackage)});
            return false;
        }
        bool install(DiagnosticContext& context,
                     StringView packages_config,
                     const Path& out_dir,
                     const NuGetSource& src) const
        {
            return run_nuget_commandline(context, install_cmd(packages_config, out_dir, src));
        }

    private:
        Command subcommand(StringLiteral sub) const
        {
            auto cmd = m_cmd;
            cmd.string_arg(sub).string_arg("-ForceEnglishOutput").string_arg("-Verbosity").string_arg("detailed");
            if (!m_interactive) cmd.string_arg("-NonInteractive");
            return cmd;
        }

        Command install_cmd(StringView packages_config, const Path& out_dir, const NuGetSource& src) const
        {
            auto cmd = subcommand("install");
            cmd.string_arg(packages_config)
                .string_arg("-OutputDirectory")
                .string_arg(out_dir)
                .string_arg("-ExcludeVersion")
                .string_arg("-PreRelease")
                .string_arg("-PackageSaveMode")
                .string_arg("nupkg");
            if (!m_use_nuget_cache) cmd.string_arg("-DirectDownload").string_arg("-NoHttpCache");
            cmd.string_arg(src.option).string_arg(src.value);
            return cmd;
        }

        Command pack_cmd(const Path& nuspec_path, const Path& out_dir) const
        {
            return subcommand("pack")
                .string_arg(nuspec_path)
                .string_arg("-OutputDirectory")
                .string_arg(out_dir)
                .string_arg("-NoDefaultExcludes");
        }

        Command push_cmd(const Path& nupkg_path, const NuGetSource& src) const
        {
            return subcommand("push")
                .string_arg(nupkg_path)
                .string_arg("-Timeout")
                .string_arg(m_timeout)
                .string_arg(src.option)
                .string_arg(src.value);
        }

        bool run_nuget_commandline(DiagnosticContext& context, const Command& cmd) const
        {
            if (m_interactive)
            {
                // note that this must cmd_execute not cmd_execute_and_capture_output because we need
                // our console, stdin, stdout, and stderr to be inherited directly by the interactive
                // nuget process.
                auto maybe_exit_code = cmd_execute(context, cmd);
                if (check_zero_exit_code(context, cmd, maybe_exit_code))
                {
                    return true;
                }

                context.report(
                    DiagnosticLine{DiagKind::Note, msg::format(msgNuGetOutputNotCapturedBecauseInteractiveSpecified)});
                return false;
            }

            RedirectedProcessLaunchSettings settings;
            settings.echo_in_debug = EchoInDebug::Show;
            AttemptDiagnosticContext adc{context};
            auto maybe_code_and_output = cmd_execute_and_capture_output(adc, cmd, settings);
            if (auto code_and_output = maybe_code_and_output.get())
            {
                if (code_and_output->exit_code == 0)
                {
                    adc.commit();
                    return true;
                }

                // NuGet is extremely chatty in its console output so we look for some failures we know about and
                // avoid printing the whole console output in such cases.
                if (code_and_output->output.find("Authentication may require manual action.") != std::string::npos)
                {
                    adc.commit();
                    report_nonzero_exit_code(context, cmd, code_and_output->exit_code);
                    context.report(
                        DiagnosticLine{DiagKind::Note, msg::format(msgNuGetAuthenticationMayRequireManualAction)});
                    return false;
                }

                if (code_and_output->output.find(
                        "Response status code does not indicate success: 401 (Unauthorized)") != std::string::npos)
                {
                    adc.commit();
                    report_nonzero_exit_code(context, cmd, code_and_output->exit_code);
                    context.report(DiagnosticLine{DiagKind::Note,
                                                  msg::format(msgFailedVendorAuthentication,
                                                              msg::vendor = "NuGet",
                                                              msg::url = docs::troubleshoot_binary_cache_url)});
                    return false;
                }

                if (code_and_output->output.find("for example \"-ApiKey AzureDevOps\"") != std::string::npos)
                {
                    AttemptDiagnosticContext retry_adc{context};
                    auto retry_cmd = cmd;
                    retry_cmd.string_arg("-ApiKey").string_arg("AzureDevOps");
                    auto maybe_retry_code_and_output = cmd_execute_and_capture_output(retry_adc, retry_cmd, settings);
                    if (check_zero_exit_code(retry_adc, retry_cmd, maybe_retry_code_and_output, settings.echo_in_debug))
                    {
                        adc.handle();
                        retry_adc.commit();
                        return true;
                    }

                    adc.commit();
                    // we only print the whole console output from the retry
                    report_nonzero_exit_code(context, cmd, code_and_output->exit_code);
                    retry_adc.commit();
                    return false;
                }

                adc.commit();
                report_nonzero_exit_code_and_output(context, cmd, *code_and_output, settings.echo_in_debug);
            }

            return false;
        }

        Command m_cmd;
        std::string m_timeout;
        bool m_interactive;
        bool m_use_nuget_cache;
    };

    struct NugetBaseBinaryProvider
    {
        NugetBaseBinaryProvider(const NuGetTool& tool,
                                const Path& packages,
                                const Path& buildtrees,
                                StringView nuget_prefix)
            : m_cmd(tool), m_packages(packages), m_buildtrees(buildtrees), m_nuget_prefix(nuget_prefix.to_string())
        {
        }

        NuGetTool m_cmd;
        Path m_packages;
        Path m_buildtrees;
        std::string m_nuget_prefix;
    };

    struct NugetReadBinaryProvider : IReadBinaryProvider, private NugetBaseBinaryProvider
    {
        NugetReadBinaryProvider(const NugetBaseBinaryProvider& base, NuGetSource src)
            : NugetBaseBinaryProvider(base), m_src(std::move(src))
        {
        }

        NuGetSource m_src;

        static std::string generate_packages_config(View<FeedReference> refs)
        {
            XmlSerializer xml;
            xml.emit_declaration().line_break();
            xml.open_tag("packages").line_break();

            for (auto&& ref : refs)
            {
                xml.start_complex_open_tag("package")
                    .text_attr("id", ref.id)
                    .text_attr("version", ref.version)
                    .finish_self_closing_complex_tag()
                    .line_break();
            }

            xml.close_tag("packages").line_break();
            return std::move(xml.buf);
        }

        // Prechecking is too expensive with NuGet, so it is not implemented
        void precheck(DiagnosticContext&,
                      const Filesystem&,
                      View<const InstallPlanAction*>,
                      Span<CacheAvailability>) const override
        {
        }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromNuGet, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        void fetch(DiagnosticContext& context,
                   const Filesystem& fs,
                   View<const InstallPlanAction*> actions,
                   Span<RestoreResult> out_status) const override
        {
            auto packages_config = m_buildtrees / "packages.config";
            auto refs =
                Util::fmap(actions, [this](const InstallPlanAction* p) { return make_nugetref(*p, m_nuget_prefix); });
            WarningDiagnosticContext wdc{context};
            if (!fs.write_contents(wdc, packages_config, generate_packages_config(refs)))
            {
                return;
            }

            (void)m_cmd.install(wdc, packages_config, m_packages, m_src);
            for (size_t i = 0; i < actions.size(); ++i)
            {
                // nuget.exe provides the nupkg file and the unpacked folder
                const auto nupkg_path = m_packages / refs[i].id / refs[i].id + ".nupkg";
                if (fs.exists(nupkg_path, IgnoreErrors{}))
                {
                    (void)fs.remove(wdc, nupkg_path);
                    const auto nuget_dir = actions[i]->spec.dir();
                    if (nuget_dir == refs[i].id)
                    {
                        out_status[i] = RestoreResult::restored;
                    }
                    else
                    {
                        const auto path_from = m_packages / refs[i].id;
                        const auto path_to = m_packages / nuget_dir;
                        if (fs.rename(wdc, path_from, path_to))
                        {
                            out_status[i] = RestoreResult::restored;
                        }
                    }
                }
            }
        }
    };

    struct NugetBinaryPushProvider : IWriteBinaryProvider, private NugetBaseBinaryProvider
    {
        NugetBinaryPushProvider(const NugetBaseBinaryProvider& base, NuGetSource src)
            : NugetBaseBinaryProvider(base), m_src(std::move(src))
        {
        }

        NuGetSource m_src;

        bool needs_nuspec_data() const override { return true; }
        bool needs_zip_file() const override { return false; }

        bool push_success(DiagnosticContext& context,
                          const Filesystem& fs,
                          const BinaryPackageWriteInfo& request) override
        {
            auto& spec = request.spec;
            auto nuspec_path = m_buildtrees / spec.name() / spec.triplet().canonical_name() + ".nuspec";
            auto& nuspec_contents = request.nuspec.value_or_exit(VCPKG_LINE_INFO);
            std::error_code ec;
            fs.write_contents(nuspec_path, nuspec_contents, ec);
            if (ec)
            {
                context.report_error(
                    format_filesystem_call_error(ec, "write_contents", {nuspec_path, nuspec_contents}));
                context.report(DiagnosticLine{DiagKind::Note, msg::format(msgWhilePackingNuGetPackage)});
                return false;
            }

            auto pack_result = m_cmd.pack(context, nuspec_path, m_buildtrees);
            fs.remove(nuspec_path, IgnoreErrors{});
            if (!pack_result)
            {
                return false;
            }

            auto nupkg_path = m_buildtrees / make_feedref(request, m_nuget_prefix).nupkg_filename();
            const auto vendor = m_src.option == "-ConfigFile" ? "NuGet config" : "NuGet";
            context.statusln(msg::format(msgUploadingBinariesToVendor,
                                         msg::spec = request.display_name,
                                         msg::vendor = vendor,
                                         msg::path = m_src.value));
            const auto stored = m_cmd.push(context, nupkg_path, m_src);

            fs.remove(nupkg_path, IgnoreErrors{});
            return stored;
        }
    };

    struct IObjectStorageTool
    {
        virtual ~IObjectStorageTool() = default;

        virtual LocalizedString restored_message(size_t count,
                                                 std::chrono::high_resolution_clock::duration elapsed) const = 0;
        virtual Optional<CacheAvailability> stat(DiagnosticContext& context, StringView url) const = 0;
        virtual Optional<RestoreResult> download_file(DiagnosticContext& context,
                                                      StringView object,
                                                      const Path& archive) const = 0;
        virtual bool upload_file(DiagnosticContext& context, StringView object, const Path& archive) const = 0;
    };

    struct ObjectStorageProvider : ZipReadBinaryProvider
    {
        ObjectStorageProvider(const ZipTool& zip,
                              const Path& buildtrees,
                              std::string&& prefix,
                              const std::shared_ptr<const IObjectStorageTool>& tool)
            : ZipReadBinaryProvider(zip), m_buildtrees(buildtrees), m_prefix(std::move(prefix)), m_tool(tool)
        {
        }

        static std::string make_object_path(const std::string& prefix, const std::string& abi)
        {
            return Strings::concat(prefix, abi, ".zip");
        }

        void acquire_zips(DiagnosticContext& context,
                          const Filesystem&,
                          View<const InstallPlanAction*> actions,
                          Span<Optional<ZipResource>> out_zip_paths) const override
        {
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                auto&& action = *actions[idx];
                const auto& abi = action.package_abi_or_exit(VCPKG_LINE_INFO);
                auto tmp = make_temp_archive_path(m_buildtrees, action.spec, abi);
                WarningDiagnosticContext wdc{context};
                auto res = m_tool->download_file(wdc, make_object_path(m_prefix, abi), tmp);
                if (auto cache_result = res.get())
                {
                    if (*cache_result == RestoreResult::restored)
                    {
                        out_zip_paths[idx].emplace(std::move(tmp), RemoveWhen::always);
                    }
                }
            }
        }

        void precheck(DiagnosticContext& context,
                      const Filesystem&,
                      View<const InstallPlanAction*> actions,
                      Span<CacheAvailability> cache_status) const override
        {
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                auto&& action = *actions[idx];
                const auto& abi = action.package_abi_or_exit(VCPKG_LINE_INFO);
                WarningDiagnosticContext wdc{context};
                auto maybe_res = m_tool->stat(wdc, make_object_path(m_prefix, abi));
                if (auto res = maybe_res.get())
                {
                    cache_status[idx] = *res;
                }
                else
                {
                    cache_status[idx] = CacheAvailability::unavailable;
                }
            }
        }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return m_tool->restored_message(count, elapsed);
        }

        Path m_buildtrees;
        std::string m_prefix;
        std::shared_ptr<const IObjectStorageTool> m_tool;
    };
    struct ObjectStoragePushProvider : IWriteBinaryProvider
    {
        ObjectStoragePushProvider(std::string&& prefix, std::shared_ptr<const IObjectStorageTool> tool)
            : m_prefix(std::move(prefix)), m_tool(std::move(tool))
        {
        }

        static std::string make_object_path(const std::string& prefix, const std::string& abi)
        {
            return Strings::concat(prefix, abi, ".zip");
        }

        bool push_success(DiagnosticContext& context, const Filesystem&, const BinaryPackageWriteInfo& request) override
        {
            if (auto zip_path = request.zip_path.get())
            {
                WarningDiagnosticContext wdc{context};
                return m_tool->upload_file(wdc, make_object_path(m_prefix, request.package_abi), *zip_path);
            }

            return false;
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

        std::string m_prefix;
        std::shared_ptr<const IObjectStorageTool> m_tool;
    };

    struct AzCopyStorageProvider : ZipReadBinaryProvider
    {
        AzCopyStorageProvider(const ZipTool& zip, const Path& buildtrees, AzCopyUrl&& az_url, const Path& tool)
            : ZipReadBinaryProvider(zip), m_buildtrees(buildtrees), m_url(std::move(az_url)), m_tool(tool)
        {
        }

        // Batch the azcopy arguments to fit within the maximum allowed command line length.
        static std::vector<std::vector<std::string>> batch_azcopy_args(const std::vector<std::string>& abis,
                                                                       const size_t reserved_len)
        {
            return batch_command_arguments_with_fixed_length(abis,
                                                             reserved_len,
                                                             Command::maximum_allowed,
                                                             ABI_LENGTH + 4, // ABI_LENGTH for SHA256 + 4 for ".zip"
                                                             1);             // the separator length is 1 for ';'
        }

        Optional<std::vector<std::string>> azcopy_list(DiagnosticContext& context) const
        {
            std::vector<std::string> abis;
            auto cmd = Command{m_tool}
                           .string_arg("list")
                           .string_arg("--output-level")
                           .string_arg("ESSENTIAL")
                           .string_arg(m_url.make_container_path());
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (auto output = check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                for (const auto& line : Strings::split(*output, '\n'))
                {
                    if (line.empty()) continue;
                    // `azcopy list` output uses format `<filename>; Content Length: <size>`, we only need the filename
                    auto first_part_end = std::find(line.begin(), line.end(), ';');
                    if (first_part_end != line.end())
                    {
                        std::string abifile{line.begin(), first_part_end};

                        // Check file names with the format `<abi>.zip`
                        if (abifile.size() == ABI_LENGTH + 4 &&
                            std::all_of(abifile.begin(), abifile.begin() + ABI_LENGTH, ParserBase::is_hex_digit) &&
                            abifile.substr(ABI_LENGTH) == ".zip")
                        {
                            // remove ".zip" extension
                            abis.emplace_back(abifile.substr(0, abifile.size() - 4));
                        }
                    }
                }
            }

            return abis;
        }

        void acquire_zips(DiagnosticContext& context,
                          const Filesystem& fs,
                          View<const InstallPlanAction*> actions,
                          Span<Optional<ZipResource>> out_zip_paths) const override
        {
            WarningDiagnosticContext wdc{context};
            std::vector<std::string> abis;
            std::map<std::string, size_t> abi_index_map;
            for (size_t idx = 0; idx < actions.size(); ++idx)
            {
                auto&& action = *actions[idx];
                const auto& abi = action.package_abi_or_exit(VCPKG_LINE_INFO);
                abis.push_back(abi);
                abi_index_map[abi] = idx;
            }

            const auto tmp_downloads_location = m_buildtrees / ".azcopy";
            auto base_cmd = Command{m_tool}
                                .string_arg("copy")
                                .string_arg("--from-to")
                                .string_arg("BlobLocal")
                                .string_arg("--output-level")
                                .string_arg("QUIET")
                                .string_arg("--overwrite")
                                .string_arg("true")
                                .string_arg(m_url.make_container_path())
                                .string_arg(tmp_downloads_location)
                                .string_arg("--include-path");

            const size_t reserved_len =
                base_cmd.command_line().size() + 4; // for space + surrounding quotes + terminator
            for (auto&& batch : batch_azcopy_args(abis, reserved_len))
            {
                auto cmd = Command{base_cmd}.string_arg(
                    Strings::join(";", Util::fmap(batch, [](const auto& abi) { return abi + ".zip"; })));
                // note that we don't check for zero exit code because azcopy returns nonzero exit codes
                // when any individual download fails, as we expect for cache misses
                (void)cmd_execute_and_capture_output(wdc, cmd);
            }

            const auto& container_url = m_url.url;
            const auto last_slash = std::find(container_url.rbegin(), container_url.rend(), '/');
            const auto container_name = std::string{last_slash.base(), container_url.end()};
            auto maybe_files = fs.try_get_files_non_recursive(wdc, tmp_downloads_location / container_name);
            if (auto files = maybe_files.get())
            {
                for (auto&& file : *files)
                {
                    auto filename = file.stem().to_string();
                    auto it = abi_index_map.find(filename);
                    if (it != abi_index_map.end())
                    {
                        out_zip_paths[it->second].emplace(std::move(file), RemoveWhen::always);
                    }
                }
            }
        }

        void precheck(DiagnosticContext& context,
                      const Filesystem&,
                      View<const InstallPlanAction*> actions,
                      Span<CacheAvailability> cache_status) const override
        {
            WarningDiagnosticContext wdc{context};
            auto maybe_abis = azcopy_list(wdc);
            if (auto abis = maybe_abis.get())
            {
                for (size_t idx = 0; idx < actions.size(); ++idx)
                {
                    auto&& action = *actions[idx];
                    const auto& abi = action.package_abi_or_exit(VCPKG_LINE_INFO);
                    cache_status[idx] =
                        Util::contains(*abis, abi) ? CacheAvailability::available : CacheAvailability::unavailable;
                }
            }
            else
            {
                // If the command failed, we assume that the cache is unavailable.
                std::fill(cache_status.begin(), cache_status.end(), CacheAvailability::unavailable);
            }
        }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(
                msgRestoredPackagesFromAzureStorage, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        Path m_buildtrees;
        AzCopyUrl m_url;
        Path m_tool;
    };
    struct AzCopyStoragePushProvider : IWriteBinaryProvider
    {
        AzCopyStoragePushProvider(AzCopyUrl&& container, const Path& tool)
            : m_container(std::move(container)), m_tool(tool)
        {
        }

        bool upload_file(DiagnosticContext& context, StringView url, const Path& archive) const
        {
            auto cmd = Command{m_tool}
                           .string_arg("copy")
                           .string_arg("--from-to")
                           .string_arg("LocalBlob")
                           .string_arg("--overwrite")
                           .string_arg("true")
                           .string_arg(archive)
                           .string_arg(url);

            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            return check_zero_exit_code(context, cmd, maybe_code_and_output);
        }

        bool push_success(DiagnosticContext& context, const Filesystem&, const BinaryPackageWriteInfo& request) override
        {
            const auto& zip_path = request.zip_path.value_or_exit(VCPKG_LINE_INFO);
            WarningDiagnosticContext wdc{context};
            return upload_file(wdc, m_container.make_object_path(request.package_abi), zip_path);
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

        AzCopyUrl m_container;
        Path m_tool;
    };

    struct GcsStorageTool : IObjectStorageTool
    {
        GcsStorageTool(const Path& tool) : m_tool(tool) { }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromGCS, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        Optional<CacheAvailability> stat(DiagnosticContext& context, StringView url) const override
        {
            auto cmd = Command{m_tool}.string_arg("-q").string_arg("stat").string_arg(url);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                return CacheAvailability::available;
            }

            return nullopt;
        }

        Optional<RestoreResult> download_file(DiagnosticContext& context,
                                              StringView object,
                                              const Path& archive) const override
        {
            auto cmd = Command{m_tool}.string_arg("-q").string_arg("cp").string_arg(object).string_arg(archive);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                return RestoreResult::restored;
            }

            return nullopt;
        }

        bool upload_file(DiagnosticContext& context, StringView object, const Path& archive) const override
        {
            auto cmd = Command{m_tool}.string_arg("-q").string_arg("cp").string_arg(archive).string_arg(object);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            return check_zero_exit_code(context, cmd, maybe_code_and_output);
        }

        Path m_tool;
    };

    struct AwsStorageTool : IObjectStorageTool
    {
        AwsStorageTool(const Path& tool, bool no_sign_request) : m_tool(tool), m_no_sign_request(no_sign_request) { }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromAWS, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        Optional<CacheAvailability> stat(DiagnosticContext& context, StringView url) const override
        {
            auto cmd = Command{m_tool}.string_arg("s3").string_arg("ls").string_arg(url);
            if (m_no_sign_request)
            {
                cmd.string_arg("--no-sign-request");
            }

            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            // When the file is not found, "aws s3 ls" prints nothing, and returns exit code 1.
            // check_zero_exit_code would treat this as an error, but we want to treat it as a (silent)
            // cache miss instead, so we handle this special case. See
            // https://github.com/aws/aws-cli/issues/5544 for the related aws-cli bug report.
            if (auto code_and_output = maybe_code_and_output.get())
            {
                // We want to return CacheAvailability::unavailable even if aws-cli starts to return exit code 0
                // with an empty output when the file is missing. This way, both the current and possible future
                // behavior of aws-cli is covered.
                if (code_and_output->exit_code == 0 || code_and_output->exit_code == 1)
                {
                    if (Strings::trim(code_and_output->output).empty())
                    {
                        return CacheAvailability::unavailable;
                    }
                }

                if (code_and_output->exit_code == 0)
                {
                    return CacheAvailability::available;
                }

                report_nonzero_exit_code_and_output(context, cmd, *code_and_output);
            }

            return nullopt;
        }

        Optional<RestoreResult> download_file(DiagnosticContext& context,
                                              StringView object,
                                              const Path& archive) const override
        {
            auto r = stat(context, object);
            if (auto stat_result = r.get())
            {
                if (*stat_result != CacheAvailability::available)
                {
                    return RestoreResult::unavailable;
                }
            }
            else
            {
                return nullopt;
            }

            auto cmd = Command{m_tool}.string_arg("s3").string_arg("cp").string_arg(object).string_arg(archive);
            if (m_no_sign_request)
            {
                cmd.string_arg("--no-sign-request");
            }

            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                return RestoreResult::restored;
            }

            return nullopt;
        }

        bool upload_file(DiagnosticContext& context, StringView object, const Path& archive) const override
        {
            auto cmd = Command{m_tool}.string_arg("s3").string_arg("cp").string_arg(archive).string_arg(object);
            if (m_no_sign_request)
            {
                cmd.string_arg("--no-sign-request");
            }

            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            return check_zero_exit_code(context, cmd, maybe_code_and_output);
        }

        Path m_tool;
        bool m_no_sign_request;
    };

    struct CosStorageTool : IObjectStorageTool
    {
        CosStorageTool(const Path& tool) : m_tool(tool) { }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromCOS, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        Optional<CacheAvailability> stat(DiagnosticContext& context, StringView url) const override
        {
            auto cmd = Command{m_tool}.string_arg("ls").string_arg(url);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                return CacheAvailability::available;
            }

            return nullopt;
        }

        Optional<RestoreResult> download_file(DiagnosticContext& context,
                                              StringView object,
                                              const Path& archive) const override
        {
            auto cmd = Command{m_tool}.string_arg("cp").string_arg(object).string_arg(archive);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            if (check_zero_exit_code(context, cmd, maybe_code_and_output))
            {
                return RestoreResult::restored;
            }

            return nullopt;
        }

        bool upload_file(DiagnosticContext& context, StringView object, const Path& archive) const override
        {
            auto cmd = Command{m_tool}.string_arg("cp").string_arg(archive).string_arg(object);
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd);
            return check_zero_exit_code(context, cmd, maybe_code_and_output);
        }

        Path m_tool;
    };

    struct AzureUpkgTool
    {
        AzureUpkgTool(const Path& tool_path) : az_cli(tool_path) { }

        Command base_cmd(const AzureUpkgSource& src,
                         StringView package_name,
                         StringView package_version,
                         StringView verb) const
        {
            Command cmd{az_cli};
            cmd.string_arg("artifacts")
                .string_arg("universal")
                .string_arg(verb)
                .string_arg("--organization")
                .string_arg(src.organization)
                .string_arg("--feed")
                .string_arg(src.feed)
                .string_arg("--name")
                .string_arg(package_name)
                .string_arg("--version")
                .string_arg(package_version);
            if (!src.project.empty())
            {
                cmd.string_arg("--project").string_arg(src.project).string_arg("--scope").string_arg("project");
            }
            return cmd;
        }

        bool download(DiagnosticContext& context,
                      const AzureUpkgSource& src,
                      StringView package_name,
                      StringView package_version,
                      const Path& download_path) const
        {
            Command cmd = base_cmd(src, package_name, package_version, "download");
            cmd.string_arg("--path").string_arg(download_path);
            return run_az_artifacts_cmd(context, cmd);
        }

        bool publish(DiagnosticContext& context,
                     const AzureUpkgSource& src,
                     StringView package_name,
                     StringView package_version,
                     const Path& zip_path,
                     StringView description) const
        {
            Command cmd = base_cmd(src, package_name, package_version, "publish");
            cmd.string_arg("--description").string_arg(description).string_arg("--path").string_arg(zip_path);
            return run_az_artifacts_cmd(context, cmd);
        }

        bool run_az_artifacts_cmd(DiagnosticContext& context, const Command& cmd) const
        {
            RedirectedProcessLaunchSettings show_in_debug_settings;
            show_in_debug_settings.echo_in_debug = EchoInDebug::Show;
            auto maybe_code_and_output = cmd_execute_and_capture_output(context, cmd, show_in_debug_settings);
            if (auto code_and_output = maybe_code_and_output.get())
            {
                if (code_and_output->exit_code == 0)
                {
                    return true;
                }

                report_nonzero_exit_code_and_output(context, cmd, *code_and_output);
                // az command line error message: Before you can run Azure DevOps commands, you need to
                // run the login command(az login if using AAD/MSA identity else az devops login if using PAT
                // token) to setup credentials.
                if (code_and_output->output.find("you need to run the login command") != std::string::npos)
                {
                    context.report(DiagnosticLine{
                        DiagKind::Error,
                        msg::format(msgFailedVendorAuthentication,
                                    msg::vendor = "Universal Packages",
                                    msg::url = "https://learn.microsoft.com/cli/azure/authenticate-azure-cli")});
                }
            }

            return false;
        }

        Path az_cli;
    };

    struct AzureUpkgPutBinaryProvider : public IWriteBinaryProvider
    {
        AzureUpkgPutBinaryProvider(const Path& tool_path, AzureUpkgSource&& source)
            : m_azure_tool(tool_path), m_source(std::move(source))
        {
        }

        bool push_success(DiagnosticContext& context, const Filesystem&, const BinaryPackageWriteInfo& request) override
        {
            auto ref = make_feedref(request, "");
            std::string package_description = "Cached package for " + ref.id;

            const Path& zip_path = request.zip_path.value_or_exit(VCPKG_LINE_INFO);
            WarningDiagnosticContext wdc{context};
            return m_azure_tool.publish(wdc, m_source, ref.id, ref.version, zip_path, package_description);
        }

        bool needs_nuspec_data() const override { return false; }
        bool needs_zip_file() const override { return true; }

    private:
        AzureUpkgTool m_azure_tool;
        AzureUpkgSource m_source;
    };

    struct AzureUpkgGetBinaryProvider : public ZipReadBinaryProvider
    {
        AzureUpkgGetBinaryProvider(const ZipTool& zip,
                                   const Path& azcli_path,
                                   AzureUpkgSource&& source,
                                   const Path& buildtrees)
            : ZipReadBinaryProvider(zip)
            , m_azure_tool(azcli_path)
            , m_source(std::move(source))
            , m_buildtrees(buildtrees)
        {
        }

        // Prechecking doesn't exist with universal packages so it's not implemented
        void precheck(DiagnosticContext&,
                      const Filesystem&,
                      View<const InstallPlanAction*>,
                      Span<CacheAvailability>) const override
        {
        }

        LocalizedString restored_message(size_t count,
                                         std::chrono::high_resolution_clock::duration elapsed) const override
        {
            return msg::format(msgRestoredPackagesFromAZUPKG, msg::count = count, msg::elapsed = ElapsedTime(elapsed));
        }

        void acquire_zips(DiagnosticContext& context,
                          const Filesystem& fs,
                          View<const InstallPlanAction*> actions,
                          Span<Optional<ZipResource>> out_zips) const override
        {
            WarningDiagnosticContext wdc{context};
            for (size_t i = 0; i < actions.size(); ++i)
            {
                const auto& action = *actions[i];
                const auto info = BinaryPackageReadInfo{action};
                const auto ref = make_feedref(info, "");

                Path temp_dir = m_buildtrees / fmt::format("upkg_download_{}", info.package_abi);
                Path temp_zip_path = temp_dir / fmt::format("{}.zip", ref.id);
                Path final_zip_path = m_buildtrees / fmt::format("{}.zip", ref.id);

                const auto result = m_azure_tool.download(wdc, m_source, ref.id, ref.version, temp_dir);
                if (result && fs.exists(temp_zip_path, IgnoreErrors{}) && fs.rename(wdc, temp_zip_path, final_zip_path))
                {
                    out_zips[i].emplace(std::move(final_zip_path), RemoveWhen::always);
                }

                if (fs.exists(temp_dir, IgnoreErrors{}))
                {
                    fs.remove_all(temp_dir, IgnoreErrors{});
                }
            }
        }

    private:
        AzureUpkgTool m_azure_tool;
        AzureUpkgSource m_source;
        const Path& m_buildtrees;
    };

    Optional<Path> default_cache_path(DiagnosticContext& context)
    {
        auto maybe_cachepath = get_environment_variable_nonempty(EnvironmentVariableVcpkgDefaultBinaryCache);
        if (const auto pcachepath = maybe_cachepath.get())
        {
            get_global_metrics_collector().track_define(DefineMetric::VcpkgDefaultBinaryCache);
            Path path = std::move(*pcachepath);
            path.make_preferred();
            if (!real_filesystem.is_directory(path))
            {
                context.report(
                    DiagnosticLine{DiagKind::Error, path, msg::format(msgDefaultBinaryCacheRequiresDirectory)});
                return nullopt;
            }

            if (!path.is_absolute())
            {
                context.report(
                    DiagnosticLine{DiagKind::Error, path, msg::format(msgDefaultBinaryCacheRequiresAbsolutePath)});
                return nullopt;
            }

            return path;
        }

        auto maybe_platform_cache = get_platform_cache_vcpkg();
        if (auto platform_cache = maybe_platform_cache.get())
        {
            if (platform_cache->is_absolute())
            {
                *platform_cache /= "archives";
                platform_cache->make_preferred();
                return std::move(*platform_cache);
            }

            context.report(DiagnosticLine{
                DiagKind::Error, *platform_cache, msg::format(msgDefaultBinaryCachePlatformCacheRequiresAbsolutePath)});
            return nullopt;
        }

        context.report_error(LocalizedString{maybe_platform_cache.error()});
        return nullopt;
    }

    struct AssetSourcesState
    {
        bool cleared = false;
        bool block_origin = false;
        Optional<std::string> url_template_to_get;
        Optional<std::string> azblob_template_to_put;
        std::vector<std::string> secrets;
        Optional<std::string> script;

        void clear()
        {
            cleared = true;
            block_origin = false;
            url_template_to_get.clear();
            azblob_template_to_put.clear();
            secrets.clear();
            script.clear();
        }
    };

    static bool set_asset_read_url(DiagnosticContext& context,
                                   AssetSourcesState& state,
                                   const StackedEscapeParseDocument& position,
                                   std::string&& value)
    {
        if (state.url_template_to_get.has_value())
        {
            position.report_error_with_caret_line(context, msg::format(msgAMaximumOfOneAssetReadUrlCanBeSpecified));
            return false;
        }

        state.url_template_to_get.emplace(std::move(value));
        return true;
    }

    static bool set_asset_write_url(DiagnosticContext& context,
                                    AssetSourcesState& state,
                                    const StackedEscapeParseDocument& position,
                                    std::string&& value)
    {
        if (state.azblob_template_to_put.has_value())
        {
            position.report_error_with_caret_line(context, msg::format(msgAMaximumOfOneAssetWriteUrlCanBeSpecified));
            return false;
        }

        state.azblob_template_to_put.emplace(std::move(value));
        return true;
    }

    static Optional<BinaryCacheAccess> parse_asset_access_value(DiagnosticContext& context,
                                                                const StackedEscapeParseDocument& access)
    {
        if (access.text() == "read")
        {
            return BinaryCacheAccess::Read;
        }
        else if (access.text() == "write")
        {
            return BinaryCacheAccess::Write;
        }
        else if (access.text() == "readwrite")
        {
            return BinaryCacheAccess::ReadWrite;
        }

        access.report_error_with_caret_line(context, msg::format(msgExpectedReadWriteReadWrite));
        return nullopt;
    }
}

namespace vcpkg
{
    FeedReference::FeedReference(std::string id, std::string version) : id(std::move(id)), version(std::move(version))
    {
    }

    std::string FeedReference::nupkg_filename() const { return Strings::concat(id, '.', version, ".nupkg"); }

    static Optional<UrlTemplate> validate_url_template(DiagnosticContext& context,
                                                       const StackedEscapeParseDocument& candidate)
    {
        Optional<UrlTemplate> result;
        auto& url_template = result.emplace();
        url_template.url_template = std::string{candidate.text()};
        auto maybe_formatted = api_stable_format(
            context,
            candidate,
            [&url_template](DiagnosticContext& context,
                            std::string&,
                            StringView variable_name,
                            const StackedParseEnumerator& position) {
                if (variable_name == "sha")
                {
                    url_template.has_sha = true;
                    return true;
                }
                static constexpr StringLiteral other_valid_keys[] = {"name", "version", "triplet"};
                if (Util::Vectors::contains(other_valid_keys, variable_name))
                {
                    url_template.has_other = true;
                    return true;
                }

                position.report_error_with_caret_line(
                    context, msg::format(msgUnknownVariablesInTemplate).append_raw(": ").append_raw(variable_name));
                return false;
            });

        if (!maybe_formatted.has_value())
        {
            result.clear();
        }

        return result;
    }

    std::string UrlTemplate::instantiate_variables(const BinaryPackageReadInfo& info) const
    {
        ParsedDocument doc{url_template, nullopt};
        const auto stacked = doc.stacked(console_diagnostic_context).value_or_exit(VCPKG_LINE_INFO);
        return api_stable_format(
                   console_diagnostic_context,
                   stacked,
                   [&](DiagnosticContext&, std::string& out, StringView key, const StackedParseEnumerator&) {
                       if (key == "version")
                       {
                           out += info.version.text;
                       }
                       else if (key == "name")
                       {
                           out += info.spec.name();
                       }
                       else if (key == "triplet")
                       {
                           out += info.spec.triplet().canonical_name();
                       }
                       else if (key == "sha")
                       {
                           out += info.package_abi;
                       }
                       else
                       {
                           Checks::unreachable(VCPKG_LINE_INFO,
                                               "used instantiate_variables without validating UrlTemplate first");
                       };

                       return true;
                   })
            .value_or_exit(VCPKG_LINE_INFO);
    }

    std::string AzCopyUrl::make_object_path(const std::string& abi) const
    {
        const auto base_url = url.back() == '/' ? url : Strings::concat(url, "/");
        return sas.empty() ? Strings::concat(base_url, abi, ".zip") : Strings::concat(base_url, abi, ".zip?", sas);
    }

    std::string AzCopyUrl::make_container_path() const { return sas.empty() ? url : Strings::concat(url, "?", sas); }

    static NuGetRepoInfo get_nuget_repo_info_from_env(const VcpkgCmdArguments& args)
    {
        if (auto p = args.vcpkg_nuget_repository.get())
        {
            get_global_metrics_collector().track_define(DefineMetric::VcpkgNuGetRepository);
            return {*p};
        }

        auto maybe_gh_repo = get_environment_variable(EnvironmentVariableGitHubRepository);
        const auto pgh_repo = maybe_gh_repo.get();
        if (!pgh_repo || pgh_repo->empty())
        {
            return {};
        }

        auto maybe_gh_server = get_environment_variable(EnvironmentVariableGitHubServerUrl);
        const auto pgh_server = maybe_gh_server.get();
        if (!pgh_server || pgh_server->empty())
        {
            return {};
        }

        get_global_metrics_collector().track_define(DefineMetric::GitHubRepository);
        return {Strings::concat(*pgh_server, '/', *pgh_repo, ".git"),
                get_environment_variable(EnvironmentVariableGitHubRef).value_or(""),
                get_environment_variable(EnvironmentVariableGitHubSha).value_or("")};
    }

    void ReadOnlyBinaryCache::fetch(DiagnosticContext& context, const Filesystem& fs, View<InstallPlanAction> actions)
    {
        std::vector<const InstallPlanAction*> action_ptrs;
        std::vector<RestoreResult> restores;
        std::vector<CacheStatus*> statuses;
        for (auto&& provider : m_config.read)
        {
            action_ptrs.clear();
            restores.clear();
            statuses.clear();
            for (size_t i = 0; i < actions.size(); ++i)
            {
                if (auto abi = actions[i].package_abi())
                {
                    CacheStatus& status = m_status[*abi];
                    if (status.should_attempt_restore(provider.get()))
                    {
                        action_ptrs.push_back(&actions[i]);
                        restores.push_back(RestoreResult::unavailable);
                        statuses.push_back(&status);
                    }
                }
            }
            if (action_ptrs.empty()) continue;

            ElapsedTimer timer;
            provider->fetch(context, fs, action_ptrs, restores);
            size_t num_restored = 0;
            for (size_t i = 0; i < restores.size(); ++i)
            {
                if (restores[i] == RestoreResult::unavailable)
                {
                    statuses[i]->mark_unavailable(provider.get());
                }
                else
                {
                    statuses[i]->mark_restored();
                    ++num_restored;
                }
            }
            context.statusln(provider->restored_message(
                num_restored, timer.elapsed().as<std::chrono::high_resolution_clock::duration>()));
        }
    }

    bool ReadOnlyBinaryCache::is_restored(const InstallPlanAction& action) const
    {
        if (auto abi = action.package_abi())
        {
            auto it = m_status.find(*abi);
            if (it != m_status.end()) return it->second.is_restored();
        }
        return false;
    }

    void ReadOnlyBinaryCache::install_read_provider(std::unique_ptr<IReadBinaryProvider>&& provider)
    {
        m_config.read.push_back(std::move(provider));
    }

    void ReadOnlyBinaryCache::mark_all_unrestored()
    {
        for (auto& entry : m_status)
        {
            entry.second.mark_unrestored();
        }
    }

    std::vector<CacheAvailability> ReadOnlyBinaryCache::precheck(DiagnosticContext& context,
                                                                 const Filesystem& fs,
                                                                 View<const InstallPlanAction*> actions)
    {
        const std::vector<CacheStatus*> statuses = Util::fmap(actions, [this](const InstallPlanAction* action) {
            Checks::check_exit(VCPKG_LINE_INFO, action);
            return &m_status[action->package_abi_or_exit(VCPKG_LINE_INFO)];
        });

        std::vector<const InstallPlanAction*> action_ptrs;
        std::vector<CacheAvailability> cache_result;
        std::vector<size_t> indexes;
        for (auto&& provider : m_config.read)
        {
            action_ptrs.clear();
            cache_result.clear();
            indexes.clear();
            for (size_t i = 0; i < actions.size(); ++i)
            {
                if (statuses[i]->should_attempt_precheck(provider.get()))
                {
                    action_ptrs.push_back(actions[i]);
                    cache_result.push_back(CacheAvailability::unknown);
                    indexes.push_back(i);
                }
            }
            if (action_ptrs.empty()) continue;

            provider->precheck(context, fs, action_ptrs, cache_result);

            for (size_t i = 0; i < action_ptrs.size(); ++i)
            {
                if (cache_result[i] == CacheAvailability::available)
                {
                    statuses[i]->mark_available(provider.get());
                }
                else if (cache_result[i] == CacheAvailability::unavailable)
                {
                    statuses[i]->mark_unavailable(provider.get());
                }
            }
        }

        return Util::fmap(statuses, [](CacheStatus* s) {
            return s->get_available_provider() ? CacheAvailability::available : CacheAvailability::unavailable;
        });
    }

    void BinaryCacheSynchronizer::add_submitted() noexcept
    {
        // This can set the unused bit but if that happens we are terminating anyway.
        if ((m_state.fetch_add(1, std::memory_order_acq_rel) & SubmittedMask) == SubmittedMask)
        {
            Checks::unreachable(VCPKG_LINE_INFO, "Maximum job count exceeded");
        }
    }

    BinaryCacheSyncState BinaryCacheSynchronizer::fetch_add_completed() noexcept
    {
        auto old = m_state.load(std::memory_order_acquire);
        backing_uint_t local;
        do
        {
            local = old;
            if ((local & CompletedMask) == CompletedMask)
            {
                Checks::unreachable(VCPKG_LINE_INFO, "Maximum job count exceeded");
            }

            local += OneCompleted;
        } while (!m_state.compare_exchange_weak(old, local, std::memory_order_acq_rel));

        BinaryCacheSyncState result;
        result.jobs_submitted = local & SubmittedMask;
        result.jobs_completed = (local & CompletedMask) >> UpperShift;
        result.submission_complete = (local & SubmissionCompleteBit) != 0;
        return result;
    }

    BinaryCacheSynchronizer::counter_uint_t BinaryCacheSynchronizer::
        fetch_incomplete_mark_submission_complete() noexcept
    {
        auto old = m_state.load(std::memory_order_acquire);
        backing_uint_t local;
        BinaryCacheSynchronizer::counter_uint_t submitted;
        do
        {
            local = old;

            // Remove completions from the submission counter so that the (X/Y) console
            // output is prettier.
            submitted = local & SubmittedMask;
            auto completed = (local & CompletedMask) >> UpperShift;
            if (completed >= submitted)
            {
                local = SubmissionCompleteBit;
            }
            else
            {
                local = (submitted - completed) | SubmissionCompleteBit;
            }
        } while (!m_state.compare_exchange_weak(old, local, std::memory_order_acq_rel));
        auto state = m_state.fetch_or(SubmissionCompleteBit, std::memory_order_acq_rel);

        return (state & SubmittedMask) - ((state & CompletedMask) >> UpperShift);
    }

    bool BinaryCache::install_providers(DiagnosticContext& context,
                                        const VcpkgCmdArguments& args,
                                        const VcpkgPaths& paths)
    {
        auto& fs = paths.get_filesystem();
        auto& tools = paths.get_tool_cache();
        if (args.binary_caching_enabled())
        {
            if (args.env_binary_sources.has_value())
            {
                get_global_metrics_collector().track_define(DefineMetric::VcpkgBinarySources);
            }

            if (args.cli_binary_sources.size() != 0)
            {
                get_global_metrics_collector().track_define(DefineMetric::BinaryCachingSource);
            }

            auto maybe_default_cache_path = default_cache_path(context);
            auto default_cache_path = maybe_default_cache_path.get();
            if (!default_cache_path)
            {
                return false;
            }

            auto maybe_parsed = parse_binary_provider_configs(
                context, *default_cache_path, args.env_binary_sources.value_or(""), args.cli_binary_sources);
            auto parsed = maybe_parsed.get();
            if (!parsed)
            {
                return false;
            }

            static const std::map<StringLiteral, DefineMetric> metric_names{
                {"aws", DefineMetric::BinaryCachingAws},
                {"azblob", DefineMetric::BinaryCachingAzBlob},
                {"azcopy", DefineMetric::BinaryCachingAzCopy},
                {"azcopy-sas", DefineMetric::BinaryCachingAzCopySas},
                {"cos", DefineMetric::BinaryCachingCos},
                {"default", DefineMetric::BinaryCachingDefault},
                {"files", DefineMetric::BinaryCachingFiles},
                {"gcs", DefineMetric::BinaryCachingGcs},
                {"http", DefineMetric::BinaryCachingHttp},
                {"nuget", DefineMetric::BinaryCachingNuGet},
                {"upkg", DefineMetric::BinaryCachingUpkg},
            };

            MetricsSubmission metrics;
            for (const auto& cache_provider : parsed->telemetry_tags)
            {
                auto it = metric_names.find(cache_provider);
                if (it != metric_names.end())
                {
                    metrics.track_define(it->second);
                }
            }

            get_global_metrics_collector().track_submission(std::move(metrics));

            m_config.nuget_prefix = args.nuget_id_prefix.value_or("");
            if (!m_config.nuget_prefix.empty()) m_config.nuget_prefix.push_back('_');

            m_config.nuget_repo = get_nuget_repo_info_from_env(args);

            const auto& buildtrees = paths.buildtrees();

            std::vector<std::string> secrets;
            for (const auto& provider : parsed->providers)
            {
                if (provider.kind == BinaryCacheProviderKind::AzBlob ||
                    provider.kind == BinaryCacheProviderKind::AzCopySas)
                {
                    secrets.push_back(provider.arg2.value_or_exit(VCPKG_LINE_INFO));
                }
            }

            ZipTool zip_tool;
            bool has_zip_tool = false;
            auto ensure_zip_tool = [&]() -> bool {
                if (!has_zip_tool)
                {
                    if (!zip_tool.setup(context, fs, tools))
                    {
                        return false;
                    }

                    has_zip_tool = true;
                }

                return true;
            };

            std::shared_ptr<const GcsStorageTool> gcs_tool;
            auto ensure_gcs_tool = [&]() -> bool {
                if (!gcs_tool)
                {
                    if (auto gcs_tool_path = tools.get_tool_path(context, fs, Tools::GSUTIL))
                    {
                        gcs_tool = std::make_shared<GcsStorageTool>(*gcs_tool_path);
                    }
                    else
                    {
                        return false;
                    }
                }

                return true;
            };

            std::shared_ptr<const AwsStorageTool> aws_tool;
            auto ensure_aws_tool = [&]() -> bool {
                if (!aws_tool)
                {
                    if (auto aws_tool_path = tools.get_tool_path(context, fs, Tools::AWSCLI))
                    {
                        aws_tool = std::make_shared<AwsStorageTool>(*aws_tool_path, parsed->aws_no_sign_request);
                    }
                    else
                    {
                        return false;
                    }
                }

                return true;
            };

            std::shared_ptr<const CosStorageTool> cos_tool;
            auto ensure_cos_tool = [&]() -> bool {
                if (!cos_tool)
                {
                    if (auto cos_tool_path = tools.get_tool_path(context, fs, Tools::COSCLI))
                    {
                        cos_tool = std::make_shared<CosStorageTool>(*cos_tool_path);
                    }
                    else
                    {
                        return false;
                    }
                }

                return true;
            };

            Path azcopy_tool;
            bool has_azcopy_tool = false;
            auto ensure_azcopy_tool = [&]() -> bool {
                if (!has_azcopy_tool)
                {
                    if (auto tool = tools.get_tool_path(context, fs, Tools::AZCOPY))
                    {
                        azcopy_tool = *tool;
                    }
                    else
                    {
                        return false;
                    }

                    has_azcopy_tool = true;
                }

                return true;
            };

            Path azcli_tool;
            bool has_azcli_tool = false;
            auto ensure_azcli_tool = [&]() -> bool {
                if (!has_azcli_tool)
                {
                    if (auto tool = tools.get_tool_path(context, fs, Tools::AZCLI))
                    {
                        azcli_tool = *tool;
                    }
                    else
                    {
                        return false;
                    }

                    has_azcli_tool = true;
                }

                return true;
            };

            std::unique_ptr<NugetBaseBinaryProvider> nuget_base;
            auto ensure_nuget_base = [&]() -> bool {
                if (!nuget_base)
                {
                    auto maybe_nuget_tools = get_nuget_tool_tools(context, fs, tools);
                    if (auto* nuget_tools = maybe_nuget_tools.get())
                    {
                        nuget_base =
                            std::make_unique<NugetBaseBinaryProvider>(NuGetTool(std::move(*nuget_tools),
                                                                                parsed->nuget_timeout,
                                                                                parsed->nuget_interactive,
                                                                                args.use_nuget_cache.value_or(false)),
                                                                      paths.packages(),
                                                                      buildtrees,
                                                                      m_config.nuget_prefix);
                    }
                    else
                    {
                        return false;
                    }
                }

                return true;
            };

            auto installs_read = [](BinaryCacheAccess access) {
                return access == BinaryCacheAccess::Read || access == BinaryCacheAccess::ReadWrite;
            };
            auto installs_write = [](BinaryCacheAccess access) {
                return access == BinaryCacheAccess::Write || access == BinaryCacheAccess::ReadWrite;
            };

            for (const auto& provider : parsed->providers)
            {
                switch (provider.kind)
                {
                    case BinaryCacheProviderKind::Files:
                    {
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<FilesReadBinaryProvider>(
                                zip_tool, Path{provider.arg1.value_or_exit(VCPKG_LINE_INFO)}));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(std::make_unique<FilesWriteBinaryProvider>(
                                Path{provider.arg1.value_or_exit(VCPKG_LINE_INFO)}));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::NuGet:
                    {
                        if (!ensure_nuget_base()) return false;
                        const auto& source = provider.arg1.value_or_exit(VCPKG_LINE_INFO);
                        if (installs_read(provider.access))
                        {
                            m_config.read.push_back(std::make_unique<NugetReadBinaryProvider>(
                                *nuget_base, nuget_sources_arg({&source, 1})));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(std::make_unique<NugetBinaryPushProvider>(
                                *nuget_base, nuget_sources_arg({&source, 1})));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::NuGetConfig:
                    {
                        if (!ensure_nuget_base()) return false;
                        Path config_path{provider.arg1.value_or_exit(VCPKG_LINE_INFO)};
                        if (installs_read(provider.access))
                        {
                            m_config.read.push_back(std::make_unique<NugetReadBinaryProvider>(
                                *nuget_base, nuget_configfile_arg(config_path)));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(std::make_unique<NugetBinaryPushProvider>(
                                *nuget_base, nuget_configfile_arg(config_path)));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::Http:
                    {
                        UrlTemplate url_template{provider.arg1.value_or_exit(VCPKG_LINE_INFO)};
                        if (const auto header = provider.arg2.get())
                        {
                            url_template.headers.push_back(*header);
                        }

                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<HttpGetBinaryProvider>(
                                zip_tool, buildtrees, UrlTemplate{url_template}, secrets));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<HTTPPutBinaryProvider>(std::move(url_template), secrets));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::AzBlob:
                    {
                        AzCopyUrl az_url{provider.arg1.value_or_exit(VCPKG_LINE_INFO),
                                         provider.arg2.value_or_exit(VCPKG_LINE_INFO)};
                        UrlTemplate url_template{az_url.make_object_path("{sha}")};
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<HttpGetBinaryProvider>(
                                zip_tool, buildtrees, UrlTemplate{url_template}, secrets));
                        }

                        if (installs_write(provider.access))
                        {
                            auto headers = azure_blob_headers();
                            url_template.headers.assign(headers.begin(), headers.end());
                            m_config.write.push_back(
                                std::make_unique<AzureBlobPutBinaryProvider>(std::move(url_template), secrets));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::AzCopy:
                    case BinaryCacheProviderKind::AzCopySas:
                    {
                        if (!ensure_azcopy_tool()) return false;
                        AzCopyUrl az_url{provider.arg1.value_or_exit(VCPKG_LINE_INFO), provider.arg2.value_or("")};
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<AzCopyStorageProvider>(
                                zip_tool, buildtrees, AzCopyUrl{az_url}, azcopy_tool));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<AzCopyStoragePushProvider>(std::move(az_url), azcopy_tool));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::GCS:
                    {
                        if (!ensure_gcs_tool()) return false;
                        auto prefix = provider.arg1.value_or_exit(VCPKG_LINE_INFO);
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<ObjectStorageProvider>(
                                zip_tool, buildtrees, std::string{prefix}, gcs_tool));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<ObjectStoragePushProvider>(std::string{prefix}, gcs_tool));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::AWS:
                    {
                        if (!ensure_aws_tool()) return false;
                        auto prefix = provider.arg1.value_or_exit(VCPKG_LINE_INFO);
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<ObjectStorageProvider>(
                                zip_tool, buildtrees, std::string{prefix}, aws_tool));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<ObjectStoragePushProvider>(std::string{prefix}, aws_tool));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::COS:
                    {
                        if (!ensure_cos_tool()) return false;
                        auto prefix = provider.arg1.value_or_exit(VCPKG_LINE_INFO);
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<ObjectStorageProvider>(
                                zip_tool, buildtrees, std::string{prefix}, cos_tool));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<ObjectStoragePushProvider>(std::string{prefix}, cos_tool));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::AzUniversal:
                    {
                        if (!ensure_azcli_tool()) return false;
                        AzureUpkgSource source{provider.arg1.value_or_exit(VCPKG_LINE_INFO),
                                               provider.arg2.value_or_exit(VCPKG_LINE_INFO),
                                               provider.arg3.value_or_exit(VCPKG_LINE_INFO)};
                        if (installs_read(provider.access))
                        {
                            if (!ensure_zip_tool()) return false;
                            m_config.read.push_back(std::make_unique<AzureUpkgGetBinaryProvider>(
                                zip_tool, azcli_tool, AzureUpkgSource{source}, buildtrees));
                        }

                        if (installs_write(provider.access))
                        {
                            m_config.write.push_back(
                                std::make_unique<AzureUpkgPutBinaryProvider>(azcli_tool, std::move(source)));
                        }

                        break;
                    }
                    case BinaryCacheProviderKind::None: break;
                    default: Checks::unreachable(VCPKG_LINE_INFO);
                }
            }
        }

        m_needs_nuspec_data = Util::any_of(m_config.write, [](auto&& p) { return p->needs_nuspec_data(); });
        m_needs_zip_file = Util::any_of(m_config.write, [](auto&& p) { return p->needs_zip_file(); });
        if (m_needs_zip_file)
        {
            if (!m_zip_tool.setup(context, fs, tools))
            {
                return false;
            }
        }

        return true;
    }
    BinaryCache::BinaryCache(const Filesystem& fs)
        : m_fs(fs), m_bg_msg_sink(stdout_sink), m_push_thread(&BinaryCache::push_thread_main, this)
    {
    }
    BinaryCache::~BinaryCache() { wait_for_async_complete_and_join(); }

    void BinaryCache::push_success(CleanPackages clean_packages, const InstallPlanAction& action)
    {
        if (auto abi = action.package_abi())
        {
            bool restored;
            auto it = m_status.find(*abi);
            if (it == m_status.end())
            {
                restored = false;
            }
            else
            {
                restored = it->second.is_restored();

                // Purge all status information on push_success (cache invalidation)
                // - push_success may delete packages/ (invalidate restore)
                // - push_success may make the package available from providers (invalidate unavailable)
                m_status.erase(it);
            }

            if (!restored && !m_config.write.empty())
            {
                ElapsedTimer timer;
                BinaryPackageWriteInfo request{action};

                if (m_needs_nuspec_data)
                {
                    request.nuspec =
                        generate_nuspec(request.package_dir, action, m_config.nuget_prefix, m_config.nuget_repo);
                }

                if (m_config.write.size() == 1)
                {
                    request.unique_write_provider = true;
                }

                m_synchronizer.add_submitted();
                msg::println(msg::format(msgSubmittingBinaryCacheBackground,
                                         msg::spec = action.display_name(),
                                         msg::count = m_config.write.size()));
                m_actions_to_push.push(ActionToPush{std::move(request), clean_packages});
                return;
            }
        }

        if (clean_packages == CleanPackages::Yes)
        {
            m_fs.remove_all(action.package_dir, VCPKG_LINE_INFO);
        }
    }

    void BinaryCache::print_updates() { m_bg_msg_sink.print_published(); }

    void BinaryCache::wait_for_async_complete_and_join()
    {
        m_bg_msg_sink.print_published();
        auto incomplete_count = m_synchronizer.fetch_incomplete_mark_submission_complete();
        if (incomplete_count != 0)
        {
            msg::println(msgWaitUntilPackagesUploaded, msg::count = incomplete_count);
        }

        m_bg_msg_sink.publish_directly_to_out_sink();
        m_actions_to_push.stop();
        if (m_push_thread.joinable())
        {
            m_push_thread.join();
        }
    }

    void BinaryCache::push_thread_main()
    {
        std::vector<ActionToPush> my_tasks;
        PrintingDiagnosticContext pdc{m_bg_msg_sink};
        WarningDiagnosticContext wdc{pdc};
        while (m_actions_to_push.get_work(my_tasks))
        {
            for (auto& action_to_push : my_tasks)
            {
                ElapsedTimer timer;
                if (m_needs_zip_file)
                {
                    Path zip_path = action_to_push.request.package_dir + ".zip";
                    if (m_zip_tool.compress_directory_to_zip(pdc, m_fs, action_to_push.request.package_dir, zip_path))
                    {
                        action_to_push.request.zip_path = std::move(zip_path);
                    }
                }

                size_t num_destinations = 0;
                for (auto&& provider : m_config.write)
                {
                    if (!provider->needs_zip_file() || action_to_push.request.zip_path.has_value())
                    {
                        num_destinations += provider->push_success(pdc, m_fs, action_to_push.request);
                    }
                }

                if (action_to_push.request.zip_path)
                {
                    (void)m_fs.remove(wdc, *action_to_push.request.zip_path.get());
                }

                if (action_to_push.clean_after_push == CleanPackages::Yes)
                {
                    (void)m_fs.remove_all(wdc, action_to_push.request.package_dir);
                }

                auto sync_state = m_synchronizer.fetch_add_completed();
                auto message = msg::format(msgSubmittingBinaryCacheComplete,
                                           msg::spec = action_to_push.request.display_name,
                                           msg::count = num_destinations,
                                           msg::elapsed = timer.elapsed());
                if (sync_state.submission_complete)
                {
                    message.append_raw(fmt::format(" ({}/{})", sync_state.jobs_completed, sync_state.jobs_submitted));
                }

                m_bg_msg_sink.println(message);
            }
        }
    }

    bool CacheStatus::should_attempt_precheck(const IReadBinaryProvider* sender) const noexcept
    {
        switch (m_status)
        {
            case CacheStatusState::unknown: return !Util::Vectors::contains(m_known_unavailable_providers, sender);
            case CacheStatusState::available: return false;
            case CacheStatusState::restored: return false;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    bool CacheStatus::should_attempt_restore(const IReadBinaryProvider* sender) const noexcept
    {
        switch (m_status)
        {
            case CacheStatusState::unknown: return !Util::Vectors::contains(m_known_unavailable_providers, sender);
            case CacheStatusState::available: return m_available_provider == sender;
            case CacheStatusState::restored: return false;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    bool CacheStatus::is_unavailable(const IReadBinaryProvider* sender) const noexcept
    {
        return Util::Vectors::contains(m_known_unavailable_providers, sender);
    }

    bool CacheStatus::is_restored() const noexcept { return m_status == CacheStatusState::restored; }

    void CacheStatus::mark_unavailable(const IReadBinaryProvider* sender)
    {
        if (!Util::Vectors::contains(m_known_unavailable_providers, sender))
        {
            m_known_unavailable_providers.push_back(sender);
        }
    }
    void CacheStatus::mark_available(const IReadBinaryProvider* sender) noexcept
    {
        switch (m_status)
        {
            case CacheStatusState::unknown:
                m_status = CacheStatusState::available;
                m_available_provider = sender;
                break;
            case CacheStatusState::available:
            case CacheStatusState::restored: break;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    void CacheStatus::mark_restored() noexcept
    {
        switch (m_status)
        {
            case CacheStatusState::unknown: m_known_unavailable_providers.clear(); [[fallthrough]];
            case CacheStatusState::available: m_status = CacheStatusState::restored; break;
            case CacheStatusState::restored: break;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    void CacheStatus::mark_unrestored() noexcept
    {
        if (m_status == CacheStatusState::restored)
        {
            m_status = CacheStatusState::available;
        }
    }

    const IReadBinaryProvider* CacheStatus::get_available_provider() const noexcept
    {
        switch (m_status)
        {
            case CacheStatusState::available: return m_available_provider;
            case CacheStatusState::unknown:
            case CacheStatusState::restored: return nullptr;
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    BinaryPackageReadInfo::BinaryPackageReadInfo(const InstallPlanAction& action)
        : package_abi(action.package_abi_or_exit(VCPKG_LINE_INFO))
        , spec(action.spec)
        , display_name(action.display_name())
        , version(action.version)
        , package_dir(action.package_dir)
    {
    }

    Optional<AssetCachingSettings> parse_download_configuration(DiagnosticContext& context,
                                                                const Optional<std::string>& arg)
    {
        Optional<AssetCachingSettings> out;
        auto& result = out.emplace();
        if (!arg || arg.get()->empty()) return out;

        get_global_metrics_collector().track_define(DefineMetric::AssetSource);

        AssetSourcesState s;
        const auto source = format_environment_variable(EnvironmentVariableXVcpkgAssetSources).to_string();
        ParsedDocument doc(*arg.get(), source);
        auto e = doc.enumerator();
        while (!e.at_eof())
        {
            char32_t matched_terminal;
            auto maybe_kind = e.match_escaped(context, matched_terminal, '`', ",;");
            const auto kind = maybe_kind.get();
            if (!kind)
            {
                out.clear();
                return out;
            }

            if (kind->text().empty() && matched_terminal == ';')
            {
                continue;
            }

            if (kind->text() == "x-block-origin")
            {
                if (matched_terminal == ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgAssetCacheProviderAcceptsNoArguments, msg::value = "x-block-origin"));
                    out.clear();
                    return out;
                }

                s.block_origin = true;
                continue;
            }

            if (kind->text() == "clear")
            {
                if (matched_terminal == ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgAssetCacheProviderAcceptsNoArguments, msg::value = "clear"));
                    out.clear();
                    return out;
                }

                s.clear();
                continue;
            }

            if (kind->text() == "x-azurl")
            {
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(context,
                                                                     msg::format(msgAzUrlAssetCacheRequiresBaseUrl));
                    out.clear();
                    return out;
                }

                auto maybe_baseurl = e.match_escaped(context, matched_terminal, '`', ",;");
                auto baseurl = maybe_baseurl.get();
                if (!baseurl)
                {
                    out.clear();
                    return out;
                }

                if (baseurl->text().empty())
                {
                    baseurl->report_error_with_caret_line(context, msg::format(msgAzUrlAssetCacheRequiresBaseUrl));
                    out.clear();
                    return out;
                }

                auto normalized = baseurl->text();
                if (normalized.back() != '/')
                {
                    normalized.push_back('/');
                }

                normalized.append("<SHA>");

                if (matched_terminal == ',')
                {
                    auto maybe_sas = e.match_escaped(context, matched_terminal, '`', ",;");
                    auto sas = maybe_sas.get();
                    if (!sas)
                    {
                        out.clear();
                        return out;
                    }

                    if (!sas->text().empty())
                    {
                        if (!Strings::starts_with(sas->text(), "?"))
                        {
                            normalized.push_back('?');
                        }

                        normalized.append(sas->text().data(), sas->text().size());
                        s.secrets.push_back(sas->text());
                    }
                }

                BinaryCacheAccess access = BinaryCacheAccess::Read;
                if (matched_terminal == ',')
                {
                    auto maybe_access = e.match_escaped(context, matched_terminal, '`', ",;");
                    const auto access_doc = maybe_access.get();
                    if (!access_doc)
                    {
                        out.clear();
                        return out;
                    }

                    auto maybe_parsed_access = parse_asset_access_value(context, *access_doc);
                    const auto parsed_access = maybe_parsed_access.get();
                    if (!parsed_access)
                    {
                        out.clear();
                        return out;
                    }

                    access = *parsed_access;

                    if (matched_terminal == ',')
                    {
                        auto maybe_extra = e.match_escaped(context, matched_terminal, '`', ",;");
                        const auto extra = maybe_extra.get();
                        if (!extra)
                        {
                            out.clear();
                            return out;
                        }

                        extra->report_error_with_caret_line(context,
                                                            msg::format(msgAzUrlAssetCacheRequiresLessThanFour));
                        out.clear();
                        return out;
                    }
                }

                if (access == BinaryCacheAccess::Read || access == BinaryCacheAccess::ReadWrite)
                {
                    if (!set_asset_read_url(context, s, *baseurl, std::string(normalized)))
                    {
                        out.clear();
                        return out;
                    }
                }

                if (access == BinaryCacheAccess::Write || access == BinaryCacheAccess::ReadWrite)
                {
                    if (!set_asset_write_url(context, s, *baseurl, std::move(normalized)))
                    {
                        out.clear();
                        return out;
                    }
                }

                continue;
            }

            if (kind->text() == "x-script")
            {
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(context,
                                                                     msg::format(msgScriptAssetCacheRequiresScript));
                    out.clear();
                    return out;
                }

                auto maybe_script = e.match_escaped(context, matched_terminal, '`', ",;");
                const auto script = maybe_script.get();
                if (!script)
                {
                    out.clear();
                    return out;
                }

                if (matched_terminal == ',')
                {
                    auto maybe_extra = e.match_escaped(context, matched_terminal, '`', ",;");
                    const auto extra = maybe_extra.get();
                    if (!extra)
                    {
                        out.clear();
                        return out;
                    }

                    extra->report_error_with_caret_line(context, msg::format(msgScriptAssetCacheRequiresScript));
                    out.clear();
                    return out;
                }

                s.script = script->move_text();
                continue;
            }

            kind->report_error_with_caret_line(context, msg::format(msgUnexpectedAssetCacheProvider));
            out.clear();
            return out;
        }

        if (auto read = s.url_template_to_get.get())
        {
            result.m_read_url_template = std::move(*read);
        }

        if (auto write = s.azblob_template_to_put.get())
        {
            result.m_write_url_template = std::move(*write);
            auto v = azure_blob_headers();
            result.m_write_headers.assign(v.begin(), v.end());
        }

        result.m_secrets = std::move(s.secrets);
        result.m_block_origin = s.block_origin;
        result.m_script = std::move(s.script);
        return out;
    }

    StringLiteral to_string_literal(BinaryCacheProviderKind kind)
    {
        switch (kind)
        {
            case BinaryCacheProviderKind::None: return "none";
            case BinaryCacheProviderKind::Files: return "files";
            case BinaryCacheProviderKind::NuGet: return "nuget";
            case BinaryCacheProviderKind::NuGetConfig: return "nugetconfig";
            case BinaryCacheProviderKind::Http: return "http";
            case BinaryCacheProviderKind::AzBlob: return "x-azblob";
            case BinaryCacheProviderKind::AzCopy: return "x-azcopy";
            case BinaryCacheProviderKind::AzCopySas: return "x-azcopy-sas";
            case BinaryCacheProviderKind::GCS: return "x-gcs";
            case BinaryCacheProviderKind::AWS: return "x-aws";
            case BinaryCacheProviderKind::COS: return "x-cos";
            case BinaryCacheProviderKind::AzUniversal: return "x-az-universal";
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    StringLiteral to_string_literal(BinaryCacheAccess access)
    {
        switch (access)
        {
            case BinaryCacheAccess::Read: return "read";
            case BinaryCacheAccess::Write: return "write";
            case BinaryCacheAccess::ReadWrite: return "readwrite";
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    bool operator==(const BinaryCacheProviderEntry& lhs, const BinaryCacheProviderEntry& rhs)
    {
        return lhs.kind == rhs.kind && lhs.access == rhs.access && lhs.arg1 == rhs.arg1 && lhs.arg2 == rhs.arg2 &&
               lhs.arg3 == rhs.arg3;
    }

    bool operator!=(const BinaryCacheProviderEntry& lhs, const BinaryCacheProviderEntry& rhs) { return !(lhs == rhs); }

    void BinaryCacheProviderEntry::to_string(std::string& out) const
    {
        fmt::format_to(std::back_inserter(out), "kind: {}, access: {}", kind, access);

        if (const auto actual_arg1 = arg1.get())
        {
            fmt::format_to(std::back_inserter(out), ", arg1: {}", *actual_arg1);
        }

        if (const auto actual_arg2 = arg2.get())
        {
            fmt::format_to(std::back_inserter(out), ", arg2: {}", *actual_arg2);
        }

        if (const auto actual_arg3 = arg3.get())
        {
            fmt::format_to(std::back_inserter(out), ", arg3: {}", *actual_arg3);
        }
    }

    std::string BinaryCacheProviderEntry::to_string() const { return adapt_to_string(*this); }

    static Optional<BinaryCacheAccess> parse_access_terminal(DiagnosticContext& context,
                                                             ParseEnumerator& e,
                                                             char32_t matched_terminal,
                                                             StringLiteral binary_source,
                                                             const msg::MessageT<msg::binary_source_t>& overlong_error)
    {
        Optional<BinaryCacheAccess> result;
        if (matched_terminal != ',')
        {
            // default to readwrite if no access is specified
            result.emplace(BinaryCacheAccess::ReadWrite);
            return result;
        }

        auto maybe_access = e.match_escaped(context, matched_terminal, '`', ",;");
        auto access = maybe_access.get();
        if (!access)
        {
            return result;
        }

        if (matched_terminal == ',')
        {
            access->report_error_with_caret_line_end_delimiter(
                context, msg::format(overlong_error, msg::binary_source = binary_source));
            return result;
        }

        if (access->text() == "readwrite")
        {
            result.emplace(BinaryCacheAccess::ReadWrite);
        }
        else if (access->text() == "read")
        {
            result.emplace(BinaryCacheAccess::Read);
        }
        else if (access->text() == "write")
        {
            result.emplace(BinaryCacheAccess::Write);
        }
        else
        {
            access->report_error_with_caret_line(context, msg::format(msgExpectedReadWriteReadWrite));
        }

        return result;
    }

    static bool parse_absolute_path_provider(DiagnosticContext& context,
                                             ParseEnumerator& e,
                                             char32_t matched_terminal,
                                             const StackedEscapeParseDocument& kind,
                                             BinaryCacheParsedConfigs& result,
                                             BinaryCacheProviderKind provider_kind,
                                             StringLiteral binary_source,
                                             StringLiteral telemetry_tag)
    {
        // match <provider>,<absolute path>[,<rw>]
        if (matched_terminal != ',')
        {
            kind.report_error_with_caret_line_end_delimiter(
                context, msg::format(msgInvalidArgumentRequiresPathArgument, msg::binary_source = binary_source));
            return false;
        }

        auto maybe_path = e.match_escaped(context, matched_terminal, '`', ",;");
        const auto path = maybe_path.get();
        if (!path)
        {
            return false;
        }

        Path as_path(path->move_text());
        if (!as_path.is_absolute())
        {
            path->report_error_with_caret_line(context, msg::format(msgInvalidArgumentRequiresAbsolutePath));
            return false;
        }

        auto maybe_access = parse_access_terminal(
            context, e, matched_terminal, binary_source, msgInvalidArgumentRequiresOneOrTwoArguments);
        const auto access = maybe_access.get();
        if (!access)
        {
            return false;
        }

        result.providers.push_back({provider_kind, *access, std::move(as_path).native(), nullopt, nullopt});
        result.telemetry_tags.insert(telemetry_tag);
        return true;
    }

    static bool check_azure_base_url(DiagnosticContext& context,
                                     const StackedEscapeParseDocument& candidate_segment,
                                     StringLiteral binary_source)
    {
        if (!Strings::starts_with(candidate_segment.text(), "https://") &&
            // Allow unencrypted Azurite for testing (not reflected in error msg)
            !Strings::starts_with(candidate_segment.text(), "http://127.0.0.1"))
        {
            candidate_segment.report_error_with_caret_line(context,
                                                           msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                                       msg::base_url = "https://",
                                                                       msg::binary_source = binary_source));
            return false;
        }

        return true;
    }

    static bool parse_object_storage_provider(DiagnosticContext& context,
                                              ParseEnumerator& e,
                                              char32_t matched_terminal,
                                              const StackedEscapeParseDocument& kind,
                                              BinaryCacheParsedConfigs& result,
                                              BinaryCacheProviderKind provider_kind,
                                              StringLiteral binary_source,
                                              StringLiteral expected_base_url)
    {
        // match <provider>,<prefix>[,<rw>]
        if (matched_terminal != ',')
        {
            kind.report_error_with_caret_line_end_delimiter(context,
                                                            msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                                        msg::base_url = expected_base_url,
                                                                        msg::binary_source = binary_source));
            return false;
        }

        auto maybe_prefix = e.match_escaped(context, matched_terminal, '`', ",;");
        auto prefix = maybe_prefix.get();
        if (!prefix)
        {
            return false;
        }

        if (!Strings::starts_with(prefix->text(), expected_base_url))
        {
            prefix->report_error_with_caret_line(context,
                                                 msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                             msg::base_url = expected_base_url,
                                                             msg::binary_source = binary_source));
            return false;
        }

        auto maybe_access = parse_access_terminal(
            context, e, matched_terminal, binary_source, msgInvalidArgumentRequiresOneOrTwoArguments);
        const auto access = maybe_access.get();
        if (!access)
        {
            return false;
        }

        auto normalized_prefix = prefix->move_text();
        if (normalized_prefix.back() != '/')
        {
            normalized_prefix.push_back('/');
        }

        result.providers.push_back({provider_kind, *access, std::move(normalized_prefix), nullopt, nullopt});
        result.telemetry_tags.insert(binary_source);
        return true;
    }

    static bool parse_azure_base_url_and_token_provider(DiagnosticContext& context,
                                                        ParseEnumerator& e,
                                                        char32_t& matched_terminal,
                                                        const StackedEscapeParseDocument& kind,
                                                        BinaryCacheParsedConfigs& result,
                                                        BinaryCacheProviderKind provider_kind,
                                                        StringLiteral binary_source,
                                                        StringLiteral telemetry_tag)
    {
        // match <provider>,<baseurl>,<sas>[,...]
        if (matched_terminal != ',')
        {
            kind.report_error_with_caret_line_end_delimiter(
                context, msg::format(msgInvalidArgumentRequiresBaseUrlAndToken, msg::binary_source = binary_source));
            return false;
        }

        auto maybe_baseuri = e.match_escaped(context, matched_terminal, '`', ",;");
        auto baseuri = maybe_baseuri.get();
        if (!baseuri)
        {
            return false;
        }

        if (matched_terminal != ',')
        {
            baseuri->report_error_with_caret_line_end_delimiter(
                context, msg::format(msgInvalidArgumentRequiresBaseUrlAndToken, msg::binary_source = binary_source));
            return false;
        }

        if (!check_azure_base_url(context, *baseuri, binary_source))
        {
            return false;
        }

        auto maybe_sas = e.match_escaped(context, matched_terminal, '`', ",;");
        auto sas = maybe_sas.get();
        if (!sas)
        {
            return false;
        }

        if (sas->text().empty() || sas->text()[0] == '?')
        {
            sas->report_error_with_caret_line(
                context, msg::format(msgInvalidArgumentRequiresValidToken, msg::binary_source = binary_source));
            return false;
        }

        auto maybe_access = parse_access_terminal(
            context, e, matched_terminal, binary_source, msgInvalidArgumentRequiresTwoOrThreeArguments);
        const auto access = maybe_access.get();
        if (!access)
        {
            return false;
        }

        result.providers.push_back({provider_kind, *access, baseuri->move_text(), sas->move_text(), nullopt});
        result.telemetry_tags.insert(telemetry_tag);
        return true;
    }

    static Optional<std::string> parse_azure_base_url(DiagnosticContext& context,
                                                      ParseEnumerator& e,
                                                      char32_t& matched_terminal,
                                                      const StackedEscapeParseDocument& kind,
                                                      StringLiteral binary_source)
    {
        // match <provider>,<baseurl>[,...]
        if (matched_terminal != ',')
        {
            kind.report_error_with_caret_line_end_delimiter(context,
                                                            msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                                        msg::base_url = "https://",
                                                                        msg::binary_source = binary_source));
            return nullopt;
        }

        auto maybe_baseuri = e.match_escaped(context, matched_terminal, '`', ",;");
        auto baseuri = maybe_baseuri.get();
        if (!baseuri)
        {
            return nullopt;
        }

        if (!check_azure_base_url(context, *baseuri, binary_source))
        {
            return nullopt;
        }

        return baseuri->move_text();
    }

    static Optional<BinaryCacheAccess> parse_access_value(DiagnosticContext& context,
                                                          const StackedEscapeParseDocument& access)
    {
        if (access.text() == "readwrite")
        {
            return BinaryCacheAccess::ReadWrite;
        }
        else if (access.text() == "read")
        {
            return BinaryCacheAccess::Read;
        }
        else if (access.text() == "write")
        {
            return BinaryCacheAccess::Write;
        }

        access.report_error_with_caret_line(context, msg::format(msgExpectedReadWriteReadWrite));
        return nullopt;
    }

    static bool parse_binary_provider_configs_append(DiagnosticContext& context,
                                                     BinaryCacheParsedConfigs& result,
                                                     const Path& default_cache_path,
                                                     const std::string& input_text,
                                                     Optional<StringView> origin)
    {
        ParsedDocument doc{input_text, origin};
        auto e = doc.enumerator();
        while (!e.at_eof())
        {
            char32_t matched_terminal;
            auto maybe_kind = e.match_escaped(context, matched_terminal, '`', ",;");
            const auto kind = maybe_kind.get();
            if (!kind)
            {
                return false;
            }

            if (kind->text().empty() && matched_terminal == ';')
            {
                // allow and ignore empty ; segments
                continue;
            }

            if (kind->text() == "clear")
            {
                if (matched_terminal == ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgInvalidArgumentRequiresNoneArguments, msg::binary_source = "clear"));
                    return false;
                }

                result.providers.clear();
                result.telemetry_tags.clear();
                continue;
            }

            if (kind->text() == "files")
            {
                if (!parse_absolute_path_provider(
                        context, e, matched_terminal, *kind, result, BinaryCacheProviderKind::Files, "files", "files"))
                {
                    return false;
                }

                continue;
            }

            if (kind->text() == "nugetconfig")
            {
                if (!parse_absolute_path_provider(context,
                                                  e,
                                                  matched_terminal,
                                                  *kind,
                                                  result,
                                                  BinaryCacheProviderKind::NuGetConfig,
                                                  "nugetconfig",
                                                  "nuget"))
                {
                    return false;
                }

                continue;
            }

            if (kind->text() == "nuget")
            {
                // nuget,<source>[,<rw>]
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgInvalidArgumentRequiresSourceArgument, msg::binary_source = "nuget"));
                    return false;
                }

                auto maybe_source = e.match_escaped(context, matched_terminal, '`', ",;");
                const auto source = maybe_source.get();
                if (!source)
                {
                    return false;
                }

                auto maybe_access = parse_access_terminal(
                    context, e, matched_terminal, "nuget", msgInvalidArgumentRequiresOneOrTwoArguments);
                const auto access = maybe_access.get();
                if (!access)
                {
                    return false;
                }

                result.providers.push_back(
                    {BinaryCacheProviderKind::NuGet, *access, source->move_text(), nullopt, nullopt});
                result.telemetry_tags.insert("nuget");
                continue;
            }

            if (kind->text() == "nugettimeout")
            {
                // nugettimeout,<seconds>
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgNuGetTimeoutExpectsSinglePositiveInteger));
                    return false;
                }

                auto maybe_timeout = e.match_escaped(context, matched_terminal, '`', ",;");
                const auto timeout = maybe_timeout.get();
                if (!timeout)
                {
                    return false;
                }

                if (matched_terminal == ',')
                {
                    timeout->report_error_with_caret_line_end_delimiter(
                        context, msg::format(msgNuGetTimeoutExpectsSinglePositiveInteger));
                    return false;
                }

                auto timeout_enumerator = timeout->enumerator();
                auto timeout_digits = timeout_enumerator.match_while_ascii(ParserBase::is_ascii_digit);
                if (!timeout_enumerator.at_eof() || timeout_digits.empty())
                {
                    timeout_enumerator.report_error_with_caret_line(
                        context, msg::format(msgNuGetTimeoutExpectsSinglePositiveInteger));
                    return false;
                }

                auto maybe_seconds = Strings::strto<long>(timeout_digits);
                auto seconds = maybe_seconds.get();
                if (!seconds || *seconds <= 0)
                {
                    timeout->report_error_with_caret_line(context,
                                                          msg::format(msgNuGetTimeoutExpectsSinglePositiveInteger));
                    return false;
                }

                result.nuget_timeout = *seconds;
                continue;
            }

            if (kind->text() == "interactive")
            {
                if (matched_terminal == ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresNoneArguments, msg::binary_source = "interactive"));
                    return false;
                }

                result.nuget_interactive = true;
                continue;
            }

            if (kind->text() == "default")
            {
                // default[,<rw>]
                auto maybe_access = parse_access_terminal(
                    context, e, matched_terminal, "default", msgInvalidArgumentRequiresSingleArgument);
                auto access = maybe_access.get();
                if (!access)
                {
                    return false;
                }

                result.providers.push_back(
                    {BinaryCacheProviderKind::Files, *access, default_cache_path.native(), nullopt, nullopt});
                result.telemetry_tags.insert("default");
                continue;
            }

            if (kind->text() == "x-azblob")
            {
                // x-azblob,<baseurl>,<sas>[,<rw>]
                if (!parse_azure_base_url_and_token_provider(context,
                                                             e,
                                                             matched_terminal,
                                                             *kind,
                                                             result,
                                                             BinaryCacheProviderKind::AzBlob,
                                                             "azblob",
                                                             "azblob"))
                {
                    return false;
                }
                continue;
            }

            if (kind->text() == "x-gcs")
            {
                // x-gcs,<prefix>[,<rw>]
                if (!parse_object_storage_provider(
                        context, e, matched_terminal, *kind, result, BinaryCacheProviderKind::GCS, "gcs", "gs://"))
                {
                    return false;
                }

                continue;
            }

            if (kind->text() == "x-aws")
            {
                // x-aws,<prefix>[,<rw>]
                if (!parse_object_storage_provider(
                        context, e, matched_terminal, *kind, result, BinaryCacheProviderKind::AWS, "aws", "s3://"))
                {
                    return false;
                }

                continue;
            }

            if (kind->text() == "x-aws-config")
            {
                // x-aws-config,setting
                // (only "no-sign-request" is currently accepted as a setting)
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresSingleStringArgument,
                                    msg::binary_source = "x-aws-config"));
                    return false;
                }

                auto maybe_setting = e.match_escaped(context, matched_terminal, '`', ",;");
                auto setting = maybe_setting.get();
                if (!setting)
                {
                    return false;
                }

                if (matched_terminal == ',')
                {
                    setting->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresSingleStringArgument,
                                    msg::binary_source = "x-aws-config"));
                    return false;
                }

                if (setting->text() != "no-sign-request")
                {
                    setting->report_error_with_caret_line(context, msg::format(msgInvalidArgument));
                    return false;
                }

                result.aws_no_sign_request = true;
                continue;
            }

            if (kind->text() == "x-cos")
            {
                // x-cos,<prefix>[,<rw>]
                if (!parse_object_storage_provider(
                        context, e, matched_terminal, *kind, result, BinaryCacheProviderKind::COS, "cos", "cos://"))
                {
                    return false;
                }

                continue;
            }

            if (kind->text() == "x-gha")
            {
                WarningDiagnosticContext wdc{context};
                kind->report_error_with_caret_line(
                    wdc, msg::format(msgGhaBinaryCacheDeprecated, msg::url = docs::binarycaching_url));
                while (matched_terminal == ',')
                {
                    if (!e.match_escaped(context, matched_terminal, '`', ",;").has_value())
                    {
                        return false;
                    }
                }

                continue;
            }

            if (kind->text() == "http")
            {
                // http,<url_template>[,<rw>[,<header>]]
                // plus URL template validation stuff from above
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(context,
                                                                     msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                                                 msg::base_url = "https://",
                                                                                 msg::binary_source = "http"));
                    return false;
                }

                auto maybe_url = e.match_escaped(context, matched_terminal, '`', ",;");
                auto url = maybe_url.get();
                if (!url)
                {
                    return false;
                }

                if (!Strings::starts_with(url->text(), "http://") && !Strings::starts_with(url->text(), "https://"))
                {
                    url->report_error_with_caret_line(context,
                                                      msg::format(msgInvalidArgumentRequiresBaseUrl,
                                                                  msg::base_url = "https://",
                                                                  msg::binary_source = "http"));
                    return false;
                }

                auto maybe_url_template = validate_url_template(context, *url);
                auto url_template = maybe_url_template.get();
                if (!url_template)
                {
                    return false;
                }

                if (!url_template->has_sha)
                {
                    if (url_template->has_other)
                    {
                        url->report_error_with_caret_line(context, msg::format(msgMissingShaVariable));
                        return false;
                    }

                    if (url_template->url_template.back() != '/')
                    {
                        url_template->url_template.push_back('/');
                    }

                    url_template->url_template.append("{sha}.zip");
                }

                BinaryCacheAccess access = BinaryCacheAccess::ReadWrite;
                Optional<std::string> header;
                if (matched_terminal == ',')
                {
                    auto maybe_access = e.match_escaped(context, matched_terminal, '`', ",;");
                    auto access_text = maybe_access.get();
                    if (!access_text)
                    {
                        return false;
                    }

                    if (access_text->text() == "readwrite")
                    {
                        access = BinaryCacheAccess::ReadWrite;
                    }
                    else if (access_text->text() == "read")
                    {
                        access = BinaryCacheAccess::Read;
                    }
                    else if (access_text->text() == "write")
                    {
                        access = BinaryCacheAccess::Write;
                    }
                    else
                    {
                        access_text->report_error_with_caret_line(context, msg::format(msgExpectedReadWriteReadWrite));
                        return false;
                    }

                    if (matched_terminal == ',')
                    {
                        auto maybe_header = e.match_escaped(context, matched_terminal, '`', ",;");
                        auto parsed_header = maybe_header.get();
                        if (!parsed_header)
                        {
                            return false;
                        }

                        if (matched_terminal == ',')
                        {
                            parsed_header->report_error_with_caret_line_end_delimiter(
                                context,
                                msg::format(msgInvalidArgumentRequiresTwoOrThreeArguments,
                                            msg::binary_source = "http"));
                            return false;
                        }

                        header.emplace(parsed_header->move_text());
                    }
                }

                result.providers.push_back({BinaryCacheProviderKind::Http,
                                            access,
                                            std::move(url_template->url_template),
                                            std::move(header),
                                            nullopt});
                result.telemetry_tags.insert("http");
                continue;
            }

            if (kind->text() == "x-az-universal")
            {
                // x-az-universal,<organization>,<project>,<feed>[,<rw>]
                if (matched_terminal != ',')
                {
                    kind->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresFourOrFiveArguments,
                                    msg::binary_source = "Universal Packages"));
                    return false;
                }

                auto maybe_organization = e.match_escaped(context, matched_terminal, '`', ",;");
                auto organization = maybe_organization.get();
                if (!organization)
                {
                    return false;
                }

                if (matched_terminal != ',')
                {
                    organization->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresFourOrFiveArguments,
                                    msg::binary_source = "Universal Packages"));
                    return false;
                }

                auto maybe_project = e.match_escaped(context, matched_terminal, '`', ",;");
                auto project = maybe_project.get();
                if (!project)
                {
                    return false;
                }

                if (matched_terminal != ',')
                {
                    project->report_error_with_caret_line_end_delimiter(
                        context,
                        msg::format(msgInvalidArgumentRequiresFourOrFiveArguments,
                                    msg::binary_source = "Universal Packages"));
                    return false;
                }

                auto maybe_feed = e.match_escaped(context, matched_terminal, '`', ",;");
                auto feed = maybe_feed.get();
                if (!feed)
                {
                    return false;
                }

                BinaryCacheAccess access = BinaryCacheAccess::ReadWrite;
                if (matched_terminal == ',')
                {
                    auto maybe_access_doc = e.match_escaped(context, matched_terminal, '`', ",;");
                    const auto access_doc = maybe_access_doc.get();
                    if (!access_doc)
                    {
                        return false;
                    }

                    if (matched_terminal == ',')
                    {
                        access_doc->report_error_with_caret_line_end_delimiter(
                            context,
                            msg::format(msgInvalidArgumentRequiresFourOrFiveArguments,
                                        msg::binary_source = "Universal Packages"));
                        return false;
                    }

                    auto maybe_access = parse_access_value(context, *access_doc);
                    const auto parsed_access = maybe_access.get();
                    if (!parsed_access)
                    {
                        return false;
                    }

                    access = *parsed_access;
                }

                result.providers.push_back({BinaryCacheProviderKind::AzUniversal,
                                            access,
                                            organization->move_text(),
                                            project->move_text(),
                                            feed->move_text()});
                result.telemetry_tags.insert("upkg");
                continue;
            }

            if (kind->text() == "x-azcopy")
            {
                // x-azcopy,<baseurl>[,<rw>]
                auto maybe_base_url = parse_azure_base_url(context, e, matched_terminal, *kind, "x-azcopy");
                auto base_url = maybe_base_url.get();
                if (!base_url)
                {
                    return false;
                }

                BinaryCacheAccess access = BinaryCacheAccess::ReadWrite;
                if (matched_terminal == ',')
                {
                    auto maybe_access_doc = e.match_escaped(context, matched_terminal, '`', ",;");
                    const auto access_doc = maybe_access_doc.get();
                    if (!access_doc)
                    {
                        return false;
                    }

                    if (matched_terminal == ',')
                    {
                        e.report_error_with_caret_line(
                            context,
                            msg::format(msgInvalidArgumentRequiresOneOrTwoArguments, msg::binary_source = "x-azcopy"));
                        return false;
                    }

                    auto maybe_access = parse_access_value(context, *access_doc);
                    const auto parsed_access = maybe_access.get();
                    if (!parsed_access)
                    {
                        return false;
                    }

                    access = *parsed_access;
                }

                result.providers.push_back(
                    {BinaryCacheProviderKind::AzCopy, access, std::move(*base_url), nullopt, nullopt});
                result.telemetry_tags.insert("azcopy");
                continue;
            }

            if (kind->text() == "x-azcopy-sas")
            {
                // x-azcopy-sas,<baseurl>,<sas>[,<rw>]
                if (!parse_azure_base_url_and_token_provider(context,
                                                             e,
                                                             matched_terminal,
                                                             *kind,
                                                             result,
                                                             BinaryCacheProviderKind::AzCopySas,
                                                             "x-azcopy-sas",
                                                             "azcopy-sas"))
                {
                    return false;
                }
                continue;
            }

            kind->report_error_with_caret_line(context, msg::format(msgUnknownBinaryProviderType));
            return false;
        }

        return true;
    }

    Optional<BinaryCacheParsedConfigs> parse_binary_provider_configs(DiagnosticContext& context,
                                                                     const Path& default_cache_path,
                                                                     const std::string& env_string,
                                                                     View<std::string> args)
    {
        Optional<BinaryCacheParsedConfigs> out;
        auto& result = out.emplace();

        result.providers.push_back(
            {BinaryCacheProviderKind::Files, BinaryCacheAccess::ReadWrite, default_cache_path.native(), nullopt});
        result.telemetry_tags.insert("default");
        const auto binary_sources_origin = format_environment_variable("VCPKG_BINARY_SOURCES").to_string();
        if (!parse_binary_provider_configs_append(
                context, result, default_cache_path, env_string, binary_sources_origin))
        {
            out.clear();
            return out;
        }

        for (const auto& arg : args)
        {
            if (!parse_binary_provider_configs_append(context, result, default_cache_path, arg, nullopt))
            {
                out.clear();
                return out;
            }
        }

        return out;
    }

    std::string format_version_for_feedref(StringView version_text, StringView abi_tag)
    {
        // this cannot use DotVersion::try_parse or DateVersion::try_parse,
        // since this is a subtly different algorithm
        // and ignores random extra stuff from the end

        ParsedExternalVersion parsed_version;
        if (try_extract_external_date_version(parsed_version, version_text))
        {
            parsed_version.normalize();
            return fmt::format(
                "{}.{}.{}-vcpkg{}", parsed_version.major, parsed_version.minor, parsed_version.patch, abi_tag);
        }

        if (!version_text.empty() && version_text[0] == 'v')
        {
            version_text = version_text.substr(1);
        }
        if (try_extract_external_dot_version(parsed_version, version_text))
        {
            parsed_version.normalize();
            return fmt::format(
                "{}.{}.{}-vcpkg{}", parsed_version.major, parsed_version.minor, parsed_version.patch, abi_tag);
        }

        return Strings::concat("0.0.0-vcpkg", abi_tag);
    }

    std::string generate_nuspec(const Path& package_dir,
                                const InstallPlanAction& action,
                                StringView id_prefix,
                                const NuGetRepoInfo& rinfo)
    {
        auto& spec = action.spec;
        auto& scf = *action.source_control_file_and_location().source_control_file;
        auto& version = scf.core_paragraph->version;
        const auto& abi_info = action.abi_info.value_or_exit(VCPKG_LINE_INFO);
        Checks::check_exit(VCPKG_LINE_INFO, abi_info.compiler_info != nullptr);
        const auto& compiler_info = *abi_info.compiler_info;
        Checks::check_exit(VCPKG_LINE_INFO, abi_info.triplet_abi != nullptr);
        auto ref = make_nugetref(action, id_prefix);
        std::string description =
            Strings::concat("NOT FOR DIRECT USE. Automatically generated cache package.\n\n",
                            Strings::join("\n    ", scf.core_paragraph->description),
                            "\n\nVersion: ",
                            version,
                            "\nTriplet: ",
                            spec.triplet().to_string(),
                            "\nCXX Compiler id: ",
                            compiler_info.id,
                            "\nCXX Compiler version: ",
                            compiler_info.version,
                            "\nTriplet/Compiler hash: ",
                            *abi_info.triplet_abi,
                            "\nFeatures:",
                            Strings::join(",", action.feature_list, [](const std::string& s) { return " " + s; }),
                            "\nDependencies:\n");

        for (auto&& dep : action.package_dependencies)
        {
            Strings::append(description, "    ", dep.name(), '\n');
        }

        XmlSerializer xml;
        xml.open_tag("package").line_break();
        xml.open_tag("metadata").line_break();
        xml.simple_tag("id", ref.id).line_break();
        xml.simple_tag("version", ref.version).line_break();
        if (!scf.core_paragraph->homepage.empty())
        {
            xml.simple_tag("projectUrl", scf.core_paragraph->homepage);
        }

        xml.simple_tag("authors", "vcpkg").line_break();
        xml.simple_tag("description", description).line_break();
        xml.open_tag("packageTypes");
        xml.start_complex_open_tag("packageType").text_attr("name", "vcpkg").finish_self_closing_complex_tag();
        xml.close_tag("packageTypes").line_break();
        if (!rinfo.repo.empty())
        {
            xml.start_complex_open_tag("repository").text_attr("type", "git").text_attr("url", rinfo.repo);
            if (!rinfo.branch.empty())
            {
                xml.text_attr("branch", rinfo.branch);
            }

            if (!rinfo.commit.empty())
            {
                xml.text_attr("commit", rinfo.commit);
            }

            xml.finish_self_closing_complex_tag().line_break();
        }

        xml.close_tag("metadata").line_break();
        xml.open_tag("files");
        xml.start_complex_open_tag("file")
            .text_attr("src", package_dir / "**")
            .text_attr("target", "")
            .finish_self_closing_complex_tag();
        xml.close_tag("files").line_break();
        xml.close_tag("package").line_break();
        return std::move(xml.buf);
    }

    LocalizedString format_help_topic_asset_caching()
    {
        HelpTableFormatter table;
        table.format("clear", msg::format(msgHelpCachingClear));
        table.format("x-azurl,<url>[,<sas>[,<rw>]]", msg::format(msgHelpAssetCachingAzUrl));
        table.format("x-script,<template>", msg::format(msgHelpAssetCachingScript));
        table.format("x-block-origin", msg::format(msgHelpAssetCachingBlockOrigin));
        return msg::format(msgHelpAssetCaching)
            .append_raw('\n')
            .append_raw(table.m_str)
            .append_raw('\n')
            .append(msgExtendedDocumentationAtUrl, msg::url = docs::assetcaching_url);
    }

    LocalizedString format_help_topic_binary_caching()
    {
        HelpTableFormatter table;

        // General sources:
        table.format("clear", msg::format(msgHelpCachingClear));
        SinkBufferedDiagnosticContext sdc{null_sink};
        auto p = default_cache_path(sdc);
        if (p)
        {
            table.format("default[,<rw>]", msg::format(msgHelpBinaryCachingDefaults, msg::path = *p.get()));
        }
        else
        {
            table.format("default[,<rw>]", msg::format(msgHelpBinaryCachingDefaultsError));
        }

        table.format("files,<path>[,<rw>]", msg::format(msgHelpBinaryCachingFiles));
        table.format("http,<url_template>[,<rw>[,<header>]]", msg::format(msgHelpBinaryCachingHttp));
        table.format("x-azblob,<url>,<sas>[,<rw>]", msg::format(msgHelpBinaryCachingAzBlob));
        table.format("x-gcs,<prefix>[,<rw>]", msg::format(msgHelpBinaryCachingGcs));
        table.format("x-cos,<prefix>[,<rw>]", msg::format(msgHelpBinaryCachingCos));
        table.format("x-az-universal,<organization>,<project>,<feed>[,<rw>]", msg::format(msgHelpBinaryCachingAzUpkg));
        table.blank();

        // NuGet sources:
        table.header(msg::format(msgHelpBinaryCachingNuGetHeader));
        table.format("nuget,<uri>[,<rw>]", msg::format(msgHelpBinaryCachingNuGet));
        table.format("nugetconfig,<path>[,<rw>]", msg::format(msgHelpBinaryCachingNuGetConfig));
        table.format("nugettimeout,<seconds>", msg::format(msgHelpBinaryCachingNuGetTimeout));
        table.format("interactive", msg::format(msgHelpBinaryCachingNuGetInteractive));
        table.text(msg::format(msgHelpBinaryCachingNuGetFooter), 2);
        table.text("\n<repository type=\"git\" url=\"${VCPKG_NUGET_REPOSITORY}\"/>\n"
                   "<repository type=\"git\"\n"
                   "            url=\"${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}.git\"\n"
                   "            branch=\"${GITHUB_REF}\"\n"
                   "            commit=\"${GITHUB_SHA}\"/>",
                   4);
        table.blank();

        // AWS sources:
        table.blank();
        table.header(msg::format(msgHelpBinaryCachingAwsHeader));
        table.format("x-aws,<prefix>[,<rw>]", msg::format(msgHelpBinaryCachingAws));
        table.format("x-aws-config,<parameter>", msg::format(msgHelpBinaryCachingAwsConfig));

        return msg::format(msgHelpBinaryCaching)
            .append_raw('\n')
            .append_raw(table.m_str)
            .append_raw('\n')
            .append(msgExtendedDocumentationAtUrl, msg::url = docs::binarycaching_url);
    }

    std::string generate_nuget_packages_config(const ActionPlan& plan, StringView prefix)
    {
        XmlSerializer xml;
        xml.emit_declaration().line_break();
        xml.open_tag("packages").line_break();
        for (auto&& action : plan.install_actions)
        {
            auto ref = make_nugetref(action, prefix);
            xml.start_complex_open_tag("package")
                .text_attr("id", ref.id)
                .text_attr("version", ref.version)
                .finish_self_closing_complex_tag()
                .line_break();
        }

        xml.close_tag("packages").line_break();
        return std::move(xml.buf);
    }

    FeedReference make_nugetref(const InstallPlanAction& action, StringView prefix)
    {
        return ::make_feedref(
            action.spec, action.version, action.abi_info.value_or_exit(VCPKG_LINE_INFO).package_abi, prefix);
    }

    std::vector<std::vector<std::string>> batch_command_arguments_with_fixed_length(
        const std::vector<std::string>& entries,
        const std::size_t reserved_len,
        const std::size_t max_len,
        const std::size_t fixed_len,
        const std::size_t separator_len)
    {
        const auto available_len = static_cast<ptrdiff_t>(max_len) - reserved_len;

        // Not enough space for even one entry
        if (available_len < fixed_len) return {};

        const size_t entries_per_batch = 1 + (available_len - fixed_len) / (fixed_len + separator_len);

        auto first = entries.begin();
        const auto last = entries.end();
        std::vector<std::vector<std::string>> batches;
        while (first != last)
        {
            auto end_of_batch = first + std::min(static_cast<size_t>(last - first), entries_per_batch);
            batches.emplace_back(first, end_of_batch);
            first = end_of_batch;
        }
        return batches;
    }
}
