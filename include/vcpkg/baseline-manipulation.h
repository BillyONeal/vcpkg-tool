#pragma once

#include <map>
#include <string>

#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>
#include <vcpkg/versions.h>

namespace vcpkg
{
    Json::Object serialize_baseline(const std::map<std::string, Version, std::less<>>& baseline);
    void write_json_file(const Filesystem& fs, const Json::Object& obj, const Path& output_path);
}