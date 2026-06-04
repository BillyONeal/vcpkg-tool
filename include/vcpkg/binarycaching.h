#pragma once

#include <vcpkg/base/fwd/message_sinks.h>

#include <vcpkg/fwd/binarycaching.h>
#include <vcpkg/fwd/build.h>
#include <vcpkg/fwd/dependencies.h>
#include <vcpkg/fwd/tools.h>
#include <vcpkg/fwd/vcpkgpaths.h>

#include <vcpkg/base/background-work-queue.h>
#include <vcpkg/base/downloads.h>
#include <vcpkg/base/expected.h>
#include <vcpkg/base/message_sinks.h>
#include <vcpkg/base/path.h>

#include <vcpkg/archives.h>
#include <vcpkg/packagespec.h>
#include <vcpkg/versions.h>

#include <chrono>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vcpkg
{
    struct CacheStatus
    {
        bool should_attempt_precheck(const IReadBinaryProvider* sender) const noexcept;
        bool should_attempt_restore(const IReadBinaryProvider* sender) const noexcept;

        bool is_unavailable(const IReadBinaryProvider* sender) const noexcept;
        const IReadBinaryProvider* get_available_provider() const noexcept;
        bool is_restored() const noexcept;

        void mark_unavailable(const IReadBinaryProvider* sender);
        void mark_available(const IReadBinaryProvider* sender) noexcept;
        void mark_restored() noexcept;
        void mark_unrestored() noexcept;

    private:
        CacheStatusState m_status = CacheStatusState::unknown;

        // The set of providers who know they do not have the associated cache entry.
        // Flat vector set because N is tiny.
        std::vector<const IReadBinaryProvider*> m_known_unavailable_providers;

        // The provider who affirmatively has the associated cache entry.
        const IReadBinaryProvider* m_available_provider = nullptr; // meaningful iff m_status == available
    };

    struct BinaryPackageReadInfo
    {
        explicit BinaryPackageReadInfo(const InstallPlanAction& action);
        std::string package_abi;
        PackageSpec spec;
        std::string display_name;
        Version version;
        Path package_dir;
    };

    struct BinaryPackageWriteInfo : BinaryPackageReadInfo
    {
        using BinaryPackageReadInfo::BinaryPackageReadInfo;

        // Filled if BinaryCache has a provider that returns true for needs_nuspec_data()
        Optional<std::string> nuspec;
        // Set to true if there is only one write provider, meaning that one provider can take ownership of the zip file
        bool unique_write_provider = false;
        // Filled if BinaryCache has a provider that returns true for needs_zip_file()
        // Note: this can be empty if an error occurred while compressing.
        Optional<Path> zip_path;
    };

    struct IWriteBinaryProvider
    {
        virtual ~IWriteBinaryProvider() = default;

        /// Called upon a successful build of `action` to store those contents in the binary cache.
        /// returns true if the upload succeeded
        ///
        /// Note that as this is considered non-fatal, only warnings or lower will be emitted to `context`.
        virtual bool push_success(DiagnosticContext& context,
                                  const Filesystem& fs,
                                  const Path& packages,
                                  const BinaryPackageWriteInfo& request) = 0;

        virtual bool needs_nuspec_data() const = 0;
        virtual bool needs_zip_file() const = 0;
    };

    struct IReadBinaryProvider
    {
        virtual ~IReadBinaryProvider() = default;

        /// Gives the IBinaryProvider an opportunity to batch any downloading or server communication for executing
        /// `actions`. Note that as this API can't fail, only warnings or lower will be emitted to `context`.
        ///
        /// IBinaryProvider should set out_status[i] to RestoreResult::restored for each fetched package.
        ///
        /// Prerequisites: actions[i].package_abi(), out_status.size() == actions.size()
        virtual void fetch(DiagnosticContext& context,
                           const Filesystem& fs,
                           const ZipTool* zip_tool,
                           const Path& packages,
                           View<const InstallPlanAction*> actions,
                           Span<RestoreResult> out_status) const = 0;

        /// Checks whether the `actions` are present in the cache, without restoring them.
        /// Note that as this API can't fail, only warnings or lower will be emitted to `context`.
        ///
        /// Used by CI to determine missing packages. For each `i`, out_status[i] should be set to
        /// CacheAvailability::available or CacheAvailability::unavailable
        ///
        /// Prerequisites: actions[i].package_abi(), out_status.size() == actions.size()
        virtual void precheck(DiagnosticContext& context,
                              const Filesystem& fs,
                              View<const InstallPlanAction*> actions,
                              Span<CacheAvailability> out_status) const = 0;

        virtual LocalizedString restored_message(size_t count,
                                                 std::chrono::high_resolution_clock::duration elapsed) const = 0;
    };

    struct UrlTemplate
    {
        std::string url_template;
        std::vector<std::string> headers;
        bool has_sha = false;
        bool has_other = false;

        std::string instantiate_variables(const BinaryPackageReadInfo& info) const;
    };

    struct NuGetRepoInfo
    {
        std::string repo;
        std::string branch;
        std::string commit;
    };

    struct AzureUpkgSource
    {
        std::string organization;
        std::string project;
        std::string feed;
    };

    struct AzCopyUrl
    {
        std::string url;
        std::string sas;

        std::string make_object_path(const std::string& abi) const;
        std::string make_container_path() const;
    };

    // Turns:
    // - <XXXX>-<YY>-<ZZ><whatever> -> <X>.<Y>.<Z>-vcpkg<abitag>
    // - v?<X> -> <X>.0.0-vcpkg<abitag>
    //   - this avoids turning 20-01-01 into 20.0.0-vcpkg<abitag>
    // - v?<X>.<Y><whatever> -> <X>.<Y>.0-vcpkg<abitag>
    // - v?<X>.<Y>.<Z><whatever> -> <X>.<Y>.<Z>-vcpkg<abitag>
    // - anything else -> 0.0.0-vcpkg<abitag>
    std::string format_version_for_feedref(StringView version_text, StringView abi_tag);

    struct FeedReference
    {
        FeedReference(std::string id, std::string version);

        std::string id;
        std::string version;

        std::string nupkg_filename() const;
    };

    FeedReference make_nugetref(const InstallPlanAction& action, StringView prefix);

    std::string generate_nuspec(const Path& package_dir,
                                const InstallPlanAction& action,
                                StringView id_prefix,
                                const NuGetRepoInfo& repo_info);

    StringLiteral to_string_literal(BinaryCacheProviderKind kind);
    StringLiteral to_string_literal(CacheAccessControl access);

    struct BinaryCacheProviderEntry
    {
        BinaryCacheProviderKind kind;
        CacheAccessControl access;
        Optional<std::string> arg1;
        Optional<std::string> arg2;
        Optional<std::string> arg3;

        friend bool operator==(const BinaryCacheProviderEntry& lhs, const BinaryCacheProviderEntry& rhs);
        friend bool operator!=(const BinaryCacheProviderEntry& lhs, const BinaryCacheProviderEntry& rhs);
        void to_string(std::string& out) const;
        std::string to_string() const;
    };

    struct BinaryCacheParsedConfigs
    {
        std::vector<BinaryCacheProviderEntry> providers;
        std::set<StringLiteral> telemetry_tags;
        bool nuget_interactive = false;
        bool aws_no_sign_request = false;
        long nuget_timeout = 100;
    };

    Optional<BinaryCacheParsedConfigs> parse_binary_provider_configs(DiagnosticContext&,
                                                                     const Path& default_cache_path,
                                                                     const std::string& env_string,
                                                                     View<std::string> args);

    struct BinaryProviders
    {
        std::vector<std::unique_ptr<IReadBinaryProvider>> read;
        std::vector<std::unique_ptr<IWriteBinaryProvider>> write;
        std::string nuget_prefix;
        NuGetRepoInfo nuget_repo;
    };

    struct ReadOnlyBinaryCache
    {
        ReadOnlyBinaryCache(const Filesystem& fs, Path packages);
        ReadOnlyBinaryCache(const ReadOnlyBinaryCache&) = delete;
        ReadOnlyBinaryCache& operator=(const ReadOnlyBinaryCache&) = delete;

        /// Gives the IBinaryProvider an opportunity to batch any downloading or server communication for
        /// executing `actions`.
        void fetch(DiagnosticContext& context, View<InstallPlanAction> actions);

        bool is_restored(const InstallPlanAction& ipa) const;

        void install_read_provider(std::unique_ptr<IReadBinaryProvider>&& provider);

        /// Checks whether the `actions` are present in the cache, without restoring them. Used by CI to determine
        /// missing packages.
        /// Returns a vector where each index corresponds to the matching index in `actions`.
        std::vector<CacheAvailability> precheck(DiagnosticContext& context, View<const InstallPlanAction*> actions);

        // Informs the binary cache that the packages directory has been reset. Used when the same port-name is built
        // more than once in a single invocation of vcpkg.
        void mark_all_unrestored();

    protected:
        const Filesystem& m_fs;
        Path m_packages;
        ZipTool m_zip_tool;
        BinaryProviders m_config;

        std::unordered_map<std::string, CacheStatus> m_status;
    };

    struct BinaryCacheSynchronizer
    {
        using backing_uint_t = std::conditional_t<sizeof(size_t) == 4, uint32_t, uint64_t>;
        using counter_uint_t = std::conditional_t<sizeof(size_t) == 4, uint16_t, uint32_t>;
        static constexpr backing_uint_t SubmissionCompleteBit = static_cast<backing_uint_t>(1)
                                                                << (sizeof(backing_uint_t) * 8 - 1);
        static constexpr backing_uint_t UpperShift = sizeof(counter_uint_t) * 8;
        static constexpr backing_uint_t SubmittedMask =
            static_cast<backing_uint_t>(static_cast<counter_uint_t>(-1) >> 1u);
        static constexpr backing_uint_t CompletedMask = SubmittedMask << UpperShift;
        static constexpr backing_uint_t OneCompleted = static_cast<backing_uint_t>(1) << UpperShift;

        void add_submitted() noexcept;
        BinaryCacheSyncState fetch_add_completed() noexcept;
        counter_uint_t fetch_incomplete_mark_submission_complete() noexcept;

    private:
        // This is morally:
        // struct State {
        //    counter_uint_t jobs_submitted;
        //    bool unused;
        //    counter_uint_t_minus_one_bit jobs_completed;
        //    bool submission_complete;
        // };
        std::atomic<backing_uint_t> m_state = 0;
    };

    struct BinaryCacheSyncState
    {
        BinaryCacheSynchronizer::counter_uint_t jobs_submitted;
        BinaryCacheSynchronizer::counter_uint_t jobs_completed;
        bool submission_complete;
    };

    // compression and upload of binary cache entries happens on a single 'background' thread, `m_push_thread`
    // Thread safety is achieved within the binary cache providers by:
    //   1. Only using one thread in the background for this work.
    //   2. Forming a queue of work for that thread to consume in `m_actions_to_push`, which maintains its own thread
    //   safety
    //   3. Sending any replies from the background thread through `m_bg_msg_sink`
    //   4. Ensuring any supporting data, such as tool exes, is provided before the background thread is started.
    //   5. Ensuring that work is not submitted to the background thread until the corresponding `packages` directory to
    //   upload is no longer being actively written by the foreground thread.
    struct BinaryCache : ReadOnlyBinaryCache
    {
        bool install_providers(DiagnosticContext& context, const VcpkgCmdArguments& args, const VcpkgPaths& paths);

        // fs must outlive the BinaryCache, and will be accessed from the background thread that does pushes
        explicit BinaryCache(const Filesystem& fs, Path packages);
        BinaryCache(const BinaryCache&) = delete;
        BinaryCache& operator=(const BinaryCache&) = delete;
        ~BinaryCache();
        /// Called upon a successful build of `action` to store those contents in the binary cache.
        void push_success(CleanPackages clean_packages, const InstallPlanAction& action);

        void print_updates();
        void wait_for_async_complete_and_join();

    private:
        struct ActionToPush
        {
            BinaryPackageWriteInfo request;
            CleanPackages clean_after_push;
        };
        bool m_needs_nuspec_data = false;
        bool m_needs_zip_file = false;

        CleanPackages m_clean_packages;

        BGMessageSink m_bg_msg_sink;
        BackgroundWorkQueue<ActionToPush> m_actions_to_push;
        BinaryCacheSynchronizer m_synchronizer;
        std::thread m_push_thread;

        void push_thread_main();
    };

    Optional<AssetCachingSettings> parse_download_configuration(DiagnosticContext& context,
                                                                const Optional<std::string>& arg);

    std::string generate_nuget_packages_config(const ActionPlan& action, StringView prefix);

    LocalizedString format_help_topic_asset_caching();
    LocalizedString format_help_topic_binary_caching();

    std::vector<std::vector<std::string>> batch_command_arguments_with_fixed_length(
        const std::vector<std::string>& entries,
        const std::size_t reserved_len,
        const std::size_t max_len,
        const std::size_t fixed_len,
        const std::size_t separator_len);
}

VCPKG_FORMAT_WITH_TO_STRING_LITERAL_NONMEMBER(vcpkg::BinaryCacheProviderKind);
VCPKG_FORMAT_WITH_TO_STRING_LITERAL_NONMEMBER(vcpkg::CacheAccessControl);
VCPKG_FORMAT_WITH_TO_STRING(vcpkg::BinaryCacheProviderEntry);
