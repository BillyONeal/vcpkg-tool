#include <vcpkg/base/contractual-constants.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/util.h>

#include <vcpkg/commands.version.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/spdx.h>

using namespace vcpkg;

static void append_move_if_exists_and_array(Json::Array& out, Json::Object& obj, StringView property)
{
    if (auto p = obj.get(property))
    {
        if (auto arr = p->maybe_array())
        {
            for (auto& e : *arr)
            {
                out.push_back(std::move(e));
            }
        }
    }
}

std::string vcpkg::create_spdx_sbom(const InstallPlanAction& action,
                                    View<Path> relative_paths,
                                    View<std::string> hashes,
                                    View<Path> relative_package_paths,
                                    View<std::string> package_hashes,
                                    std::string created_time,
                                    std::string document_namespace,
                                    Json::Object&& resource_doc)
{
    Checks::check_exit(VCPKG_LINE_INFO, relative_paths.size() == hashes.size());
    Checks::check_exit(VCPKG_LINE_INFO, relative_package_paths.size() == package_hashes.size());

    const auto& scfl = action.source_control_file_and_location();
    const auto& cpgh = *scfl.source_control_file->core_paragraph;
    StringView abi{SpdxNone};
    if (auto package_abi = action.package_abi())
    {
        abi = *package_abi;
    }

    const auto stringized_license = calculate_spdx_license(action);
    Json::Object doc;
    doc.insert(JsonIdDollarSchema, "https://raw.githubusercontent.com/spdx/spdx-spec/v2.3/schemas/spdx-schema.json");
    doc.insert(SpdxVersion, SpdxTwoThree);
    doc.insert(SpdxDataLicense, SpdxCCZero);
    doc.insert(SpdxSpdxId, SpdxRefDocument);
    doc.insert(SpdxDocumentNamespace, std::move(document_namespace));
    doc.insert(JsonIdName, fmt::format("{}@{} {}", action.spec, cpgh.version, abi));
    {
        auto& cinfo = doc.insert(SpdxCreationInfo, Json::Object());
        auto& creators = cinfo.insert(JsonIdCreators, Json::Array());
        creators.push_back(Strings::concat("Tool: vcpkg-", VCPKG_BASE_VERSION_AS_STRING, '-', VCPKG_VERSION_AS_STRING));
        cinfo.insert(JsonIdCreated, std::move(created_time));
    }

    auto& rels = doc.insert(JsonIdRelationships, Json::Array());
    auto& packages = doc.insert(JsonIdPackages, Json::Array());
    {
        auto& obj = packages.push_back(Json::Object());
        obj.insert(JsonIdName, action.spec.name());
        obj.insert(SpdxSpdxId, SpdxRefPort);
        obj.insert(SpdxVersionInfo, cpgh.version.to_string());
        obj.insert(SpdxDownloadLocation, scfl.spdx_location.empty() ? StringView{SpdxNoAssertion} : scfl.spdx_location);
        if (!cpgh.homepage.empty())
        {
            obj.insert(JsonIdHomepage, cpgh.homepage);
        }
        obj.insert(SpdxLicenseConcluded, stringized_license);
        obj.insert(SpdxLicenseDeclared, SpdxNoAssertion);
        obj.insert(SpdxCopyrightText, SpdxNoAssertion);
        if (!cpgh.summary.empty()) obj.insert(JsonIdSummary, Strings::join("\n", cpgh.summary));
        if (!cpgh.description.empty()) obj.insert(JsonIdDescription, Strings::join("\n", cpgh.description));
        obj.insert(JsonIdComment, "This is the port (recipe) consumed by vcpkg.");
        {
            auto& external_refs = obj.insert(SpdxExternalRefs, Json::Array());
            auto& purl_ref = external_refs.push_back(Json::Object());
            purl_ref.insert(SpdxExternalReferenceCategory, SpdxExternalReferenceCategoryPackageManager);
            purl_ref.insert(SpdxExternalReferenceType, SpdxExternalReferenceTypePurl);
            purl_ref.insert(SpdxExternalReferenceLocator, make_vcpkg_purl(action));

            if (!scfl.git_tree.empty())
            {
                auto& gitoid_ref = external_refs.push_back(Json::Object());
                gitoid_ref.insert(SpdxExternalReferenceCategory, SpdxExternalReferenceCategoryPersistentId);
                gitoid_ref.insert(SpdxExternalReferenceType, SpdxExternalReferenceTypeGitoid);
                gitoid_ref.insert(SpdxExternalReferenceLocator, fmt::format("gitoid:tree:sha1:{}", scfl.git_tree));
            }
        }
        {
            auto& rel = rels.push_back(Json::Object());
            rel.insert(SpdxElementId, SpdxRefPort);
            rel.insert(SpdxRelationshipType, SpdxGenerates);
            rel.insert(SpdxRelatedSpdxElement, SpdxRefBinary);
        }
    }
    {
        auto& obj = packages.push_back(Json::Object());
        obj.insert(JsonIdName, action.spec.to_string());
        obj.insert(SpdxSpdxId, SpdxRefBinary);
        obj.insert(SpdxVersionInfo, abi);
        obj.insert(SpdxDownloadLocation, SpdxNone);
        obj.insert(SpdxLicenseConcluded, stringized_license);
        obj.insert(SpdxLicenseDeclared, SpdxNoAssertion);
        obj.insert(SpdxCopyrightText, SpdxNoAssertion);
        obj.insert(JsonIdComment, "This is a binary package built by vcpkg.");
    }

    auto& files = doc.insert(JsonIdFiles, Json::Array());

    // Helper function to process files and create relationships
    auto process_files =
        [&files,
         &rels](View<Path> paths, View<std::string> file_hashes, StringLiteral ref_name, StringLiteral ref_type) {
            for (size_t i = 0; i < paths.size(); ++i)
            {
                const auto& path = paths[i];
                const auto& hash = file_hashes[i];

                auto& obj = files.push_back(Json::Object());
                obj.insert(SpdxFileName, "./" + path.generic_u8string());
                const auto ref = fmt::format("SPDXRef-{}-file-{}", ref_name, i);
                obj.insert(SpdxSpdxId, ref);
                auto& checksum = obj.insert(JsonIdChecksums, Json::Array());
                auto& checksum1 = checksum.push_back(Json::Object());
                checksum1.insert(JsonIdAlgorithm, JsonIdAllCapsSHA256);
                checksum1.insert(SpdxChecksumValue, hash);
                obj.insert(SpdxLicenseConcluded, SpdxNoAssertion);
                obj.insert(SpdxCopyrightText, SpdxNoAssertion);
                {
                    auto& rel = rels.push_back(Json::Object());
                    rel.insert(SpdxElementId, ref_type);
                    rel.insert(SpdxRelationshipType, SpdxContains);
                    rel.insert(SpdxRelatedSpdxElement, ref);
                }
                if (path == FileVcpkgDotJson && ref_type == SpdxRefPort)
                {
                    auto& rel = rels.push_back(Json::Object());
                    rel.insert(SpdxElementId, ref);
                    rel.insert(SpdxRelationshipType, SpdxDependencyManifestOf);
                    rel.insert(SpdxRelatedSpdxElement, ref_type);
                }
            }
        };

    // Process port files first, then package files
    process_files(relative_paths, hashes, "port", SpdxRefPort);
    process_files(relative_package_paths, package_hashes, "binary", SpdxRefBinary);

    append_move_if_exists_and_array(packages, resource_doc, JsonIdPackages);

    return Json::stringify(doc);
}

static std::vector<SpdxApplicableLicenseExpression> calculate_spdx_licenses(const InstallPlanAction& action)
{
    const auto& scfl = action.source_control_file_and_location();
    const auto& scf = *scfl.source_control_file;
    const auto& cpgh = *scf.core_paragraph;
    bool all_licenses_undeclared = cpgh.license.kind() == SpdxLicenseDeclarationKind::NotPresent;
    bool any_explicitly_null_licenses = cpgh.license.kind() == SpdxLicenseDeclarationKind::Null;
    std::vector<SpdxApplicableLicenseExpression> licenses = cpgh.license.applicable_licenses();
    for (const auto& feature_name : action.feature_list)
    {
        if (feature_name == FeatureNameCore)
        {
            continue;
        }

        const auto* feature = scf.find_feature(feature_name);
        Checks::check_exit(VCPKG_LINE_INFO, feature != nullptr);
        const auto& feature_appplicable_licenses = feature->license.applicable_licenses();
        all_licenses_undeclared &= feature->license.kind() == SpdxLicenseDeclarationKind::NotPresent;
        any_explicitly_null_licenses |= feature->license.kind() == SpdxLicenseDeclarationKind::Null;
        licenses.insert(licenses.end(), feature_appplicable_licenses.begin(), feature_appplicable_licenses.end());
    }

    if (all_licenses_undeclared)
    {
        licenses.push_back({SpdxNoAssertion.to_string(), false});
    }
    else
    {
        if (any_explicitly_null_licenses)
        {
            licenses.push_back({SpdxLicenseRefVcpkgNull.to_string(), false});
        }

        Util::sort_unique_erase(licenses);
    }

    return licenses;
}

std::string vcpkg::calculate_spdx_license(const InstallPlanAction& action)
{
    return Strings::join(
        " AND ", calculate_spdx_licenses(action), [](const auto& license) { return license.to_string(); });
}

namespace
{
    struct Spdx3Document
    {
        std::string document_namespace;
        Json::Array graph;
        size_t next_id = 0;

        std::string id(StringView local) const { return Strings::concat(document_namespace, '#', local); }

        std::string unique_id(StringView kind) { return id(fmt::format("SPDXRef-{}-{}", kind, next_id++)); }

        Json::Object element(StringView type, StringView spdx_id) const
        {
            Json::Object result;
            result.insert("type", type);
            result.insert("spdxId", spdx_id);
            result.insert("creationInfo", "_:creation");
            return result;
        }

        std::string relationship(StringView from, StringView type, StringView to)
        {
            auto result_id = unique_id("relationship");
            auto result = element("Relationship", result_id);
            result.insert("from", from);
            result.insert("relationshipType", type);
            result.insert("to", Json::Array()).push_back(to);
            graph.push_back(std::move(result));
            return result_id;
        }

        void annotate(StringView subject, Json::Object&& fields)
        {
            if (fields.is_empty()) return;
            auto result = element("Annotation", unique_id("annotation"));
            result.insert("subject", subject);
            result.insert("annotationType", "other");
            result.insert("contentType", "application/json");
            result.insert("statement", Json::stringify(fields));
            graph.push_back(std::move(result));
        }

        static bool needs_license_annotation(StringView text)
        {
            return text.empty() || text.to_string().find("LicenseRef-") != std::string::npos;
        }

        std::string license(StringView text)
        {
            if (text == SpdxNoAssertion || needs_license_annotation(text))
            {
                return "expandedlicensing_NoAssertionLicense";
            }
            if (text == SpdxNone) return "expandedlicensing_NoneLicense";
            auto result_id = unique_id("license");
            auto result = element("simplelicensing_LicenseExpression", result_id);
            result.insert("simplelicensing_licenseExpression", text);
            graph.push_back(std::move(result));
            return result_id;
        }

        std::string licenses(View<SpdxApplicableLicenseExpression> expressions)
        {
            std::vector<std::string> members;
            for (const auto& expression : expressions)
            {
                members.push_back(license(expression.to_string()));
            }
            Util::sort_unique_erase(members);
            if (members.empty()) return license(SpdxNoAssertion);
            if (members.size() == 1) return std::move(members.front());
            auto result_id = unique_id("license");
            auto result = element("expandedlicensing_ConjunctiveLicenseSet", result_id);
            auto& member_ids = result.insert("expandedlicensing_member", Json::Array());
            for (const auto& member : members)
                member_ids.push_back(member);
            graph.push_back(std::move(result));
            return result_id;
        }

        void license_relationship(StringView subject, StringView type, StringView target, StringView expression)
        {
            auto relation = relationship(subject, type, target);
            if (needs_license_annotation(expression))
            {
                // LicenseRef text is not available in manifest metadata; do not invent license terms.
                Json::Object original;
                original.insert("licenseExpression", expression);
                annotate(relation, std::move(original));
            }
        }

        void download_location(Json::Object& package, StringView location)
        {
            if (location == SpdxNone || location == SpdxNoAssertion)
            {
                Json::Object original;
                original.insert(SpdxDownloadLocation, location);
                annotate(package["spdxId"].string(VCPKG_LINE_INFO), std::move(original));
            }
            else
            {
                package.insert("software_downloadLocation", location);
            }
        }

        static Json::Object hash(StringView algorithm, StringView value)
        {
            Json::Object result;
            result.insert("type", "Hash");
            result.insert("algorithm", algorithm);
            result.insert("hashValue", value);
            return result;
        }

        void resource(const Json::Object& original, size_t index)
        {
            auto remaining = original;
            const auto resource_id = id(fmt::format("SPDXRef-resource-{}", index));
            auto package = element("software_Package", resource_id);
            package.insert(JsonIdName, original[JsonIdName].string(VCPKG_LINE_INFO));
            remaining.remove(JsonIdName);
            if (auto old_id = remaining.get(SpdxSpdxId))
            {
                if (old_id->string(VCPKG_LINE_INFO) == fmt::format("SPDXRef-resource-{}", index))
                {
                    remaining.remove(SpdxSpdxId);
                }
            }
            for (const auto& mapping : {std::pair<StringView, StringView>{SpdxVersionInfo, "software_packageVersion"},
                                        {JsonIdHomepage, "software_homePage"},
                                        {JsonIdSummary, JsonIdSummary},
                                        {JsonIdDescription, JsonIdDescription},
                                        {JsonIdComment, JsonIdComment},
                                        {SpdxCopyrightText, "software_copyrightText"}})
            {
                if (auto value = remaining.get(mapping.first))
                {
                    package.insert(mapping.second, value->string(VCPKG_LINE_INFO));
                    remaining.remove(mapping.first);
                }
            }
            if (auto location = remaining.get(SpdxDownloadLocation))
            {
                download_location(package, location->string(VCPKG_LINE_INFO));
                remaining.remove(SpdxDownloadLocation);
            }
            for (const auto& mapping : {std::pair<StringView, StringView>{SpdxLicenseConcluded, "hasConcludedLicense"},
                                        {SpdxLicenseDeclared, "hasDeclaredLicense"}})
            {
                if (auto value = remaining.get(mapping.first))
                {
                    const auto expression = value->string(VCPKG_LINE_INFO);
                    license_relationship(resource_id, mapping.second, license(expression), expression);
                    remaining.remove(mapping.first);
                }
            }
            Json::Object distribution;
            if (auto filename = remaining.get(SpdxPackageFileName))
            {
                auto distribution_id = id(fmt::format("SPDXRef-resource-{}-distribution", index));
                distribution = element("software_File", distribution_id);
                distribution.insert(JsonIdName, filename->string(VCPKG_LINE_INFO));
                relationship(resource_id, "hasDistributionArtifact", distribution_id);
                remaining.remove(SpdxPackageFileName);
            }
            if (auto checksums = remaining.get(JsonIdChecksums))
            {
                Json::Array hashes;
                bool all_supported = true;
                for (const auto& value : checksums->array(VCPKG_LINE_INFO))
                {
                    const auto& checksum = value.object(VCPKG_LINE_INFO);
                    const auto algorithm = checksum[JsonIdAlgorithm].string(VCPKG_LINE_INFO);
                    if (algorithm == "SHA256" || algorithm == "SHA512")
                    {
                        hashes.push_back(hash(Strings::ascii_to_lowercase(algorithm),
                                              checksum[SpdxChecksumValue].string(VCPKG_LINE_INFO)));
                        all_supported &= checksum.size() == 2;
                    }
                    else
                    {
                        all_supported = false;
                    }
                }
                if (hashes.size() != 0)
                {
                    (distribution.is_empty() ? package : distribution).insert("verifiedUsing", std::move(hashes));
                }
                if (all_supported) remaining.remove(JsonIdChecksums);
            }
            if (!distribution.is_empty()) graph.push_back(std::move(distribution));
            annotate(resource_id, std::move(remaining));
            graph.push_back(std::move(package));
        }
    };
}

std::string vcpkg::create_spdx3_sbom(const InstallPlanAction& action,
                                     View<Path> relative_paths,
                                     View<std::string> hashes,
                                     View<Path> relative_package_paths,
                                     View<std::string> package_hashes,
                                     StringView created_time,
                                     StringView document_namespace,
                                     const Json::Object& resource_doc)
{
    Checks::check_exit(VCPKG_LINE_INFO, relative_paths.size() == hashes.size());
    Checks::check_exit(VCPKG_LINE_INFO, relative_package_paths.size() == package_hashes.size());
    Spdx3Document doc{document_namespace.to_string(), {}};
    Json::Object creation;
    creation.insert("@id", "_:creation");
    creation.insert("type", "CreationInfo");
    creation.insert("specVersion", "3.0.1");
    creation.insert(JsonIdCreated, created_time);
    creation.insert("createdBy", Json::Array()).push_back(doc.id("SPDXRef-agent"));
    creation.insert("createdUsing", Json::Array()).push_back(doc.id("SPDXRef-tool"));
    doc.graph.push_back(std::move(creation));
    const auto tool_name = Strings::concat("vcpkg-", VCPKG_BASE_VERSION_AS_STRING, '-', VCPKG_VERSION_AS_STRING);
    for (const auto& entry :
         {std::pair<StringView, StringView>{"SoftwareAgent", "SPDXRef-agent"}, {"Tool", "SPDXRef-tool"}})
    {
        auto tool = doc.element(entry.first, doc.id(entry.second));
        tool.insert(JsonIdName, tool_name);
        doc.graph.push_back(std::move(tool));
    }

    const auto& scfl = action.source_control_file_and_location();
    const auto& cpgh = *scfl.source_control_file->core_paragraph;
    const auto abi = action.package_abi();
    const StringView abi_text = abi ? StringView{*abi} : StringView{SpdxNone};
    const auto port_id = doc.id(SpdxRefPort);
    const auto binary_id = doc.id(SpdxRefBinary);
    const auto license_id = doc.licenses(calculate_spdx_licenses(action));
    const auto license_expression = calculate_spdx_license(action);
    for (const auto& package_id : {port_id, binary_id})
    {
        auto package = doc.element("software_Package", package_id);
        package.insert("software_copyrightText", SpdxNoAssertion);
        if (package_id == port_id)
        {
            package.insert(JsonIdName, action.spec.name());
            package.insert("software_packageVersion", cpgh.version.to_string());
            doc.download_location(package,
                                  scfl.spdx_location.empty() ? StringView{SpdxNoAssertion} : scfl.spdx_location);
            if (!cpgh.homepage.empty()) package.insert("software_homePage", cpgh.homepage);
            if (!cpgh.summary.empty()) package.insert(JsonIdSummary, Strings::join("\n", cpgh.summary));
            if (!cpgh.description.empty()) package.insert(JsonIdDescription, Strings::join("\n", cpgh.description));
            package.insert(JsonIdComment, "This is the port (recipe) consumed by vcpkg.");
            package.insert("software_packageUrl", make_vcpkg_purl(action));
            if (!scfl.git_tree.empty())
            {
                Json::Object gitoid;
                gitoid.insert("type", "software_ContentIdentifier");
                gitoid.insert("software_contentIdentifierType", "gitoid");
                gitoid.insert("software_contentIdentifierValue", fmt::format("gitoid:tree:sha1:{}", scfl.git_tree));
                package.insert("software_contentIdentifier", Json::Array()).push_back(std::move(gitoid));
            }
        }
        else
        {
            package.insert(JsonIdName, action.spec.to_string());
            package.insert("software_packageVersion", abi_text);
            doc.download_location(package, SpdxNone);
            package.insert(JsonIdComment, "This is a binary package built by vcpkg.");
        }
        doc.license_relationship(package_id, "hasConcludedLicense", license_id, license_expression);
        doc.relationship(package_id, "hasDeclaredLicense", "expandedlicensing_NoAssertionLicense");
        doc.graph.push_back(std::move(package));
    }
    doc.relationship(port_id, "generates", binary_id);
    const auto process_files =
        [&](View<Path> paths, View<std::string> file_hashes, StringView kind, StringView package_id) {
            for (size_t i = 0; i < paths.size(); ++i)
            {
                const auto file_id = doc.id(fmt::format("SPDXRef-{}-file-{}", kind, i));
                auto file = doc.element("software_File", file_id);
                file.insert(JsonIdName, "./" + paths[i].generic_u8string());
                file.insert("verifiedUsing", Json::Array()).push_back(Spdx3Document::hash("sha256", file_hashes[i]));
                file.insert("software_copyrightText", SpdxNoAssertion);
                doc.graph.push_back(std::move(file));
                doc.relationship(package_id, "contains", file_id);
                doc.relationship(file_id, "hasConcludedLicense", "expandedlicensing_NoAssertionLicense");
                if (kind == "port" && paths[i] == FileVcpkgDotJson)
                {
                    doc.relationship(package_id, "hasDependencyManifest", file_id);
                }
            }
        };
    process_files(relative_paths, hashes, "port", port_id);
    process_files(relative_package_paths, package_hashes, "binary", binary_id);
    if (const auto resources = resource_doc.get(JsonIdPackages))
    {
        size_t index = 0;
        for (const auto& resource : resources->array(VCPKG_LINE_INFO))
        {
            doc.resource(resource.object(VCPKG_LINE_INFO), index++);
        }
    }

    auto document = doc.element("SpdxDocument", doc.id(SpdxRefDocument));
    document.insert(JsonIdName, fmt::format("{}@{} {}", action.spec, cpgh.version, abi_text));
    document.insert(JsonIdComment, "License information is taken from the port manifest, including selected features.");
    document.insert("dataLicense", doc.license(SpdxCCZero));
    document.insert("rootElement", Json::Array()).push_back(binary_id);
    auto& elements = document.insert("element", Json::Array());
    for (const auto& value : doc.graph)
    {
        if (const auto element_id = value.object(VCPKG_LINE_INFO).get("spdxId"))
        {
            elements.push_back(element_id->string(VCPKG_LINE_INFO));
        }
    }
    doc.graph.push_back(std::move(document));
    Json::Object result;
    result.insert("@context", "https://spdx.org/rdf/3.0.1/spdx-context.jsonld");
    result.insert("@graph", std::move(doc.graph));
    return Json::stringify(result);
}

Optional<std::string> vcpkg::read_spdx_license_text(StringView text, StringView origin)
{
    // JsonIdPackages[0]/SpdxLicenseConcluded
    auto maybe_parsed = Json::parse_object(text, origin);
    auto parsed = maybe_parsed.get();
    if (!parsed)
    {
        return nullopt;
    }

    auto maybe_packages_value = parsed->get(JsonIdPackages);
    if (!maybe_packages_value)
    {
        return nullopt;
    }

    auto maybe_packages_array = maybe_packages_value->maybe_array();
    if (!maybe_packages_array || maybe_packages_array->size() == 0)
    {
        return nullopt;
    }

    auto maybe_first_package_object = maybe_packages_array->operator[](0).maybe_object();
    if (!maybe_first_package_object)
    {
        return nullopt;
    }

    auto maybe_license_concluded_value = maybe_first_package_object->get(SpdxLicenseConcluded);
    if (!maybe_license_concluded_value)
    {
        return nullopt;
    }

    auto maybe_license_concluded = maybe_license_concluded_value->maybe_string();
    if (!maybe_license_concluded)
    {
        return nullopt;
    }

    if (maybe_license_concluded->empty() || *maybe_license_concluded == SpdxNoAssertion)
    {
        return nullopt;
    }

    return std::move(*maybe_license_concluded);
}
