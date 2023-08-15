#include <vcpkg/base/cache.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/jsonreader.h>
#include <vcpkg/base/messages.h>
#include <vcpkg/base/system.debug.h>
#include <vcpkg/base/util.h>

#include <vcpkg/documentation.h>
#include <vcpkg/metrics.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/registries.h>
#include <vcpkg/sourceparagraph.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>
#include <vcpkg/versiondeserializers.h>
#include <vcpkg/versions.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace vcpkg;

    struct GitTreeStringDeserializer : Json::StringDeserializer
    {
        LocalizedString type_name() const override { return msg::format(msgAGitObjectSha); }

        static const GitTreeStringDeserializer instance;
    };
    const GitTreeStringDeserializer GitTreeStringDeserializer::instance;

    struct RegistryPathStringDeserializer : Json::StringDeserializer
    {
        LocalizedString type_name() const override { return msg::format(msgARegistryPath); }

        static const RegistryPathStringDeserializer instance;
    };
    const RegistryPathStringDeserializer RegistryPathStringDeserializer::instance;

    struct VersionDbEntryDeserializer final : Json::IDeserializer<VersionDbEntry>
    {
        static constexpr StringLiteral GIT_TREE = "git-tree";
        static constexpr StringLiteral PATH = "path";

        LocalizedString type_name() const override;
        View<StringView> valid_fields() const override;
        Optional<VersionDbEntry> visit_object(Json::Reader& r, const Json::Object& obj) const override;
        VersionDbEntryDeserializer(VersionDbType type, const Path& root) : type(type), registry_root(root) { }

    private:
        VersionDbType type;
        Path registry_root;
    };
    constexpr StringLiteral VersionDbEntryDeserializer::GIT_TREE;
    constexpr StringLiteral VersionDbEntryDeserializer::PATH;
    LocalizedString VersionDbEntryDeserializer::type_name() const { return msg::format(msgAVersionDatabaseEntry); }
    View<StringView> VersionDbEntryDeserializer::valid_fields() const
    {
        static constexpr StringView u_git[] = {GIT_TREE};
        static constexpr StringView u_path[] = {PATH};
        static const auto t_git = vcpkg::Util::Vectors::concat<StringView>(schemed_deserializer_fields(), u_git);
        static const auto t_path = vcpkg::Util::Vectors::concat<StringView>(schemed_deserializer_fields(), u_path);

        return type == VersionDbType::Git ? t_git : t_path;
    }

    Optional<VersionDbEntry> VersionDbEntryDeserializer::visit_object(Json::Reader& r, const Json::Object& obj) const
    {
        VersionDbEntry ret;

        auto schemed_version = visit_required_schemed_deserializer(type_name(), r, obj);
        ret.scheme = schemed_version.scheme;
        ret.version = std::move(schemed_version.version);
        switch (type)
        {
            case VersionDbType::Git:
            {
                r.required_object_field(type_name(), obj, GIT_TREE, ret.git_tree, GitTreeStringDeserializer::instance);
                break;
            }
            case VersionDbType::Filesystem:
            {
                std::string path_res;
                r.required_object_field(type_name(), obj, PATH, path_res, RegistryPathStringDeserializer::instance);
                if (!Strings::starts_with(path_res, "$/"))
                {
                    r.add_generic_error(msg::format(msgARegistryPath),
                                        msg::format(msgARegistryPathMustStartWithDollar));
                    return nullopt;
                }

                if (Strings::contains(path_res, '\\') || Strings::contains(path_res, "//"))
                {
                    r.add_generic_error(msg::format(msgARegistryPath),
                                        msg::format(msgARegistryPathMustBeDelimitedWithForwardSlashes));
                    return nullopt;
                }

                auto first = path_res.begin();
                const auto last = path_res.end();
                for (std::string::iterator candidate;; first = candidate)
                {
                    candidate = std::find(first, last, '/');
                    if (candidate == last)
                    {
                        break;
                    }

                    ++candidate;
                    if (candidate == last)
                    {
                        break;
                    }

                    if (*candidate != '.')
                    {
                        continue;
                    }

                    ++candidate;
                    if (candidate == last || *candidate == '/')
                    {
                        r.add_generic_error(msg::format(msgARegistryPath),
                                            msg::format(msgARegistryPathMustNotHaveDots));
                        return nullopt;
                    }

                    if (*candidate != '.')
                    {
                        first = candidate;
                        continue;
                    }

                    ++candidate;
                    if (candidate == last || *candidate == '/')
                    {
                        r.add_generic_error(msg::format(msgARegistryPath),
                                            msg::format(msgARegistryPathMustNotHaveDots));
                        return nullopt;
                    }
                }

                ret.p = registry_root / StringView{path_res}.substr(2);
                break;
            }
        }

        return ret;
    }

    struct VersionDbEntryArrayDeserializer final : Json::IDeserializer<std::vector<VersionDbEntry>>
    {
        virtual LocalizedString type_name() const override;
        virtual Optional<std::vector<VersionDbEntry>> visit_array(Json::Reader& r,
                                                                  const Json::Array& arr) const override;
        VersionDbEntryArrayDeserializer(VersionDbType type, const Path& root) : underlying{type, root} { }

    private:
        VersionDbEntryDeserializer underlying;
    };
    LocalizedString VersionDbEntryArrayDeserializer::type_name() const { return msg::format(msgAnArrayOfVersions); }

    Optional<std::vector<VersionDbEntry>> VersionDbEntryArrayDeserializer::visit_array(Json::Reader& r,
                                                                                       const Json::Array& arr) const
    {
        return r.array_elements(arr, underlying);
    }

    using Baseline = std::map<std::string, Version, std::less<>>;

    static constexpr StringLiteral registry_versions_dir_name = "versions";

    struct PortVersionsGitTreesStructOfArrays
    {
        PortVersionsGitTreesStructOfArrays() = default;
        PortVersionsGitTreesStructOfArrays(const PortVersionsGitTreesStructOfArrays&) = default;
        PortVersionsGitTreesStructOfArrays(PortVersionsGitTreesStructOfArrays&&) = default;
        PortVersionsGitTreesStructOfArrays& operator=(const PortVersionsGitTreesStructOfArrays&) = default;
        PortVersionsGitTreesStructOfArrays& operator=(PortVersionsGitTreesStructOfArrays&&) = default;

        explicit PortVersionsGitTreesStructOfArrays(std::vector<VersionDbEntry>&& db_entries)
        {
            assign(std::move(db_entries));
        }

        void assign(std::vector<VersionDbEntry>&& db_entries)
        {
            m_port_versions.reserve(db_entries.size());
            m_git_trees.reserve(db_entries.size());
            m_port_versions.clear();
            m_git_trees.clear();
            for (auto& entry : db_entries)
            {
                m_port_versions.push_back(std::move(entry.version));
                m_git_trees.push_back(std::move(entry.git_tree));
            }

            db_entries.clear();
        }

        // these two map port versions to git trees
        // these shall have the same size, and git_trees[i] shall be the git tree for port_versions[i]
        const std::vector<Version>& port_versions() const noexcept { return m_port_versions; }
        const std::vector<std::string>& git_trees() const noexcept { return m_git_trees; }
        const std::string* try_get_git_tree(const Version& version) const noexcept
        {
            auto it = std::find(m_port_versions.begin(), m_port_versions.end(), version);
            if (it != m_port_versions.end())
            {
                return &m_git_trees[it - m_port_versions.begin()];
            }

            return nullptr;
        }

    private:
        std::vector<Version> m_port_versions;
        std::vector<std::string> m_git_trees;
    };

    Path relative_path_to_versions(StringView port_name);
    ExpectedL<Optional<std::vector<VersionDbEntry>>> load_versions_file(const ReadOnlyFilesystem& fs,
                                                                        VersionDbType vdb,
                                                                        const Path& port_versions,
                                                                        StringView port_name,
                                                                        const Path& registry_root = {});

    struct GitRegistry final : RegistryImplementation
    {
        GitRegistry(const VcpkgPaths& paths, std::string&& repo, std::string&& reference, std::string&& baseline)
            : m_paths(paths)
            , m_repo(std::move(repo))
            , m_reference(std::move(reference))
            , m_baseline_identifier(std::move(baseline))
        {
        }

        StringLiteral kind() const override { return "git"; }

        ExpectedL<Optional<PathAndLocation>> get_port(const VersionSpec& spec) const override;

        ExpectedL<Optional<View<Version>>> get_all_port_versions(StringView port_name) const override;

        ExpectedL<Unit> append_all_port_names(std::vector<std::string>&) const override;

        ExpectedL<bool> try_append_all_port_names_no_network(std::vector<std::string>& port_names) const override;

        ExpectedL<Optional<Version>> get_baseline_version(StringView) const override;

    private:
        const ExpectedL<LockFile::Entry>& get_lock_entry() const
        {
            return m_lock_entry.get_lazy(
                [this]() { return m_paths.get_installed_lockfile().get_or_fetch(m_paths, m_repo, m_reference); });
        }

        ExpectedL<Path> get_versions_tree_from_entry(const LockFile::Entry* lock_entry, bool emit_telemetry) const
        {
            auto maybe_tree = m_paths.git_find_object_id_for_remote_registry_path(
                lock_entry->commit_id(), registry_versions_dir_name.to_string());
            auto tree = maybe_tree.get();
            if (!tree)
            {
                if (emit_telemetry)
                {
                    get_global_metrics_collector().track_define(DefineMetric::RegistriesErrorNoVersionsAtCommit);
                }

                return msg::format_error(msgCouldNotFindGitTreeAtCommit,
                                         msg::package_name = m_repo,
                                         msg::commit_sha = lock_entry->commit_id())
                    .append_raw('\n')
                    .append_raw(maybe_tree.error());
            }

            auto maybe_path = m_paths.git_extract_tree_from_remote_registry(*tree);
            auto path = maybe_path.get();
            if (!path)
            {
                return msg::format_error(msgFailedToCheckoutRepo, msg::package_name = m_repo)
                    .append_raw('\n')
                    .append(maybe_path.error());
            }

            return std::move(*path);
        }

        const VcpkgPaths& m_paths;

        std::string m_repo;
        std::string m_reference;
        std::string m_baseline_identifier;
        CacheSingle<ExpectedL<LockFile::Entry>> m_lock_entry;
        CacheSingle<ExpectedL<Path>> m_stale_versions_tree;
        CacheSingle<ExpectedL<Path>> m_versions_tree;
        CacheSingle<ExpectedL<Baseline>> m_baseline;
        Cache<std::string, ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>> m_stale_versions;
        Cache<std::string, ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>> m_live_versions;

        const ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>& get_versions(
            const Cache<std::string, ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>>& cache,
            StringView port_name,
            const Path& vdb_path) const
        {
            return cache.get_lazy(port_name, [&, this]() -> ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>> {
                auto maybe_maybe_version_entries =
                    load_versions_file(m_paths.get_filesystem(), VersionDbType::Git, vdb_path, port_name);
                auto maybe_version_entries = maybe_maybe_version_entries.get();
                if (!maybe_version_entries)
                {
                    return std::move(maybe_maybe_version_entries).error();
                }

                auto version_entries = maybe_version_entries->get();
                if (!version_entries)
                {
                    return Optional<PortVersionsGitTreesStructOfArrays>{};
                }

                return PortVersionsGitTreesStructOfArrays{std::move(*version_entries)};
            });
        }

        const ExpectedL<Path>& get_live_versions_tree_path() const
        {
            return m_versions_tree.get_lazy([&, this]() -> ExpectedL<Path> {
                auto maybe_lock_entry = get_lock_entry();
                auto lock_entry = maybe_lock_entry.get();
                if (!lock_entry)
                {
                    return maybe_lock_entry.error();
                }

                auto maybe_up_to_date = lock_entry->ensure_up_to_date(m_paths);
                if (!maybe_up_to_date)
                {
                    return maybe_up_to_date.error();
                }

                return get_versions_tree_from_entry(lock_entry, true);
            });
        }

        const ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>& get_stale_versions(
            const LockFile::Entry* lock_entry, StringView port_name) const
        {
            if (!lock_entry->stale())
            {
                Checks::unreachable(VCPKG_LINE_INFO, "Non-stale stale versions");
            }

            const auto& maybe_stale_versions_path = m_stale_versions_tree.get_lazy(
                [lock_entry, this]() -> ExpectedL<Path> { return get_versions_tree_from_entry(lock_entry, false); });
            auto stale_versions_path = maybe_stale_versions_path.get();
            if (!stale_versions_path)
            {
                return m_stale_versions.get_lazy(port_name,
                                                 [&]() -> ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>> {
                                                     return maybe_stale_versions_path.error();
                                                 });
            }

            return get_versions(m_stale_versions, port_name, *stale_versions_path);
        }

        const ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>& get_live_versions(StringView port_name) const
        {
            const auto& maybe_live_vdb = get_live_versions_tree_path();
            auto live_vdb = maybe_live_vdb.get();
            if (!live_vdb)
            {
                return m_live_versions.get_lazy(port_name,
                                                [&]() -> ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>> {
                                                    return maybe_live_vdb.error();
                                                });
            }

            return get_versions(m_live_versions, port_name, *live_vdb);
        }

        ExpectedL<Optional<PathAndLocation>> load_git_tree(StringView git_tree) const
        {
            return m_paths.git_extract_tree_from_remote_registry(git_tree).map(
                [&git_tree, this](Path&& p) -> Optional<PathAndLocation> {
                    return PathAndLocation{
                        std::move(p),
                        Strings::concat("git+", m_repo, "@", git_tree),
                    };
                });
        }
    };

    struct FilesystemRegistryEntry
    {
        // these two map port versions to paths
        // these shall have the same size, and paths[i] shall be the path for port_versions[i]
        std::vector<Version> port_versions;
        std::vector<Path> version_paths;
    };

    // This registry implementation is the builtin registry without a baseline
    // that will only consult files in ports
    struct BuiltinFilesRegistry final : RegistryImplementation
    {
        static constexpr StringLiteral s_kind = "builtin-files";

        BuiltinFilesRegistry(const VcpkgPaths& paths)
            : m_fs(paths.get_filesystem()), m_builtin_ports_directory(paths.builtin_ports_directory())
        {
        }

        StringLiteral kind() const override { return s_kind; }

        ExpectedL<Optional<PathAndLocation>> get_port(const VersionSpec& spec) const override;

        ExpectedL<Optional<View<Version>>> get_all_port_versions(StringView port_name) const override;

        ExpectedL<Unit> append_all_port_names(std::vector<std::string>&) const override;

        ExpectedL<bool> try_append_all_port_names_no_network(std::vector<std::string>& port_names) const override;

        ExpectedL<Optional<Version>> get_baseline_version(StringView port_name) const override;

        ~BuiltinFilesRegistry() = default;

        CacheSingle<Baseline> m_baseline;

    private:
        const ExpectedL<std::unique_ptr<SourceControlFile>>& get_scf(StringView port_name, const Path& path) const
        {
            return m_scfs.get_lazy(path, [&, this]() { return Paragraphs::try_load_port(m_fs, port_name, path); });
        }

        const ReadOnlyFilesystem& m_fs;
        const Path m_builtin_ports_directory;
        Cache<Path, ExpectedL<std::unique_ptr<SourceControlFile>>> m_scfs;
        Cache<std::string, Optional<Version>> m_versions;
    };
    constexpr StringLiteral BuiltinFilesRegistry::s_kind;

    // This registry implementation is a builtin registry with a provided
    // baseline that will perform git operations on the root git repo
    struct BuiltinGitRegistry final : RegistryImplementation
    {
        static constexpr StringLiteral s_kind = "builtin-git";

        BuiltinGitRegistry(const VcpkgPaths& paths, std::string&& baseline)
            : m_baseline_identifier(std::move(baseline))
            , m_files_impl(std::make_unique<BuiltinFilesRegistry>(paths))
            , m_paths(paths)
        {
        }

        StringLiteral kind() const override { return s_kind; }

        ExpectedL<Optional<PathAndLocation>> get_port(const VersionSpec& spec) const override;

        ExpectedL<Optional<View<Version>>> get_all_port_versions(StringView port_name) const override;

        ExpectedL<Unit> append_all_port_names(std::vector<std::string>&) const override;

        ExpectedL<bool> try_append_all_port_names_no_network(std::vector<std::string>& port_names) const override;

        ExpectedL<Optional<Version>> get_baseline_version(StringView port_name) const override;

        ~BuiltinGitRegistry() = default;

        std::string m_baseline_identifier;
        CacheSingle<ExpectedL<Baseline>> m_baseline;

    private:
        std::unique_ptr<BuiltinFilesRegistry> m_files_impl;
        Cache<std::string, ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>> m_versions;

        const ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>>& get_versions(StringView port_name) const
        {
            return m_versions.get_lazy(
                port_name, [&port_name, this]() -> ExpectedL<Optional<PortVersionsGitTreesStructOfArrays>> {
                    const auto& fs = m_paths.get_filesystem();

                    auto versions_path = m_paths.builtin_registry_versions / relative_path_to_versions(port_name);
                    auto maybe_maybe_version_entries =
                        load_versions_file(fs, VersionDbType::Git, m_paths.builtin_registry_versions, port_name);
                    auto maybe_version_entries = maybe_maybe_version_entries.get();
                    if (!maybe_version_entries)
                    {
                        return std::move(maybe_maybe_version_entries).error();
                    }

                    auto version_entries = maybe_version_entries->get();
                    if (!version_entries)
                    {
                        return Optional<PortVersionsGitTreesStructOfArrays>();
                    }

                    return PortVersionsGitTreesStructOfArrays(std::move(*version_entries));
                });
        }

        const VcpkgPaths& m_paths;
    };
    constexpr StringLiteral BuiltinGitRegistry::s_kind;

    // This registry entry is a stub that fails on all APIs; this is used in
    // read-only vcpkg if the user has not provided a baseline.
    struct BuiltinErrorRegistry final : RegistryImplementation
    {
        static constexpr StringLiteral s_kind = "builtin-error";

        StringLiteral kind() const override { return s_kind; }

        ExpectedL<Optional<PathAndLocation>> get_port(const VersionSpec&) const override
        {
            return msg::format_error(msgErrorRequireBaseline);
        }

        ExpectedL<Optional<View<Version>>> get_all_port_versions(StringView) const override
        {
            return msg::format_error(msgErrorRequireBaseline);
        }

        ExpectedL<Unit> append_all_port_names(std::vector<std::string>&) const override
        {
            return msg::format_error(msgErrorRequireBaseline);
        }

        ExpectedL<bool> try_append_all_port_names_no_network(std::vector<std::string>&) const override
        {
            return msg::format_error(msgErrorRequireBaseline);
        }

        ExpectedL<Optional<Version>> get_baseline_version(StringView) const override
        {
            return msg::format_error(msgErrorRequireBaseline);
        }

        ~BuiltinErrorRegistry() = default;
    };
    constexpr StringLiteral BuiltinErrorRegistry::s_kind;

    struct FilesystemRegistry final : RegistryImplementation
    {
        FilesystemRegistry(const ReadOnlyFilesystem& fs, Path&& path, std::string&& baseline)
            : m_fs(fs), m_path(std::move(path)), m_baseline_identifier(std::move(baseline))
        {
        }

        StringLiteral kind() const override { return "filesystem"; }

        ExpectedL<Optional<PathAndLocation>> get_port(const VersionSpec& spec) const override;

        ExpectedL<Optional<View<Version>>> get_all_port_versions(StringView port_name) const override;

        ExpectedL<Unit> append_all_port_names(std::vector<std::string>&) const override;

        ExpectedL<bool> try_append_all_port_names_no_network(std::vector<std::string>& port_names) const override;

        ExpectedL<Optional<Version>> get_baseline_version(StringView) const override;

    private:
        const ReadOnlyFilesystem& m_fs;

        Path m_path;
        std::string m_baseline_identifier;
        CacheSingle<ExpectedL<Optional<Baseline>>> m_baseline;
        Cache<std::string, ExpectedL<Optional<FilesystemRegistryEntry>>> m_entries;

        const ExpectedL<Optional<FilesystemRegistryEntry>>& get_entry(StringView port_name) const
        {
            return m_entries.get_lazy(port_name, [&port_name, this]() -> ExpectedL<Optional<FilesystemRegistryEntry>> {
                auto maybe_maybe_version_entries = load_versions_file(
                    m_fs, VersionDbType::Filesystem, m_path / registry_versions_dir_name, port_name, m_path);
                auto maybe_version_entries = maybe_maybe_version_entries.get();
                if (!maybe_version_entries)
                {
                    return std::move(maybe_maybe_version_entries).error();
                }

                auto version_entries = maybe_version_entries->get();
                if (!version_entries)
                {
                    return Optional<FilesystemRegistryEntry>();
                }

                FilesystemRegistryEntry res;
                for (auto&& version_entry : *version_entries)
                {
                    res.port_versions.push_back(std::move(version_entry.version));
                    res.version_paths.push_back(std::move(version_entry.p));
                }

                return Optional<FilesystemRegistryEntry>{std::move(res)};
            });
        }
    };

    // returns nullopt if the baseline is valid, but doesn't contain the specified baseline,
    // or (equivalently) if the baseline does not exist.
    ExpectedL<Optional<Baseline>> parse_baseline_versions(StringView contents, StringView baseline, StringView origin);
    ExpectedL<Optional<Baseline>> load_baseline_versions(const ReadOnlyFilesystem& fs,
                                                         const Path& baseline_path,
                                                         StringView identifier = {});

    ExpectedL<Unit> load_all_port_names_from_registry_versions(std::vector<std::string>& out,
                                                               const ReadOnlyFilesystem& fs,
                                                               const Path& port_versions_path)
    {
        auto maybe_super_directories = fs.try_get_directories_non_recursive(port_versions_path);
        const auto super_directories = maybe_super_directories.get();
        if (!super_directories)
        {
            return std::move(maybe_super_directories.error());
        }

        for (auto&& super_directory : *super_directories)
        {
            auto maybe_files = fs.try_get_regular_files_non_recursive(super_directory);
            const auto files = maybe_files.get();
            if (!files)
            {
                return std::move(maybe_files).error();
            }

            for (auto&& file : *files)
            {
                auto filename = file.filename();
                if (!Strings::case_insensitive_ascii_ends_with(filename, ".json")) continue;

                if (!Strings::ends_with(filename, ".json"))
                {
                    return msg::format_error(msgJsonFileMissingExtension, msg::path = file);
                }

                auto port_name = filename.substr(0, filename.size() - 5);
                if (!Json::IdentifierDeserializer::is_ident(port_name))
                {
                    return msg::format_error(msgInvalidPortVersonName, msg::path = file);
                }

                out.push_back(port_name.to_string());
            }
        }

        return Unit{};
    }

    static ExpectedL<Path> git_checkout_baseline(const VcpkgPaths& paths, StringView commit_sha)
    {
        const Filesystem& fs = paths.get_filesystem();
        const auto destination_parent = paths.baselines_output() / commit_sha;
        auto destination = destination_parent / "baseline.json";
        if (!fs.exists(destination, IgnoreErrors{}))
        {
            const auto destination_tmp = destination_parent / "baseline.json.tmp";
            auto treeish = Strings::concat(commit_sha, ":versions/baseline.json");
            auto maybe_contents = paths.git_show(treeish, paths.root / ".git");
            if (auto contents = maybe_contents.get())
            {
                std::error_code ec;
                fs.create_directories(destination_parent, ec);
                if (ec)
                {
                    return {msg::format(msg::msgErrorMessage)
                                .append(format_filesystem_call_error(ec, "create_directories", {destination_parent}))
                                .append_raw('\n')
                                .append(msg::msgNoteMessage)
                                .append(msgWhileCheckingOutBaseline, msg::commit_sha = commit_sha),
                            expected_right_tag};
                }
                fs.write_contents(destination_tmp, *contents, ec);
                if (ec)
                {
                    return {msg::format(msg::msgErrorMessage)
                                .append(format_filesystem_call_error(ec, "write_contents", {destination_tmp}))
                                .append_raw('\n')
                                .append(msg::msgNoteMessage)
                                .append(msgWhileCheckingOutBaseline, msg::commit_sha = commit_sha),
                            expected_right_tag};
                }
                fs.rename(destination_tmp, destination, ec);
                if (ec)
                {
                    return {msg::format(msg::msgErrorMessage)
                                .append(format_filesystem_call_error(ec, "rename", {destination_tmp, destination}))
                                .append_raw('\n')
                                .append(msg::msgNoteMessage)
                                .append(msgWhileCheckingOutBaseline, msg::commit_sha = commit_sha),
                            expected_right_tag};
                }
            }
            else
            {
                return {msg::format_error(msgBaselineGitShowFailed, msg::commit_sha = commit_sha)
                            .append_raw('\n')
                            .append(maybe_contents.error()),
                        expected_right_tag};
            }
        }

        return destination;
    }

    // { RegistryImplementation

    // { BuiltinFilesRegistry::RegistryImplementation
    ExpectedL<Optional<PathAndLocation>> BuiltinFilesRegistry::get_port(const VersionSpec& spec) const
    {
        auto port_directory = m_builtin_ports_directory / spec.port_name;
        const auto& maybe_maybe_scf = get_scf(spec.port_name, port_directory);
        const auto maybe_scf = maybe_maybe_scf.get();
        if (!maybe_scf)
        {
            return maybe_maybe_scf.error();
        }

        auto scf = maybe_scf->get();
        if (!scf)
        {
            return Optional<PathAndLocation>();
        }

        if (scf->core_paragraph->name != spec.port_name)
        {
            return msg::format_error(msgUnexpectedPortName,
                                     msg::expected = scf->core_paragraph->name,
                                     msg::actual = spec.port_name,
                                     msg::path = port_directory);
        }

        auto actual_version = scf->core_paragraph->to_version();
        if (actual_version != spec.version)
        {
            msg::println_warning(msgVersionBuiltinPortTreeEntryMissing,
                                 msg::package_name = spec.port_name,
                                 msg::expected = spec.version,
                                 msg::actual = actual_version);
            return Optional<PathAndLocation>();
        }

        return PathAndLocation{port_directory, "git+https://github.com/Microsoft/vcpkg#ports/" + spec.port_name};
    }

    ExpectedL<Optional<View<Version>>> BuiltinFilesRegistry::get_all_port_versions(StringView port_name) const
    {
        auto port_directory = m_builtin_ports_directory / port_name;
        const auto& maybe_maybe_scf = get_scf(port_name, port_directory);
        const auto maybe_scf = maybe_maybe_scf.get();
        if (!maybe_scf)
        {
            return ExpectedL<Optional<View<Version>>>{maybe_maybe_scf.error(), expected_right_tag};
        }

        auto scf = maybe_scf->get();
        if (!scf)
        {
            return Optional<View<Version>>();
        }

        const auto& version = m_versions.get_lazy(
            port_name, [&, this]() -> Optional<Version> { return scf->core_paragraph->to_version(); });

        return View<Version>{&version.value_or_exit(VCPKG_LINE_INFO), 1};
    }

    ExpectedL<Optional<Version>> BuiltinFilesRegistry::get_baseline_version(StringView port_name) const
    {
        // if a baseline is not specified, use the ports directory version
        const auto& maybe_maybe_scf = get_scf(port_name, m_builtin_ports_directory / port_name);
        auto maybe_scf = maybe_maybe_scf.get();
        if (!maybe_scf)
        {
            return maybe_maybe_scf.error();
        }

        auto scf = maybe_scf->get();
        if (!scf)
        {
            return Optional<Version>();
        }

        return scf->to_version();
    }

    ExpectedL<Unit> BuiltinFilesRegistry::append_all_port_names(std::vector<std::string>& out) const
    {
        auto maybe_port_directories = m_fs.try_get_directories_non_recursive(m_builtin_ports_directory);
        if (auto port_directories = maybe_port_directories.get())
        {
            for (auto&& port_directory : *port_directories)
            {
                auto filename = port_directory.filename();
                if (filename == ".DS_Store") continue;
                out.emplace_back(filename.data(), filename.size());
            }

            return Unit{};
        }

        return std::move(maybe_port_directories).error();
    }

    ExpectedL<bool> BuiltinFilesRegistry::try_append_all_port_names_no_network(
        std::vector<std::string>& port_names) const
    {
        return append_all_port_names(port_names).map([](Unit) { return true; });
    }
    // } BuiltinFilesRegistry::RegistryImplementation

    // { BuiltinGitRegistry::RegistryImplementation
    LocalizedString format_version_git_entry_missing(StringView port_name,
                                                     const Version& expected_version,
                                                     const std::vector<Version>& versions)
    {
        auto error_msg =
            msg::format_error(msgVersionGitEntryMissing, msg::package_name = port_name, msg::version = expected_version)
                .append_raw('\n');
        for (auto&& version : versions)
        {
            error_msg.append_indent().append_raw(version.to_string()).append_raw('\n');
        }

        error_msg.append(msgVersionIncomparable4, msg::url = docs::versioning_url);
        return error_msg;
    }

    ExpectedL<Optional<PathAndLocation>> BuiltinGitRegistry::get_port(const VersionSpec& spec) const
    {
        const auto& maybe_maybe_versions = get_versions(spec.port_name);
        auto maybe_versions = maybe_maybe_versions.get();
        if (!maybe_versions)
        {
            return maybe_maybe_versions.error();
        }

        auto versions = maybe_versions->get();
        if (!versions)
        {
            return m_files_impl->get_port(spec);
        }

        auto git_tree = versions->try_get_git_tree(spec.version);
        if (!git_tree)
        {
            return format_version_git_entry_missing(spec.port_name, spec.version, versions->port_versions())
                .append_raw('\n')
                .append(msg::msgNoteMessage)
                .append(msgChecksUpdateVcpkg);
        }

        return m_paths.git_checkout_port(spec.port_name, *git_tree, m_paths.root / ".git")
            .map([git_tree](Path&& p) -> Optional<PathAndLocation> {
                return PathAndLocation{
                    std::move(p),
                    "git+https://github.com/Microsoft/vcpkg@" + *git_tree,
                };
            });
    }

    ExpectedL<Optional<View<Version>>> BuiltinGitRegistry::get_all_port_versions(StringView port_name) const
    {
        const auto& maybe_maybe_versions = get_versions(port_name);
        auto maybe_versions = maybe_maybe_versions.get();
        if (!maybe_versions)
        {
            return ExpectedL<Optional<View<Version>>>{maybe_maybe_versions.error(), expected_right_tag};
        }

        auto versions = maybe_versions->get();
        if (!versions)
        {
            return m_files_impl->get_all_port_versions(port_name);
        }

        return Optional<View<Version>>{versions->port_versions()};
    }

    ExpectedL<Optional<Version>> BuiltinGitRegistry::get_baseline_version(StringView port_name) const
    {
        const auto& maybe_baseline = m_baseline.get_lazy([this]() -> ExpectedL<Baseline> {
            auto maybe_path = git_checkout_baseline(m_paths, m_baseline_identifier);
            auto path = maybe_path.get();
            if (!path)
            {
                return std::move(maybe_path)
                    .error()
                    .append_raw('\n')
                    .append_raw(m_paths.get_current_git_sha_baseline_message());
            }

            auto maybe_maybe_baseline = load_baseline_versions(m_paths.get_filesystem(), *path);
            auto maybe_baseline = maybe_maybe_baseline.get();
            if (!maybe_baseline)
            {
                return std::move(maybe_maybe_baseline).error();
            }

            auto baseline = maybe_baseline->get();
            if (!baseline)
            {
                return msg::format_error(
                    msgCouldNotFindBaseline, msg::commit_sha = m_baseline_identifier, msg::path = *path);
            }

            return std::move(*baseline);
        });

        auto baseline = maybe_baseline.get();
        if (!baseline)
        {
            return maybe_baseline.error();
        }

        auto it = baseline->find(port_name);
        if (it != baseline->end())
        {
            return it->second;
        }

        return Optional<Version>();
    }

    ExpectedL<Unit> BuiltinGitRegistry::append_all_port_names(std::vector<std::string>& out) const
    {
        const auto& fs = m_paths.get_filesystem();

        if (fs.exists(m_paths.builtin_registry_versions, IgnoreErrors{}))
        {
            load_all_port_names_from_registry_versions(out, fs, m_paths.builtin_registry_versions);
        }

        return m_files_impl->append_all_port_names(out);
    }

    ExpectedL<bool> BuiltinGitRegistry::try_append_all_port_names_no_network(std::vector<std::string>& port_names) const
    {
        return append_all_port_names(port_names).map([](Unit) { return true; });
    }
    // } BuiltinGitRegistry::RegistryImplementation

    // { FilesystemRegistry::RegistryImplementation
    ExpectedL<Optional<Version>> FilesystemRegistry::get_baseline_version(StringView port_name) const
    {
        const auto& maybe_maybe_baseline = m_baseline.get_lazy([this]() -> ExpectedL<Optional<Baseline>> {
            auto path_to_baseline = m_path / registry_versions_dir_name / "baseline.json";
            return load_baseline_versions(m_fs, path_to_baseline, m_baseline_identifier);
        });

        auto maybe_baseline = maybe_maybe_baseline.get();
        if (!maybe_baseline)
        {
            return maybe_maybe_baseline.error();
        }

        auto baseline = maybe_baseline->get();
        if (!baseline)
        {
            return Optional<Version>();
        }

        auto it = baseline->find(port_name);
        if (it != baseline->end())
        {
            return it->second;
        }

        return Optional<Version>();
    }

    ExpectedL<Optional<PathAndLocation>> FilesystemRegistry::get_port(const VersionSpec& spec) const
    {
        const auto& maybe_maybe_entry = get_entry(spec.port_name);
        const auto maybe_entry = maybe_maybe_entry.get();
        if (!maybe_entry)
        {
            return maybe_maybe_entry.error();
        }

        const auto entry = maybe_entry->get();
        if (!entry)
        {
            return Optional<PathAndLocation>();
        }

        auto&& port_versions = entry->port_versions;
        auto it = std::find(port_versions.begin(), port_versions.end(), spec.version);
        if (it == port_versions.end())
        {
            return Optional<PathAndLocation>();
        }

        return PathAndLocation{
            entry->version_paths[it - port_versions.begin()],
            "",
        };
    }

    ExpectedL<Optional<View<Version>>> FilesystemRegistry::get_all_port_versions(StringView port_name) const
    {
        const auto& maybe_maybe_entry = get_entry(port_name);
        const auto maybe_entry = maybe_maybe_entry.get();
        if (!maybe_entry)
        {
            return ExpectedL<Optional<View<Version>>>{maybe_maybe_entry.error(), expected_right_tag};
        }

        const auto entry = maybe_entry->get();
        if (!entry)
        {
            return Optional<View<Version>>();
        }

        return View<Version>{entry->port_versions};
    }

    ExpectedL<Unit> FilesystemRegistry::append_all_port_names(std::vector<std::string>& out) const
    {
        return load_all_port_names_from_registry_versions(out, m_fs, m_path / registry_versions_dir_name);
    }

    ExpectedL<bool> FilesystemRegistry::try_append_all_port_names_no_network(std::vector<std::string>& port_names) const
    {
        return append_all_port_names(port_names).map([](Unit) { return true; });
    }
    // } FilesystemRegistry::RegistryImplementation

    // { GitRegistry::RegistryImplementation
    ExpectedL<Optional<PathAndLocation>> GitRegistry::get_port(const VersionSpec& spec) const
    {
        const auto& maybe_entry = get_lock_entry();
        auto lock_entry = maybe_entry.get();
        if (!lock_entry)
        {
            return maybe_entry.error();
        }

        if (lock_entry->stale())
        {
            const auto& maybe_maybe_stale_versions = get_stale_versions(lock_entry, spec.port_name);
            if (const auto maybe_stale_versions = maybe_maybe_stale_versions.get())
            {
                if (const auto stale_versions = maybe_stale_versions->get())
                {
                    if (auto git_tree = stale_versions->try_get_git_tree(spec.version))
                    {
                        return load_git_tree(*git_tree);
                    }
                }
            }
        }

        const auto& maybe_maybe_live_versions = get_live_versions(spec.port_name);
        auto maybe_live_versions = maybe_maybe_live_versions.get();
        if (!maybe_live_versions)
        {
            return maybe_maybe_live_versions.error();
        }

        if (auto live_versions = maybe_live_versions->get())
        {
            if (auto git_tree = live_versions->try_get_git_tree(spec.version))
            {
                return load_git_tree(*git_tree);
            }
        }

        return Optional<PathAndLocation>();
    }

    ExpectedL<Optional<View<Version>>> GitRegistry::get_all_port_versions(StringView port_name) const
    {
        const auto& maybe_maybe_live_versions = get_live_versions(port_name);
        auto maybe_live_versions = maybe_maybe_live_versions.get();
        if (!maybe_live_versions)
        {
            return ExpectedL<Optional<View<Version>>>{maybe_maybe_live_versions.error(), expected_right_tag};
        }

        if (auto live_versions = maybe_live_versions->get())
        {
            return View<Version>(live_versions->port_versions());
        }

        return Optional<View<Version>>();
    }

    ExpectedL<Optional<Version>> GitRegistry::get_baseline_version(StringView port_name) const
    {
        const auto& maybe_baseline = m_baseline.get_lazy([this, port_name]() -> ExpectedL<Baseline> {
            // We delay baseline validation until here to give better error messages and suggestions
            if (!is_git_commit_sha(m_baseline_identifier))
            {
                auto& maybe_lock_entry = get_lock_entry();
                auto lock_entry = maybe_lock_entry.get();
                if (!lock_entry)
                {
                    return maybe_lock_entry.error();
                }

                auto maybe_up_to_date = lock_entry->ensure_up_to_date(m_paths);
                if (maybe_up_to_date)
                {
                    return msg::format_error(
                        msgGitRegistryMustHaveBaseline, msg::url = m_repo, msg::commit_sha = lock_entry->commit_id());
                }

                return std::move(maybe_up_to_date).error();
            }

            auto path_to_baseline = Path(registry_versions_dir_name.to_string()) / "baseline.json";
            auto maybe_contents = m_paths.git_show_from_remote_registry(m_baseline_identifier, path_to_baseline);
            if (!maybe_contents)
            {
                auto& maybe_lock_entry = get_lock_entry();
                auto lock_entry = maybe_lock_entry.get();
                if (!lock_entry)
                {
                    return maybe_lock_entry.error();
                }

                auto maybe_up_to_date = lock_entry->ensure_up_to_date(m_paths);
                if (!maybe_up_to_date)
                {
                    return std::move(maybe_up_to_date).error();
                }

                maybe_contents = m_paths.git_show_from_remote_registry(m_baseline_identifier, path_to_baseline);
            }

            if (!maybe_contents)
            {
                msg::println(msgFetchingBaselineInfo, msg::package_name = m_repo);
                auto maybe_err = m_paths.git_fetch(m_repo, m_baseline_identifier);
                if (!maybe_err)
                {
                    get_global_metrics_collector().track_define(DefineMetric::RegistriesErrorCouldNotFindBaseline);
                    return msg::format_error(msgFailedToFetchRepo, msg::url = m_repo)
                        .append_raw('\n')
                        .append(maybe_err.error());
                }

                maybe_contents = m_paths.git_show_from_remote_registry(m_baseline_identifier, path_to_baseline);
            }

            if (!maybe_contents)
            {
                get_global_metrics_collector().track_define(DefineMetric::RegistriesErrorCouldNotFindBaseline);
                return msg::format_error(msgCouldNotFindBaselineInCommit,
                                         msg::url = m_repo,
                                         msg::commit_sha = m_baseline_identifier,
                                         msg::package_name = port_name)
                    .append_raw('\n')
                    .append_raw(maybe_contents.error());
            }

            auto contents = maybe_contents.get();
            auto res_baseline = parse_baseline_versions(*contents, "default", path_to_baseline);
            if (auto opt_baseline = res_baseline.get())
            {
                if (auto p = opt_baseline->get())
                {
                    return std::move(*p);
                }

                get_global_metrics_collector().track_define(DefineMetric::RegistriesErrorCouldNotFindBaseline);
                return msg::format_error(
                    msgBaselineMissingDefault, msg::commit_sha = m_baseline_identifier, msg::url = m_repo);
            }

            return msg::format_error(
                       msgErrorWhileFetchingBaseline, msg::value = m_baseline_identifier, msg::package_name = m_repo)
                .append_raw('\n')
                .append(res_baseline.error());
        });

        auto baseline = maybe_baseline.get();
        if (!baseline)
        {
            return maybe_baseline.error();
        }

        auto it = baseline->find(port_name);
        if (it != baseline->end())
        {
            return it->second;
        }

        return Optional<Version>();
    }

    ExpectedL<Unit> GitRegistry::append_all_port_names(std::vector<std::string>& out) const
    {
        auto maybe_versions_path = get_live_versions_tree_path();
        if (auto versions_path = maybe_versions_path.get())
        {
            return load_all_port_names_from_registry_versions(out, m_paths.get_filesystem(), *versions_path);
        }

        return std::move(maybe_versions_path).error();
    }

    ExpectedL<bool> GitRegistry::try_append_all_port_names_no_network(std::vector<std::string>&) const
    {
        // At this time we don't record enough information to know what the last fetch for a registry is,
        // so we can't even return what the most recent answer was.
        //
        // This would be fixable if we recorded LockFile in the registries cache.
        return false;
    }
    // } GitRegistry::RegistryImplementation

    // } RegistryImplementation
}

// deserializers
namespace
{
    using namespace vcpkg;

    struct BaselineDeserializer final : Json::IDeserializer<std::map<std::string, Version, std::less<>>>
    {
        LocalizedString type_name() const override { return msg::format(msgABaselineObject); }

        Optional<type> visit_object(Json::Reader& r, const Json::Object& obj) const override
        {
            std::map<std::string, Version, std::less<>> result;

            for (auto pr : obj)
            {
                const auto& version_value = pr.second;
                Version version;
                r.visit_in_key(version_value, pr.first, version, get_versiontag_deserializer_instance());

                result.emplace(pr.first.to_string(), std::move(version));
            }

            return result;
        }

        static const BaselineDeserializer instance;
    };

    const BaselineDeserializer BaselineDeserializer::instance;

    Path relative_path_to_versions(StringView port_name)
    {
        char prefix[] = {port_name[0], '-', '\0'};
        return Path(prefix) / port_name.to_string() + ".json";
    }

    ExpectedL<Optional<std::vector<VersionDbEntry>>> load_versions_file(const ReadOnlyFilesystem& fs,
                                                                        VersionDbType type,
                                                                        const Path& registry_versions,
                                                                        StringView port_name,
                                                                        const Path& registry_root)
    {
        if (type == VersionDbType::Filesystem && registry_root.empty())
        {
            Checks::unreachable(VCPKG_LINE_INFO, "type should never = Filesystem when registry_root is empty.");
        }

        auto versions_file_path = registry_versions / relative_path_to_versions(port_name);
        std::error_code ec;
        auto contents = fs.read_contents(versions_file_path, ec);
        if (ec)
        {
            if (ec == std::errc::no_such_file_or_directory)
            {
                return Optional<std::vector<VersionDbEntry>>{};
            }

            return format_filesystem_call_error(ec, "read_contents", {versions_file_path});
        }

        auto maybe_versions_json = Json::parse(contents);
        auto versions_json = maybe_versions_json.get();
        if (!versions_json)
        {
            return LocalizedString::from_raw(maybe_versions_json.error()->to_string());
        }

        if (!versions_json->value.is_object())
        {
            return msg::format_error(msgFailedToParseNoTopLevelObj, msg::path = versions_file_path);
        }

        const auto& versions_object = versions_json->value.object(VCPKG_LINE_INFO);
        auto maybe_versions_array = versions_object.get("versions");
        if (!maybe_versions_array || !maybe_versions_array->is_array())
        {
            return msg::format_error(msgFailedToParseNoVersionsArray, msg::path = versions_file_path);
        }

        std::vector<VersionDbEntry> db_entries;
        VersionDbEntryArrayDeserializer deserializer{type, registry_root};
        // Avoid warning treated as error.
        if (maybe_versions_array != nullptr)
        {
            Json::Reader r;
            r.visit_in_key(*maybe_versions_array, "versions", db_entries, deserializer);
            if (!r.errors().empty())
            {
                return msg::format_error(msgFailedToParseVersionsFile, msg::path = versions_file_path)
                    .append_raw(Strings::join("\n", r.errors()));
            }
        }

        return db_entries;
    }

    ExpectedL<Optional<Baseline>> parse_baseline_versions(StringView contents, StringView baseline, StringView origin)
    {
        auto maybe_value = Json::parse(contents, origin);
        if (!maybe_value)
        {
            return LocalizedString::from_raw(maybe_value.error()->to_string());
        }

        auto& value = *maybe_value.get();
        if (!value.value.is_object())
        {
            return msg::format_error(msgFailedToParseNoTopLevelObj, msg::path = origin);
        }

        auto real_baseline = baseline.size() == 0 ? "default" : baseline;

        const auto& obj = value.value.object(VCPKG_LINE_INFO);
        auto baseline_value = obj.get(real_baseline);
        if (!baseline_value)
        {
            return {nullopt, expected_left_tag};
        }

        Json::Reader r;
        std::map<std::string, Version, std::less<>> result;
        r.visit_in_key(*baseline_value, real_baseline, result, BaselineDeserializer::instance);
        if (r.errors().empty())
        {
            return {std::move(result), expected_left_tag};
        }
        else
        {
            return msg::format_error(msgFailedToParseBaseline, msg::path = origin)
                .append_raw('\n')
                .append_raw(Strings::join("\n", r.errors()));
        }
    }

    ExpectedL<Optional<Baseline>> load_baseline_versions(const ReadOnlyFilesystem& fs,
                                                         const Path& baseline_path,
                                                         StringView baseline)
    {
        std::error_code ec;
        auto contents = fs.read_contents(baseline_path, ec);
        if (ec)
        {
            if (ec == std::errc::no_such_file_or_directory)
            {
                msg::println(msgFailedToFindBaseline);
                return {nullopt, expected_left_tag};
            }

            return format_filesystem_call_error(ec, "read_contents", {baseline_path});
        }

        return parse_baseline_versions(contents, baseline, baseline_path);
    }
}

namespace vcpkg
{
    ExpectedL<LockFile::Entry> LockFile::get_or_fetch(const VcpkgPaths& paths, StringView repo, StringView reference)
    {
        auto range = lockdata.equal_range(repo);
        auto it = std::find_if(range.first, range.second, [&reference](const LockDataType::value_type& repo2entry) {
            return repo2entry.second.reference == reference;
        });

        if (it == range.second)
        {
            msg::println(msgFetchingRegistryInfo, msg::url = repo, msg::value = reference);
            auto maybe_commit = paths.git_fetch_from_remote_registry(repo, reference);
            if (auto commit = maybe_commit.get())
            {
                it = lockdata.emplace(repo.to_string(), EntryData{reference.to_string(), *commit, false});
                modified = true;
            }
            else
            {
                return std::move(maybe_commit).error();
            }
        }

        return LockFile::Entry{this, it};
    }
    ExpectedL<Unit> LockFile::Entry::ensure_up_to_date(const VcpkgPaths& paths) const
    {
        if (data->second.stale)
        {
            StringView repo(data->first);
            StringView reference(data->second.reference);
            msg::println(msgFetchingRegistryInfo, msg::url = repo, msg::value = reference);

            auto maybe_commit_id = paths.git_fetch_from_remote_registry(repo, reference);
            if (const auto commit_id = maybe_commit_id.get())
            {
                data->second.commit_id = *commit_id;
                data->second.stale = false;
                lockfile->modified = true;
            }
            else
            {
                return std::move(maybe_commit_id).error();
            }
        }

        return Unit{};
    }

    Registry::Registry(std::vector<std::string>&& patterns, std::unique_ptr<RegistryImplementation>&& impl)
        : patterns_(std::move(patterns)), implementation_(std::move(impl))
    {
        Util::sort_unique_erase(patterns_);
        Checks::check_exit(VCPKG_LINE_INFO, implementation_ != nullptr);
    }

    const RegistryImplementation* RegistrySet::registry_for_port(StringView name) const
    {
        auto candidates = registries_for_port(name);
        if (candidates.empty())
        {
            return default_registry();
        }

        return candidates[0];
    }

    size_t package_pattern_match(StringView name, StringView pattern)
    {
        const auto pattern_size = pattern.size();
        const auto maybe_star_index = pattern_size - 1;
        if (pattern_size != 0 && pattern[maybe_star_index] == '*')
        {
            // pattern ends in wildcard
            if (name.size() >= maybe_star_index && std::equal(pattern.begin(), pattern.end() - 1, name.begin()))
            {
                return pattern_size;
            }
        }
        else if (name == pattern)
        {
            // exact match is like matching "infinity" prefix
            return SIZE_MAX;
        }

        return 0;
    }

    std::vector<const RegistryImplementation*> RegistrySet::registries_for_port(StringView name) const
    {
        struct RegistryCandidate
        {
            const RegistryImplementation* impl;
            std::size_t matched_prefix;
        };

        std::vector<RegistryCandidate> candidates;
        for (auto&& registry : registries())
        {
            std::size_t longest_prefix = 0;
            for (auto&& pattern : registry.patterns())
            {
                longest_prefix = std::max(longest_prefix, package_pattern_match(name, pattern));
            }

            if (longest_prefix != 0)
            {
                candidates.push_back({&registry.implementation(), longest_prefix});
            }
        }

        if (candidates.empty())
        {
            return std::vector<const RegistryImplementation*>();
        }

        std::stable_sort(
            candidates.begin(), candidates.end(), [](const RegistryCandidate& lhs, const RegistryCandidate& rhs) {
                return lhs.matched_prefix > rhs.matched_prefix;
            });

        return Util::fmap(std::move(candidates), [](const RegistryCandidate& target) { return target.impl; });
    }

    ExpectedL<Optional<Version>> RegistrySet::baseline_for_port(StringView port_name) const
    {
        auto impl = registry_for_port(port_name);
        if (!impl) return msg::format(msg::msgErrorMessage).append(msgNoRegistryForPort, msg::package_name = port_name);
        return impl->get_baseline_version(port_name);
    }

    bool RegistrySet::is_default_builtin_registry() const
    {
        return default_registry_ && default_registry_->kind() == BuiltinFilesRegistry::s_kind;
    }
    bool RegistrySet::has_modifications() const { return !registries_.empty() || !is_default_builtin_registry(); }
} // namespace vcpkg

namespace
{
    void remove_unreachable_port_names_by_patterns(std::vector<std::string>& result,
                                                   std::size_t start_at,
                                                   View<std::string> patterns)
    {
        // Remove names in result[start_at .. end] which no package pattern matches
        result.erase(std::remove_if(result.begin() + start_at,
                                    result.end(),
                                    [&](const std::string& name) {
                                        return std::none_of(
                                            patterns.begin(), patterns.end(), [&](const std::string& pattern) {
                                                return package_pattern_match(name, pattern) != 0;
                                            });
                                    }),
                     result.end());
    }
} // unnamed namespace

namespace vcpkg
{
    ExpectedL<std::vector<std::string>> RegistrySet::get_all_reachable_port_names() const
    {
        std::vector<std::string> result;
        for (const auto& registry : registries())
        {
            const auto start_at = result.size();
            auto this_append = registry.implementation().append_all_port_names(result);
            if (!this_append)
            {
                return std::move(this_append).error();
            }

            remove_unreachable_port_names_by_patterns(result, start_at, registry.patterns());
        }

        if (auto registry = default_registry())
        {
            auto this_append = registry->append_all_port_names(result);
            if (!this_append)
            {
                return std::move(this_append).error();
            }
        }

        Util::sort_unique_erase(result);
        return result;
    }

    ExpectedL<std::vector<std::string>> RegistrySet::get_all_known_reachable_port_names_no_network() const
    {
        std::vector<std::string> result;
        for (const auto& registry : registries())
        {
            const auto start_at = result.size();
            const auto patterns = registry.patterns();
            auto maybe_append = registry.implementation().try_append_all_port_names_no_network(result);
            auto append = maybe_append.get();
            if (!append)
            {
                return std::move(maybe_append).error();
            }

            if (*append)
            {
                remove_unreachable_port_names_by_patterns(result, start_at, patterns);
            }
            else
            {
                // we don't know all names, but we can at least assume the exact match patterns
                // will be names
                std::remove_copy_if(patterns.begin(),
                                    patterns.end(),
                                    std::back_inserter(result),
                                    [&](const std::string& package_pattern) -> bool {
                                        return package_pattern.empty() || package_pattern.back() == '*';
                                    });
            }
        }

        if (auto registry = default_registry())
        {
            auto maybe_append = registry->try_append_all_port_names_no_network(result);
            if (!maybe_append)
            {
                return std::move(maybe_append).error();
            }
        }

        Util::sort_unique_erase(result);
        return result;
    }

    ExpectedL<Optional<PathAndLocation>> RegistrySet::get_port(const VersionSpec& spec) const
    {
        auto impl = registry_for_port(spec.port_name);
        if (!impl)
        {
            return Optional<PathAndLocation>();
        }

        return impl->get_port(spec);
    }

    ExpectedL<PathAndLocation> RegistrySet::get_port_required(const VersionSpec& spec) const
    {
        auto maybe_maybe_port = get_port(spec);
        auto maybe_port = maybe_maybe_port.get();
        if (!maybe_port)
        {
            return std::move(maybe_maybe_port).error();
        }

        auto port = maybe_port->get();
        if (!port)
        {
            return msg::format_error(
                msgVersionDatabaseEntryMissing, msg::package_name = spec.port_name, msg::version = spec.version);
        }

        return std::move(*port);
    }

    ExpectedL<Optional<View<Version>>> RegistrySet::get_all_port_versions(StringView port_name) const
    {
        auto impl = registry_for_port(port_name);
        if (!impl)
        {
            return Optional<View<Version>>();
        }

        return impl->get_all_port_versions(port_name);
    }

    // Identical to get_all_port_versions, but nonexistent ports are translated to an error.
    ExpectedL<View<Version>> RegistrySet::get_all_port_versions_required(StringView port_name) const
    {
        auto maybe_maybe_versions = get_all_port_versions(port_name);
        auto maybe_versions = maybe_maybe_versions.get();
        if (!maybe_versions)
        {
            return std::move(maybe_maybe_versions).error();
        }

        auto versions = maybe_versions->get();
        if (!versions)
        {
            return msg::format_error(msgVersionDatabaseEntriesMissing, msg::package_name = port_name);
        }

        return std::move(*versions);
    }

    ExpectedL<Optional<std::vector<std::pair<SchemedVersion, std::string>>>> get_builtin_versions(
        const VcpkgPaths& paths, StringView port_name)
    {
        auto maybe_maybe_versions =
            load_versions_file(paths.get_filesystem(), VersionDbType::Git, paths.builtin_registry_versions, port_name);
        auto maybe_versions = maybe_maybe_versions.get();
        if (!maybe_versions)
        {
            return std::move(maybe_maybe_versions).error();
        }

        auto versions = maybe_versions->get();
        if (!versions)
        {
            return Optional<std::vector<std::pair<SchemedVersion, std::string>>>{};
        }

        return Optional<std::vector<std::pair<SchemedVersion, std::string>>>{
            Util::fmap(*versions, [](const VersionDbEntry& entry) -> std::pair<SchemedVersion, std::string> {
                return {SchemedVersion{entry.scheme, entry.version}, entry.git_tree};
            })};
    }

    ExpectedL<Baseline> get_builtin_baseline(const VcpkgPaths& paths)
    {
        auto baseline_path = paths.builtin_registry_versions / "baseline.json";
        return load_baseline_versions(paths.get_filesystem(), baseline_path)
            .then([&](Optional<Baseline>&& b) -> ExpectedL<Baseline> {
                if (auto p = b.get())
                {
                    return std::move(*p);
                }

                return msg::format_error(msgBaselineFileNoDefaultFieldPath, msg::path = baseline_path);
            });
    }

    bool is_git_commit_sha(StringView sv)
    {
        static constexpr struct
        {
            bool operator()(char ch) const { return ('0' <= ch && ch <= '9') || ('a' <= ch && ch <= 'f'); }
        } is_lcase_ascii_hex;

        return sv.size() == 40 && std::all_of(sv.begin(), sv.end(), is_lcase_ascii_hex);
    }

    std::unique_ptr<RegistryImplementation> make_builtin_registry(const VcpkgPaths& paths)
    {
        if (paths.use_git_default_registry())
        {
            return std::make_unique<BuiltinErrorRegistry>();
        }
        else
        {
            return std::make_unique<BuiltinFilesRegistry>(paths);
        }
    }
    std::unique_ptr<RegistryImplementation> make_builtin_registry(const VcpkgPaths& paths, std::string baseline)
    {
        if (paths.use_git_default_registry())
        {
            return std::make_unique<GitRegistry>(
                paths, builtin_registry_git_url.to_string(), "HEAD", std::move(baseline));
        }
        else
        {
            return std::make_unique<BuiltinGitRegistry>(paths, std::move(baseline));
        }
    }
    std::unique_ptr<RegistryImplementation> make_git_registry(const VcpkgPaths& paths,
                                                              std::string repo,
                                                              std::string reference,
                                                              std::string baseline)
    {
        return std::make_unique<GitRegistry>(paths, std::move(repo), std::move(reference), std::move(baseline));
    }
    std::unique_ptr<RegistryImplementation> make_filesystem_registry(const ReadOnlyFilesystem& fs,
                                                                     Path path,
                                                                     std::string baseline)
    {
        return std::make_unique<FilesystemRegistry>(fs, std::move(path), std::move(baseline));
    }

    std::unique_ptr<Json::IDeserializer<std::vector<VersionDbEntry>>> make_version_db_deserializer(VersionDbType type,
                                                                                                   const Path& root)
    {
        return std::make_unique<VersionDbEntryArrayDeserializer>(type, root);
    }
}
