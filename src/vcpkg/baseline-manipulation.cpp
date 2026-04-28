#include <vcpkg/base/contractual-constants.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>

#include <vcpkg/baseline-manipulation.h>

namespace
{
    void insert_version_to_json_object(vcpkg::Json::Object& obj,
                                       const vcpkg::Version& version,
                                       vcpkg::StringLiteral version_field)
    {
        obj.insert(version_field, vcpkg::Json::Value::string(version.text));
        obj.insert(vcpkg::JsonIdPortVersion, vcpkg::Json::Value::integer(version.port_version));
    }
}

namespace vcpkg
{
    Json::Object serialize_baseline(const std::map<std::string, Version, std::less<>>& baseline)
    {
        Json::Object port_entries_obj;
        for (auto&& kv_pair : baseline)
        {
            Json::Object baseline_version_obj;
            insert_version_to_json_object(baseline_version_obj, kv_pair.second, JsonIdBaseline);
            port_entries_obj.insert(kv_pair.first, std::move(baseline_version_obj));
        }

        Json::Object baseline_obj;
        baseline_obj.insert(JsonIdDefault, std::move(port_entries_obj));
        return baseline_obj;
    }

    void write_json_file(const Filesystem& fs, const Json::Object& obj, const Path& output_path)
    {
        const auto new_path = output_path + ".tmp";
        fs.create_directories(output_path.parent_path(), VCPKG_LINE_INFO);
        fs.write_contents(new_path, Json::stringify(obj), VCPKG_LINE_INFO);
        fs.rename(new_path, output_path, VCPKG_LINE_INFO);
    }
}