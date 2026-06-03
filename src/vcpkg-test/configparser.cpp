#include <vcpkg-test/util.h>

#include <vcpkg/base/contractual-constants.h>
#include <vcpkg/base/diagnostics.h>
#include <vcpkg/base/message_sinks.h>
#include <vcpkg/base/util.h>

#include <vcpkg/binarycaching.h>

#include <algorithm>

using namespace vcpkg;

#if defined(_WIN32)
#define DEFAULT_ABSOLUTE_PATH "C:\\default"
#define ABSOLUTE_PATH "C:\\foo"
#else
#define DEFAULT_ABSOLUTE_PATH "/default"
#define ABSOLUTE_PATH "/foo"
#endif

namespace
{
    BinaryCacheParsedConfigs parse_binary_provider_arg_or_exit(StringView arg)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        std::vector<std::string> args;
        args.push_back(arg.to_string());
        auto result = parse_binary_provider_configs(context, DEFAULT_ABSOLUTE_PATH, {}, args);
        REQUIRE(std::move(context).to_string() == "");
        return result.value_or_exit(VCPKG_LINE_INFO);
    }

    BinaryCacheParsedConfigs parse_binary_provider_env_and_args_or_exit(StringView env_string,
                                                                        std::vector<std::string> args)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        auto result = parse_binary_provider_configs(context, DEFAULT_ABSOLUTE_PATH, env_string.to_string(), args);
        auto diagnostics = std::move(context).to_string();
        REQUIRE(std::move(context).to_string() == "");
        return result.value_or_exit(VCPKG_LINE_INFO);
    }

    std::string parse_binary_provider_arg_error(StringView arg)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        std::vector<std::string> args;
        args.push_back(arg.to_string());
        auto result = parse_binary_provider_configs(context, DEFAULT_ABSOLUTE_PATH, {}, args);
        auto diagnostics = std::move(context).to_string();
        INFO(diagnostics);
        REQUIRE(!result.has_value());
        return diagnostics;
    }

    std::string parse_binary_provider_env_error(StringView env_string)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        auto result = parse_binary_provider_configs(context, DEFAULT_ABSOLUTE_PATH, env_string.to_string(), {});
        auto diagnostics = std::move(context).to_string();
        INFO(diagnostics);
        REQUIRE(!result.has_value());
        return diagnostics;
    }

    std::string parse_binary_provider_env_and_args_error(StringView env_string, std::vector<std::string> args)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        auto result = parse_binary_provider_configs(context, DEFAULT_ABSOLUTE_PATH, env_string.to_string(), args);
        auto diagnostics = std::move(context).to_string();
        INFO(diagnostics);
        REQUIRE(!result.has_value());
        return diagnostics;
    }

    BinaryCacheParsedConfigs parse_binary_provider_configs_or_exit(StringView env_string,
                                                                   std::vector<std::string> args = {})
    {
        if (args.empty())
        {
            return parse_binary_provider_arg_or_exit(env_string);
        }

        return parse_binary_provider_env_and_args_or_exit(env_string, std::move(args));
    }

    std::string format_config_parse_error(Optional<StringView> origin,
                                          StringView source_text,
                                          std::size_t column,
                                          StringView message)
    {
        std::string result;
        if (const auto actual_origin = origin.get())
        {
            fmt::format_to(std::back_inserter(result), "{}:1:{}: error: {}", *actual_origin, column, message);
        }
        else
        {
            fmt::format_to(std::back_inserter(result), "error: {}", message);
        }

        result.push_back('\n');
        result.append(source_text.data(), source_text.size());
        result.push_back('\n');
        result.append(column - 1, ' ');
        result.push_back('^');
        return result;
    }

    std::size_t col_after(StringView prefix) { return prefix.size() + 1; }

    void require_binary_provider_parse_error(StringView arg, std::size_t column, StringView expected_message)
    {
        auto diagnostics = parse_binary_provider_arg_error(arg);
        REQUIRE_LINES(diagnostics, format_config_parse_error(nullopt, arg, column, expected_message));
    }

    void require_binary_provider_parse_env_error(StringView env_string, std::size_t column, StringView expected_message)
    {
        auto diagnostics = parse_binary_provider_env_error(env_string);
        REQUIRE_LINES(diagnostics,
                      format_config_parse_error(
                          format_environment_variable("VCPKG_BINARY_SOURCES"), env_string, column, expected_message));
    }

    void require_binary_provider_parse_arg_error(StringView env_string,
                                                 std::vector<std::string> args,
                                                 std::size_t column,
                                                 StringView expected_message)
    {
        REQUIRE(!args.empty());

        const auto source_text = args.back();
        auto diagnostics = parse_binary_provider_env_and_args_error(env_string, std::move(args));
        REQUIRE_LINES(diagnostics, format_config_parse_error(nullopt, source_text, column, expected_message));
    }

    AssetCachingSettings parse_asset_configuration_or_exit(Optional<std::string> arg)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        auto result = parse_download_configuration(context, arg);
        auto diagnostics = std::move(context).to_string();
        REQUIRE(diagnostics == "");
        return result.value_or_exit(VCPKG_LINE_INFO);
    }

    std::string parse_asset_configuration_error(const std::string& arg)
    {
        SinkBufferedDiagnosticContext context{null_sink};
        auto result = parse_download_configuration(context, arg);
        auto diagnostics = std::move(context).to_string();
        INFO(diagnostics);
        REQUIRE(!result.has_value());
        return diagnostics;
    }

    std::string format_asset_configuration_parse_error(StringView source_text,
                                                       std::size_t column,
                                                       StringView expected_message)
    {
        return format_config_parse_error(
            format_environment_variable(EnvironmentVariableXVcpkgAssetSources), source_text, column, expected_message);
    }

    void require_asset_configuration_parse_error(const std::string& arg,
                                                 std::size_t column,
                                                 StringView expected_message)
    {
        auto diagnostics = parse_asset_configuration_error(arg);
        REQUIRE_LINES(diagnostics, format_asset_configuration_parse_error(arg, column, expected_message));
    }

}

TEST_CASE ("BinaryConfigParser empty", "[binaryconfigparser]")
{
    auto parsed = parse_binary_provider_configs_or_exit("", {});

    REQUIRE(parsed.providers.size() == 1);
    CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                          CacheAccessControl::ReadWrite,
                                                          DEFAULT_ABSOLUTE_PATH});
    REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    REQUIRE(!parsed.nuget_interactive);
    REQUIRE(!parsed.aws_no_sign_request);
    REQUIRE(parsed.nuget_timeout == 100);
}

TEST_CASE ("BinaryConfigParser unacceptable provider", "[binaryconfigparser]")
{
    require_binary_provider_parse_env_error(
        "unacceptable",
        1,
        "unknown binary provider type: valid providers are 'clear', 'default', 'nuget', 'nugetconfig', "
        "'nugettimeout', 'interactive', 'x-azblob', 'x-gcs', 'x-aws', 'x-aws-config', 'http', and 'files'");
}

TEST_CASE ("BinaryConfigParser files provider", "[binaryconfigparser]")
{
    {
        require_binary_provider_parse_error(
            "files", col_after("files"), "binary config 'files' requires at least one path argument");
    }
    {
        require_binary_provider_parse_error(
            "files,relative-path", col_after("files,"), "path arguments for binary config strings must be absolute");
    }
    {
        require_binary_provider_parse_error(
            "files,C:foo", col_after("files,"), "path arguments for binary config strings must be absolute");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH, {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::ReadWrite, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"files"}});
    }
    {
        require_binary_provider_parse_error("files," ABSOLUTE_PATH ",nonsense",
                                            col_after("files," ABSOLUTE_PATH ","),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH ",read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::Read, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"files"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH ",write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::Write, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"files"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH ",readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::ReadWrite, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"files"}});
    }
    {
        require_binary_provider_parse_error("files," ABSOLUTE_PATH ",readwrite,extra",
                                            col_after("files," ABSOLUTE_PATH ",readwrite"),
                                            "binary config 'files' requires 1 or 2 arguments");
    }
    {
        require_binary_provider_parse_error(
            "files,,upload", col_after("files,"), "path arguments for binary config strings must be absolute");
    }
}

TEST_CASE ("BinaryConfigParser nuget source provider", "[binaryconfigparser]")
{
    {
        require_binary_provider_parse_error(
            "nuget", col_after("nuget"), "binary config 'nuget' requires at least one source argument");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nuget,relative-path", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGet, CacheAccessControl::ReadWrite, "relative-path"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nuget,http://example.org/", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGet,
                                                              CacheAccessControl::ReadWrite,
                                                              "http://example.org/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nuget," ABSOLUTE_PATH, {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGet, CacheAccessControl::ReadWrite, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        require_binary_provider_parse_error("nuget," ABSOLUTE_PATH ",nonsense",
                                            col_after("nuget," ABSOLUTE_PATH ","),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nuget," ABSOLUTE_PATH ",readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGet, CacheAccessControl::ReadWrite, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        require_binary_provider_parse_error("nuget," ABSOLUTE_PATH ",readwrite,extra",
                                            col_after("nuget," ABSOLUTE_PATH ",readwrite"),
                                            "binary config 'nuget' requires 1 or 2 arguments");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nuget,,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGet, CacheAccessControl::ReadWrite, ""});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
}

TEST_CASE ("BinaryConfigParser nuget timeout", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("nugettimeout,3601", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
        REQUIRE(parsed.nuget_timeout == 3601);
    }
    {
        require_binary_provider_parse_error("nugettimeout",
                                            col_after("nugettimeout"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,",
                                            col_after("nugettimeout,"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,nonsense",
                                            col_after("nugettimeout,"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,0",
                                            col_after("nugettimeout,"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,12x",
                                            col_after("nugettimeout,12"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,-321",
                                            col_after("nugettimeout,"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
    {
        require_binary_provider_parse_error("nugettimeout,321,123",
                                            col_after("nugettimeout,321"),
                                            "binary config 'nugettimeout' expects a single positive integer argument");
    }
}

TEST_CASE ("BinaryConfigParser nuget config provider", "[binaryconfigparser]")
{
    {
        require_binary_provider_parse_error(
            "nugetconfig", col_after("nugetconfig"), "binary config 'nugetconfig' requires at least one path argument");
    }
    {
        require_binary_provider_parse_error("nugetconfig,relative-path",
                                            col_after("nugetconfig,"),
                                            "path arguments for binary config strings must be absolute");
    }
    {
        require_binary_provider_parse_error("nugetconfig,http://example.org/",
                                            col_after("nugetconfig,"),
                                            "path arguments for binary config strings must be absolute");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nugetconfig," ABSOLUTE_PATH, {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGetConfig,
                                                              CacheAccessControl::ReadWrite,
                                                              ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        require_binary_provider_parse_error("nugetconfig," ABSOLUTE_PATH ",nonsense",
                                            col_after("nugetconfig," ABSOLUTE_PATH ","),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nugetconfig," ABSOLUTE_PATH ",read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGetConfig, CacheAccessControl::Read, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nugetconfig," ABSOLUTE_PATH ",write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGetConfig, CacheAccessControl::Write, ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("nugetconfig," ABSOLUTE_PATH ",readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::NuGetConfig,
                                                              CacheAccessControl::ReadWrite,
                                                              ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"nuget"}});
    }
    {
        require_binary_provider_parse_error("nugetconfig," ABSOLUTE_PATH ",readwrite,extra",
                                            col_after("nugetconfig," ABSOLUTE_PATH ",readwrite"),
                                            "binary config 'nugetconfig' requires 1 or 2 arguments");
    }
    {
        require_binary_provider_parse_error("nugetconfig,,readwrite",
                                            col_after("nugetconfig,"),
                                            "path arguments for binary config strings must be absolute");
    }
}

TEST_CASE ("BinaryConfigParser default provider", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("default", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        require_binary_provider_parse_error(
            "default,nonsense", col_after("default,"), "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("default,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::Read, DEFAULT_ABSOLUTE_PATH});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("default,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("default,write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::Write,
                                                              DEFAULT_ABSOLUTE_PATH});
    }
    {
        require_binary_provider_parse_error("default,read,extra",
                                            col_after("default,read"),
                                            "binary config 'default' does not take more than 1 argument");
    }
}

TEST_CASE ("BinaryConfigParser clear provider", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear", {});

        REQUIRE(parsed.providers.empty());
        REQUIRE(parsed.telemetry_tags.empty());
    }
    {
        require_binary_provider_parse_error(
            "clear,upload", col_after("clear"), "binary config 'clear' does not take arguments");
    }
}

TEST_CASE ("BinaryConfigParser interactive provider", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("interactive", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
        REQUIRE(parsed.nuget_interactive);
    }
    {
        require_binary_provider_parse_error(
            "interactive,read", col_after("interactive"), "binary config 'interactive' does not take arguments");
    }
}

TEST_CASE ("BinaryConfigParser multiple providers", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear;default", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear;default,read", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::Files, CacheAccessControl::Read, DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear;default,write", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::Write,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear;default,readwrite", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("clear;default,readwrite;clear;clear", {});

        REQUIRE(parsed.providers.empty());
        REQUIRE(parsed.telemetry_tags.empty());
    }
    {
        require_binary_provider_parse_error("clear;files,relative;default",
                                            col_after("clear;files,"),
                                            "path arguments for binary config strings must be absolute");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit(";;;clear;;;;", {});

        REQUIRE(parsed.providers.empty());
        REQUIRE(parsed.telemetry_tags.empty());
    }
    {
        require_binary_provider_parse_error(
            ";;;,;;;;",
            4,
            "unknown binary provider type: valid providers are 'clear', 'default', 'nuget', 'nugetconfig', "
            "'nugettimeout', 'interactive', 'x-azblob', 'x-gcs', 'x-aws', 'x-aws-config', 'http', and 'files'");
    }
}

TEST_CASE ("BinaryConfigParser escaping", "[binaryconfigparser]")
{
    constexpr StringLiteral trailing_backtick_error = "unexpected EOF after escape character";

    {
        require_binary_provider_parse_error(";;;;;;;`", col_after(";;;;;;;`"), trailing_backtick_error);
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit(";;;;;;;`defaul`t", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
    }
    {
        require_binary_provider_parse_error(
            "files," ABSOLUTE_PATH "`", col_after("files," ABSOLUTE_PATH "`"), trailing_backtick_error);
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH "`,", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              ABSOLUTE_PATH ","});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH "``", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              ABSOLUTE_PATH "`"});
    }
    {
        require_binary_provider_parse_error(
            "files," ABSOLUTE_PATH "```", col_after("files," ABSOLUTE_PATH "```"), trailing_backtick_error);
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH "````", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              ABSOLUTE_PATH "``"});
    }
    {
        require_binary_provider_parse_error("files," ABSOLUTE_PATH ",",
                                            col_after("files," ABSOLUTE_PATH ","),
                                            "expected 'read', 'readwrite', or 'write'");
    }
}

TEST_CASE ("BinaryConfigParser args", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH, std::vector<std::string>{"clear"});

        REQUIRE(parsed.providers.empty());
        REQUIRE(parsed.telemetry_tags.empty());
    }
    {
        auto parsed =
            parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH, std::vector<std::string>{"clear;default"});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{"default"});
    }
    {
        require_binary_provider_parse_arg_error("files," ABSOLUTE_PATH,
                                                std::vector<std::string>{"clear;default,"},
                                                col_after("clear;default,"),
                                                "expected 'read', 'readwrite', or 'write'");
    }
    {
        // being passesd as separate --binarysource args is equivalent to being all in one arg
        require_binary_provider_parse_arg_error("files," ABSOLUTE_PATH,
                                                std::vector<std::string>{"clear", "clear;default,"},
                                                col_after("clear;default,"),
                                                "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed =
            parse_binary_provider_configs_or_exit("files," ABSOLUTE_PATH, std::vector<std::string>{"clear", "clear"});

        REQUIRE(parsed.providers.empty());
        REQUIRE(parsed.telemetry_tags.empty());
    }
}

TEST_CASE ("BinaryConfigParser azblob provider", "[binaryconfigparser]")
{
    constexpr StringLiteral requires_azblob_https_base_url_error =
        "binary config 'azblob' requires a https:// base url as the first argument";

    {
        auto parsed = parse_binary_provider_configs_or_exit("x-azblob,https://azure/container,sas", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{
                  BinaryCacheProviderKind::AzBlob, CacheAccessControl::ReadWrite, "https://azure/container", "sas"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azblob"}, {"default"}});
    }
    {
        require_binary_provider_parse_error(
            "x-azblob,https://azure/container,?sas",
            col_after("x-azblob,https://azure/container,"),
            "binary config 'azblob' requires a SAS token without a preceeding '?' as the second argument");
    }
    {
        require_binary_provider_parse_error(
            "x-azblob,,sas", col_after("x-azblob,"), requires_azblob_https_base_url_error);
    }
    {
        require_binary_provider_parse_error("x-azblob,https://azure/container",
                                            col_after("x-azblob,https://azure/container"),
                                            "binary config 'azblob' requires at least a base-url and a SAS token");
    }
    {
        require_binary_provider_parse_error(
            "x-azblob,http://not/container,sas", col_after("x-azblob,"), requires_azblob_https_base_url_error);
    }
    {
        require_binary_provider_parse_error("x-azblob,https://azure/container,sas,invalid",
                                            col_after("x-azblob,https://azure/container,sas,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        require_binary_provider_parse_error("x-azblob,https://azure/container,sas,readwrite,extra",
                                            col_after("x-azblob,https://azure/container,sas,readwrite"),
                                            "binary config 'azblob' requires 2 or 3 arguments");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-azblob,https://azure/container,sas,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{
                  BinaryCacheProviderKind::AzBlob, CacheAccessControl::Read, "https://azure/container", "sas"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azblob"}, {"default"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-azblob,https://azure/container,sas,write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{
                  BinaryCacheProviderKind::AzBlob, CacheAccessControl::Write, "https://azure/container", "sas"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azblob"}, {"default"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-azblob,https://azure/container,sas,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{
                  BinaryCacheProviderKind::AzBlob, CacheAccessControl::ReadWrite, "https://azure/container", "sas"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azblob"}, {"default"}});
    }
}

TEST_CASE ("BinaryConfigParser azcopy providers", "[binaryconfigparser]")
{
    SECTION ("azcopy no SAS token")
    {
        constexpr StringLiteral requires_https_base_url_error =
            "binary config 'x-azcopy' requires a https:// base url as the first argument";

        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy,https://azure/container", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopy,
                                                                  CacheAccessControl::ReadWrite,
                                                                  "https://azure/container"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy"}, {"default"}});
        }
        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy,https://azure/container,read", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopy,
                                                                  CacheAccessControl::Read,
                                                                  "https://azure/container"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy"}, {"default"}});
        }
        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy,https://azure/container,write", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopy,
                                                                  CacheAccessControl::Write,
                                                                  "https://azure/container"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy"}, {"default"}});
        }
        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy,https://azure/container,readwrite", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopy,
                                                                  CacheAccessControl::ReadWrite,
                                                                  "https://azure/container"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy"}, {"default"}});
        }
        {
            require_binary_provider_parse_error("x-azcopy", col_after("x-azcopy"), requires_https_base_url_error);
        }
        {
            require_binary_provider_parse_error(
                "x-azcopy,http://not/container", col_after("x-azcopy,"), requires_https_base_url_error);
        }
        {
            require_binary_provider_parse_error(
                "x-azcopy,,readwrite", col_after("x-azcopy,"), requires_https_base_url_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy,https://azure/container,",
                                                col_after("x-azcopy,https://azure/container,"),
                                                "expected 'read', 'readwrite', or 'write'");
        }
        {
            require_binary_provider_parse_error("x-azcopy,https://azure/container,?sas",
                                                col_after("x-azcopy,https://azure/container,"),
                                                "expected 'read', 'readwrite', or 'write'");
        }
        {
            require_binary_provider_parse_error("x-azcopy,https://azure/container,sas,readwrite",
                                                col_after("x-azcopy,https://azure/container,sas,"),
                                                "binary config 'x-azcopy' requires 1 or 2 arguments");
        }
    }

    SECTION ("azcopy with SAS token")
    {
        constexpr StringLiteral requires_azcopy_sas_https_base_url_error =
            "binary config 'x-azcopy-sas' requires a https:// base url as the first argument";
        constexpr StringLiteral requires_valid_token_azcopy_sas_error =
            "binary config 'x-azcopy-sas' requires a SAS token without a preceeding '?' as the second argument";
        constexpr StringLiteral requires_base_url_and_token_azcopy_sas_error =
            "binary config 'x-azcopy-sas' requires at least a base-url and a SAS token";

        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy-sas,https://azure/container,sas", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopySas,
                                                                  CacheAccessControl::ReadWrite,
                                                                  "https://azure/container",
                                                                  "sas"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy-sas"}, {"default"}});
        }
        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy-sas,https://azure/container,sas,read", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] ==
                  BinaryCacheProviderEntry{
                      BinaryCacheProviderKind::AzCopySas, CacheAccessControl::Read, "https://azure/container", "sas"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy-sas"}, {"default"}});
        }
        {
            auto parsed = parse_binary_provider_configs_or_exit("x-azcopy-sas,https://azure/container,sas,write", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] ==
                  BinaryCacheProviderEntry{
                      BinaryCacheProviderKind::AzCopySas, CacheAccessControl::Write, "https://azure/container", "sas"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy-sas"}, {"default"}});
        }
        {
            auto parsed =
                parse_binary_provider_configs_or_exit("x-azcopy-sas,https://azure/container,sas,readwrite", {});

            REQUIRE(parsed.providers.size() == 2);
            CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                                  CacheAccessControl::ReadWrite,
                                                                  DEFAULT_ABSOLUTE_PATH});
            CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzCopySas,
                                                                  CacheAccessControl::ReadWrite,
                                                                  "https://azure/container",
                                                                  "sas"});
            REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"azcopy-sas"}, {"default"}});
        }
        {
            require_binary_provider_parse_error(
                "x-azcopy-sas", col_after("x-azcopy-sas"), requires_base_url_and_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error(
                "x-azcopy-sas,,sas,readwrite", col_after("x-azcopy-sas,"), requires_azcopy_sas_https_base_url_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,http://not/container",
                                                col_after("x-azcopy-sas,http://not/container"),
                                                requires_base_url_and_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,http://not/container,sas",
                                                col_after("x-azcopy-sas,"),
                                                requires_azcopy_sas_https_base_url_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container",
                                                col_after("x-azcopy-sas,https://azure/container"),
                                                requires_base_url_and_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container,",
                                                col_after("x-azcopy-sas,https://azure/container,"),
                                                requires_valid_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container,?sas",
                                                col_after("x-azcopy-sas,https://azure/container,"),
                                                requires_valid_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container,,readwrite",
                                                col_after("x-azcopy-sas,https://azure/container,"),
                                                requires_valid_token_azcopy_sas_error);
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container,sas,invalid",
                                                col_after("x-azcopy-sas,https://azure/container,sas,"),
                                                "expected 'read', 'readwrite', or 'write'");
        }
        {
            require_binary_provider_parse_error("x-azcopy-sas,https://azure/container,sas,readwrite,extra",
                                                col_after("x-azcopy-sas,https://azure/container,sas,readwrite"),
                                                "binary config 'x-azcopy-sas' requires 2 or 3 arguments");
        }
    }
}

TEST_CASE ("BinaryConfigParser GCS provider", "[binaryconfigparser]")
{
    {
        require_binary_provider_parse_error(
            "x-gcs", col_after("x-gcs"), "binary config 'gcs' requires a gs:// base url as the first argument");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-gcs,gs://my-bucket/", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::GCS, CacheAccessControl::ReadWrite, "gs://my-bucket/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"gcs"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-gcs,gs://my-bucket/my-folder", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::GCS,
                                                              CacheAccessControl::ReadWrite,
                                                              "gs://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"gcs"}});
    }
    {
        require_binary_provider_parse_error(
            "x-gcs,", col_after("x-gcs,"), "binary config 'gcs' requires a gs:// base url as the first argument");
    }
    {
        require_binary_provider_parse_error("x-gcs,gs://my-bucket/my-folder,invalid",
                                            col_after("x-gcs,gs://my-bucket/my-folder,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-gcs,gs://my-bucket/my-folder,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::GCS,
                                                              CacheAccessControl::Read,
                                                              "gs://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"gcs"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-gcs,gs://my-bucket/my-folder,write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::GCS,
                                                              CacheAccessControl::Write,
                                                              "gs://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"gcs"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-gcs,gs://my-bucket/my-folder,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::GCS,
                                                              CacheAccessControl::ReadWrite,
                                                              "gs://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"gcs"}});
    }
}

TEST_CASE ("BinaryConfigParser AWS provider", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws,s3://my-bucket/", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::AWS, CacheAccessControl::ReadWrite, "s3://my-bucket/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"aws"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws,s3://my-bucket/my-folder", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AWS,
                                                              CacheAccessControl::ReadWrite,
                                                              "s3://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"aws"}});
    }
    {
        require_binary_provider_parse_error(
            "x-aws,", col_after("x-aws,"), "binary config 'aws' requires a s3:// base url as the first argument");
    }
    {
        require_binary_provider_parse_error("x-aws,s3://my-bucket/my-folder,invalid",
                                            col_after("x-aws,s3://my-bucket/my-folder,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws,s3://my-bucket/my-folder,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AWS,
                                                              CacheAccessControl::Read,
                                                              "s3://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"aws"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws,s3://my-bucket/my-folder,write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AWS,
                                                              CacheAccessControl::Write,
                                                              "s3://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"aws"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws,s3://my-bucket/my-folder,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AWS,
                                                              CacheAccessControl::ReadWrite,
                                                              "s3://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"aws"}});
    }
}

TEST_CASE ("BinaryConfigParser AWS config provider", "[binaryconfigparser]")
{
    constexpr StringLiteral requires_single_string_argument_error =
        "binary config 'x-aws-config' expects a single string argument";

    {
        auto parsed = parse_binary_provider_configs_or_exit("x-aws-config,no-sign-request", {});

        REQUIRE(parsed.providers.size() == 1);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        REQUIRE(parsed.aws_no_sign_request);
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}});
    }
    {
        require_binary_provider_parse_error(
            "x-aws-config", col_after("x-aws-config"), requires_single_string_argument_error);
    }
    {
        require_binary_provider_parse_error("x-aws-config,invalid", col_after("x-aws-config,"), "invalid argument");
    }
    {
        require_binary_provider_parse_error("x-aws-config,no-sign-request,extra",
                                            col_after("x-aws-config,no-sign-request"),
                                            requires_single_string_argument_error);
    }
}

TEST_CASE ("BinaryConfigParser COS provider", "[binaryconfigparser]")
{
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-cos,cos://my-bucket/", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] ==
              BinaryCacheProviderEntry{BinaryCacheProviderKind::COS, CacheAccessControl::ReadWrite, "cos://my-bucket/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"cos"}, {"default"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-cos,cos://my-bucket/my-folder", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::COS,
                                                              CacheAccessControl::ReadWrite,
                                                              "cos://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"cos"}, {"default"}});
    }
    {
        require_binary_provider_parse_error(
            "x-cos,", col_after("x-cos,"), "binary config 'cos' requires a cos:// base url as the first argument");
    }
    {
        require_binary_provider_parse_error("x-cos,cos://my-bucket/my-folder,invalid",
                                            col_after("x-cos,cos://my-bucket/my-folder,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-cos,cos://my-bucket/my-folder,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::COS,
                                                              CacheAccessControl::Read,
                                                              "cos://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"cos"}, {"default"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-cos,cos://my-bucket/my-folder,write", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::COS,
                                                              CacheAccessControl::Write,
                                                              "cos://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"cos"}, {"default"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("x-cos,cos://my-bucket/my-folder,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::COS,
                                                              CacheAccessControl::ReadWrite,
                                                              "cos://my-bucket/my-folder/"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"cos"}, {"default"}});
    }
}

TEST_CASE ("BinaryConfigParser HTTP provider", "[binaryconfigparser]")
{
    {
        require_binary_provider_parse_error(
            "http", col_after("http"), "binary config 'http' requires a https:// base url as the first argument");
    }
    {
        require_binary_provider_parse_error("http,ftp://example.org/",
                                            col_after("http,"),
                                            "binary config 'http' requires a https:// base url as the first argument");
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("http,http://example.org/", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Http,
                                                              CacheAccessControl::ReadWrite,
                                                              "http://example.org/{sha}.zip"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"http"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("http,http://example.org", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Http,
                                                              CacheAccessControl::ReadWrite,
                                                              "http://example.org/{sha}.zip"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"http"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit("http,http://example.org/{triplet}/{sha}", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Http,
                                                              CacheAccessControl::ReadWrite,
                                                              "http://example.org/{triplet}/{sha}"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"http"}});
    }
    {
        require_binary_provider_parse_error(
            "http,http://example.org/{triplet}",
            col_after("http,"),
            "the {sha} variable must be used in the template if other variables are used");
    }
    {
        require_binary_provider_parse_error("http,http://example.org/{other}/{sha}",
                                            col_after("http,http://example.org/"),
                                            "template contains unknown variable: other");
    }
    {
        require_binary_provider_parse_error("http,http://example.org/,invalid",
                                            col_after("http,http://example.org/,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
    {
        require_binary_provider_parse_error("http,http://example.org/,read,header,extra",
                                            col_after("http,http://example.org/,read,header"),
                                            "binary config 'http' requires 2 or 3 arguments");
    }
}

TEST_CASE ("BinaryConfigParser Universal Packages provider", "[binaryconfigparser]")
{
    constexpr StringLiteral requires_four_or_five_arguments_error =
        "binary config 'Universal Packages' requires 4 or 5 arguments";

    // Scheme: x-az-universal,<organization>,<project>,<feed>[,<readwrite>]
    {
        auto parsed = parse_binary_provider_configs_or_exit(
            "x-az-universal,test_organization,test_project_name,test_feed,read", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzUniversal,
                                                              CacheAccessControl::Read,
                                                              "test_organization",
                                                              "test_project_name",
                                                              "test_feed"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"upkg"}});
    }
    {
        auto parsed = parse_binary_provider_configs_or_exit(
            "x-az-universal,test_organization,test_project_name,test_feed,readwrite", {});

        REQUIRE(parsed.providers.size() == 2);
        CHECK(parsed.providers[0] == BinaryCacheProviderEntry{BinaryCacheProviderKind::Files,
                                                              CacheAccessControl::ReadWrite,
                                                              DEFAULT_ABSOLUTE_PATH});
        CHECK(parsed.providers[1] == BinaryCacheProviderEntry{BinaryCacheProviderKind::AzUniversal,
                                                              CacheAccessControl::ReadWrite,
                                                              "test_organization",
                                                              "test_project_name",
                                                              "test_feed"});
        REQUIRE(parsed.telemetry_tags == std::set<StringLiteral>{{"default"}, {"upkg"}});
    }
    {
        require_binary_provider_parse_error(
            "x-az-universal,test_organization,test_project_name,test_feed,extra_argument,readwrite",
            col_after("x-az-universal,test_organization,test_project_name,test_feed,extra_argument"),
            requires_four_or_five_arguments_error);
    }
    {
        require_binary_provider_parse_error("x-az-universal,missing_args,read",
                                            col_after("x-az-universal,missing_args,read"),
                                            requires_four_or_five_arguments_error);
    }
    {
        require_binary_provider_parse_error("x-az-universal,test_organization,test_project_name,test_feed,invalid",
                                            col_after("x-az-universal,test_organization,test_project_name,test_feed,"),
                                            "expected 'read', 'readwrite', or 'write'");
    }
}

TEST_CASE ("AssetConfigParser azurl provider", "[assetconfigparser]")
{
    CHECK(parse_asset_configuration_or_exit(nullopt).m_read_url_template == nullopt);

    require_asset_configuration_parse_error(
        "x-azurl", col_after("x-azurl"), "unexpected arguments: asset config 'azurl' requires a base url");
    require_asset_configuration_parse_error(
        "x-azurl,", col_after("x-azurl,"), "unexpected arguments: asset config 'azurl' requires a base url");
    require_asset_configuration_parse_error(
        "x-azurl,value,,", col_after("x-azurl,value,,"), "expected 'read', 'readwrite', or 'write'");
    require_asset_configuration_parse_error(
        "x-azurl,value,,invalid", col_after("x-azurl,value,,"), "expected 'read', 'readwrite', or 'write'");
    require_asset_configuration_parse_error(
        "x-azurl,value,,readwrite,",
        col_after("x-azurl,value,,readwrite,"),
        "unexpected arguments: asset config 'azurl' requires fewer than 4 arguments");

    {
        AssetCachingSettings empty;
        CHECK(empty.m_write_headers.empty());
        CHECK(empty.m_read_headers.empty());
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123,foo");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>?foo");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == nullopt);
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123/,foo");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>?foo");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == nullopt);
        CHECK(dm.m_secrets == std::vector<std::string>{"foo"});
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123,?foo");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>?foo");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == nullopt);
        CHECK(dm.m_secrets == std::vector<std::string>{"?foo"});
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == nullopt);
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123,,readwrite");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == "https://abc/123/<SHA>");
        Test::check_ranges(dm.m_write_headers, azure_blob_headers());
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-azurl,https://abc/123,foo,readwrite");
        CHECK(dm.m_read_url_template == "https://abc/123/<SHA>?foo");
        CHECK(dm.m_read_headers.empty());
        CHECK(dm.m_write_url_template == "https://abc/123/<SHA>?foo");
        Test::check_ranges(dm.m_write_headers, azure_blob_headers());
        CHECK(dm.m_secrets == std::vector<std::string>{"foo"});
    }
    {
        AssetCachingSettings dm = parse_asset_configuration_or_exit("x-script,powershell {SHA} {URL}");
        CHECK(!dm.m_read_url_template.has_value());
        CHECK(dm.m_read_headers.empty());
        CHECK(!dm.m_write_url_template.has_value());
        CHECK(dm.m_write_headers.empty());
        CHECK(dm.m_secrets.empty());
        CHECK(dm.m_script.value_or_exit(VCPKG_LINE_INFO) == "powershell {SHA} {URL}");
    }
}

TEST_CASE ("AssetConfigParser clear provider", "[assetconfigparser]")
{
    CHECK(parse_asset_configuration_or_exit("clear").m_read_url_template == nullopt);
    require_asset_configuration_parse_error(
        "clear,", col_after("clear"), "unexpected arguments: 'clear' does not accept arguments");
    CHECK(parse_asset_configuration_or_exit("x-azurl,value;clear").m_read_url_template == nullopt);
    auto value_or = [](auto o, auto v) {
        if (o)
            return std::move(*o.get());
        else
            return std::move(v);
    };

    AssetCachingSettings empty;

    CHECK(
        value_or(Optional<AssetCachingSettings>{parse_asset_configuration_or_exit("x-azurl,https://abc/123,foo;clear")},
                 empty)
            .m_read_url_template == nullopt);
    CHECK(value_or(
              Optional<AssetCachingSettings>{parse_asset_configuration_or_exit("clear;x-azurl,https://abc/123/,foo")},
              empty)
              .m_read_url_template == "https://abc/123/<SHA>?foo");
}

TEST_CASE ("AssetConfigParser x-block-origin provider", "[assetconfigparser]")
{
    CHECK(parse_asset_configuration_or_exit("x-block-origin").m_block_origin);
    require_asset_configuration_parse_error("x-block-origin,",
                                            col_after("x-block-origin"),
                                            "unexpected arguments: 'x-block-origin' does not accept arguments");
    auto value_or = [](auto o, auto v) {
        if (o)
            return std::move(*o.get());
        else
            return std::move(v);
    };

    AssetCachingSettings empty;

    CHECK(!value_or(Optional<AssetCachingSettings>{parse_asset_configuration_or_exit(nullopt)}, empty).m_block_origin);
    CHECK(value_or(Optional<AssetCachingSettings>{parse_asset_configuration_or_exit("x-block-origin")}, empty)
              .m_block_origin);
    CHECK(!value_or(Optional<AssetCachingSettings>{parse_asset_configuration_or_exit("x-block-origin;clear")}, empty)
               .m_block_origin);
}

TEST_CASE ("AssetConfigParser other errors", "[assetconfigparser]")
{
    require_asset_configuration_parse_error(
        "x-script",
        col_after("x-script"),
        "expected arguments: asset config 'x-script' requires exactly the exec template as an argument");
    require_asset_configuration_parse_error(
        "x-script,powershell,extra",
        col_after("x-script,powershell,"),
        "expected arguments: asset config 'x-script' requires exactly the exec template as an argument");
    require_asset_configuration_parse_error(
        "unacceptable",
        1,
        "unknown asset provider type: valid source types are 'x-azurl', 'x-script', 'x-block-origin', and 'clear'");
    require_asset_configuration_parse_error("x-azurl,https://abc/123;x-azurl,https://def/456",
                                            col_after("x-azurl,https://abc/123;x-azurl,"),
                                            "a maximum of one asset read url can be specified");
    require_asset_configuration_parse_error("x-azurl,https://abc/123,,write;x-azurl,https://def/456,,write",
                                            col_after("x-azurl,https://abc/123,,write;x-azurl,"),
                                            "a maximum of one asset write url can be specified");
    require_asset_configuration_parse_error(
        "x-script,powershell `", col_after("x-script,powershell `"), "unexpected EOF after escape character");

    auto diagnostics = parse_asset_configuration_error("x-script,\xFF");
    REQUIRE_LINES(diagnostics,
                  format_asset_configuration_parse_error("x-script,", col_after("x-script,"), "invalid code unit"));
}
