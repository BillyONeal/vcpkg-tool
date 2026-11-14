#include <vcpkg-test/util.h>

#include <vcpkg/dependencies.h>
#include <vcpkg/spdx.h>

using namespace vcpkg;

static const Json::Object& spdx3_element(const Json::Object& doc, StringView id)
{
    for (const auto& value : doc["@graph"].array(VCPKG_LINE_INFO))
    {
        const auto& object = value.object(VCPKG_LINE_INFO);
        const auto element_id = object.get("spdxId");
        if (element_id && element_id->string(VCPKG_LINE_INFO) == id) return object;
    }
    FAIL("Missing SPDX 3 element: " << id);
    Checks::unreachable(VCPKG_LINE_INFO);
}

static const Json::Object& spdx3_relationship(const Json::Object& doc, StringView from, StringView type)
{
    for (const auto& value : doc["@graph"].array(VCPKG_LINE_INFO))
    {
        const auto& object = value.object(VCPKG_LINE_INFO);
        if (object["type"].string(VCPKG_LINE_INFO) == "Relationship" &&
            object["from"].string(VCPKG_LINE_INFO) == from &&
            object["relationshipType"].string(VCPKG_LINE_INFO) == type)
        {
            return object;
        }
    }
    FAIL("Missing SPDX 3 relationship: " << from << " " << type);
    Checks::unreachable(VCPKG_LINE_INFO);
}

static Json::Object spdx3_annotations(const Json::Object& doc, StringView subject)
{
    Json::Object result;
    for (const auto& value : doc["@graph"].array(VCPKG_LINE_INFO))
    {
        const auto& object = value.object(VCPKG_LINE_INFO);
        if (object["type"].string(VCPKG_LINE_INFO) == "Annotation" &&
            object["subject"].string(VCPKG_LINE_INFO) == subject)
        {
            auto fields =
                Json::parse_object(object["statement"].string(VCPKG_LINE_INFO), "test").value(VCPKG_LINE_INFO);
            for (const auto& field : fields)
                result.insert(field.first, field.second);
        }
    }
    return result;
}

TEST_CASE ("spdx3 preserves package, file, and relationship information", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    SourceControlFileAndLocation scfl;
    scfl.source_control_file = Test::make_control_file("zlib", "");
    scfl.spdx_location = "git+https://example.org/registry@tree";
    scfl.spdx_repository_url = "https://example.org/registry";
    scfl.git_tree = std::string(40, 'a');
    auto& core = *scfl.source_control_file->core_paragraph;
    core.version = Version{"1.0", 5};
    core.summary = {"summary", "second line"};
    core.description = {"description", "second line"};
    core.homepage = "https://example.org/zlib";
    core.license = parse_spdx_license_expression_required("MIT OR Apache-2.0");
    InstallPlanAction action({"zlib", Test::ARM_UWP},
                             scfl,
                             packages_dir_assigner,
                             RequestType::USER_REQUESTED,
                             UseHeadVersion::No,
                             Editable::No,
                             {},
                             {},
                             {});
    action.abi_info = AbiInfo{};
    action.abi_info.get()->package_abi = "ABIHASH";
    const std::vector<Path> port_paths{"vcpkg.json", "patches/fix.diff"};
    const std::vector<Path> binary_paths{"include/zlib.h"};
    const std::vector<std::string> port_hashes{std::string(64, 'a'), std::string(64, 'b')};
    const std::vector<std::string> binary_hashes{std::string(64, 'c')};
    const auto old = Json::parse_object(create_spdx_sbom(action,
                                                         port_paths,
                                                         port_hashes,
                                                         binary_paths,
                                                         binary_hashes,
                                                         "2026-11-13T20:41:35Z",
                                                         "https://example.org/spdx2",
                                                         {}),
                                        "test")
                         .value(VCPKG_LINE_INFO);
    const auto doc = Json::parse_object(create_spdx3_sbom(action,
                                                          port_paths,
                                                          port_hashes,
                                                          binary_paths,
                                                          binary_hashes,
                                                          "2026-11-13T20:41:35Z",
                                                          "https://example.org/spdx3",
                                                          {}),
                                        "test")
                         .value(VCPKG_LINE_INFO);
    const auto& document = spdx3_element(doc, "https://example.org/spdx3#SPDXRef-DOCUMENT");
    CHECK(document["name"] == old["name"]);
    CHECK(
        spdx3_element(doc, document["dataLicense"].string(VCPKG_LINE_INFO))["simplelicensing_licenseExpression"].string(
            VCPKG_LINE_INFO) == "CC0-1.0");
    const auto& creation = doc["@graph"].array(VCPKG_LINE_INFO)[0].object(VCPKG_LINE_INFO);
    CHECK(creation["specVersion"].string(VCPKG_LINE_INFO) == "3.0.1");
    CHECK(creation["created"] == old["creationInfo"].object(VCPKG_LINE_INFO)["created"]);
    const auto& tool = spdx3_element(doc, creation["createdUsing"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO));
    CHECK(Strings::concat("Tool: ", tool["name"].string(VCPKG_LINE_INFO)) ==
          old["creationInfo"].object(VCPKG_LINE_INFO)["creators"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO));

    for (const auto& value : old["packages"].array(VCPKG_LINE_INFO))
    {
        const auto& package = value.object(VCPKG_LINE_INFO);
        const auto id = Strings::concat("https://example.org/spdx3#", package["SPDXID"].string(VCPKG_LINE_INFO));
        const auto& translated = spdx3_element(doc, id);
        for (const auto& mapping : {std::pair<StringView, StringView>{"name", "name"},
                                    {"versionInfo", "software_packageVersion"},
                                    {"homepage", "software_homePage"},
                                    {"summary", "summary"},
                                    {"description", "description"},
                                    {"comment", "comment"},
                                    {"copyrightText", "software_copyrightText"}})
        {
            if (const auto original = package.get(mapping.first))
            {
                CHECK(*original == translated[mapping.second]);
            }
        }
        const auto& license = spdx3_relationship(doc, id, "hasConcludedLicense");
        const auto& expression = spdx3_element(doc, license["to"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO));
        CHECK(expression["simplelicensing_licenseExpression"] == package["licenseConcluded"]);
        CHECK(spdx3_relationship(doc, id, "hasDeclaredLicense")["to"].array(VCPKG_LINE_INFO)[0].string(
                  VCPKG_LINE_INFO) == "expandedlicensing_NoAssertionLicense");
    }
    const auto& port = spdx3_element(doc, "https://example.org/spdx3#SPDXRef-port");
    CHECK(port["software_downloadLocation"].string(VCPKG_LINE_INFO) == scfl.spdx_location);
    CHECK(port["software_packageUrl"].string(VCPKG_LINE_INFO) == make_vcpkg_purl(action));
    CHECK(port["software_contentIdentifier"]
              .array(VCPKG_LINE_INFO)[0]
              .object(VCPKG_LINE_INFO)["software_contentIdentifierValue"]
              .string(VCPKG_LINE_INFO) == "gitoid:tree:sha1:" + scfl.git_tree);
    CHECK(spdx3_annotations(doc, "https://example.org/spdx3#SPDXRef-binary")["downloadLocation"].string(
              VCPKG_LINE_INFO) == "NONE");
    for (const auto& value : old["files"].array(VCPKG_LINE_INFO))
    {
        const auto& file = value.object(VCPKG_LINE_INFO);
        const auto id = Strings::concat("https://example.org/spdx3#", file["SPDXID"].string(VCPKG_LINE_INFO));
        const auto& translated = spdx3_element(doc, id);
        CHECK(translated["name"] == file["fileName"]);
        CHECK(translated["software_copyrightText"] == file["copyrightText"]);
        const auto& hash = translated["verifiedUsing"].array(VCPKG_LINE_INFO)[0].object(VCPKG_LINE_INFO);
        CHECK(hash["algorithm"].string(VCPKG_LINE_INFO) == "sha256");
        CHECK(hash["hashValue"] ==
              file["checksums"].array(VCPKG_LINE_INFO)[0].object(VCPKG_LINE_INFO)["checksumValue"]);
        CHECK(spdx3_relationship(doc, id, "hasConcludedLicense")["to"].array(VCPKG_LINE_INFO)[0].string(
                  VCPKG_LINE_INFO) == "expandedlicensing_NoAssertionLicense");
    }
    for (const auto& value : old["relationships"].array(VCPKG_LINE_INFO))
    {
        const auto& relationship = value.object(VCPKG_LINE_INFO);
        auto from =
            Strings::concat("https://example.org/spdx3#", relationship["spdxElementId"].string(VCPKG_LINE_INFO));
        auto to =
            Strings::concat("https://example.org/spdx3#", relationship["relatedSpdxElement"].string(VCPKG_LINE_INFO));
        const auto type = relationship["relationshipType"].string(VCPKG_LINE_INFO);
        StringView translated_type = "contains";
        if (type == "GENERATES") translated_type = "generates";
        if (type == "DEPENDENCY_MANIFEST_OF")
        {
            translated_type = "hasDependencyManifest";
            std::swap(from, to);
        }
        bool found = false;
        for (const auto& entry : doc["@graph"].array(VCPKG_LINE_INFO))
        {
            const auto& object = entry.object(VCPKG_LINE_INFO);
            if (object["type"].string(VCPKG_LINE_INFO) == "Relationship" &&
                object["from"].string(VCPKG_LINE_INFO) == from &&
                object["relationshipType"].string(VCPKG_LINE_INFO) == translated_type &&
                object["to"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO) == to)
                found = true;
        }
        CHECK(found);
    }
}

TEST_CASE ("spdx3 preserves unknown licenses and selected feature licenses", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    SourceControlFileAndLocation scfl;
    scfl.source_control_file = Test::make_control_file("zlib", "", {{"selected", ""}});
    auto& core = *scfl.source_control_file->core_paragraph;
    InstallPlanAction action({"zlib", Test::ARM_UWP},
                             scfl,
                             packages_dir_assigner,
                             RequestType::USER_REQUESTED,
                             UseHeadVersion::No,
                             Editable::No,
                             {},
                             {},
                             {});
    bool has_known_license = false;
    bool has_unknown_license = true;
    std::vector<std::string> known_licenses{"MIT"};
    bool preserves_expression = false;
    SECTION ("undeclared")
    {
    }
    SECTION ("explicit null")
    {
        core.license = ParsedSpdxLicenseDeclaration{NullTag{}};
        preserves_expression = true;
    }
    SECTION ("custom license without available text")
    {
        core.license = parse_spdx_license_expression_required("LicenseRef-custom");
        preserves_expression = true;
    }
    SECTION ("custom license combined with a known license")
    {
        core.license = parse_spdx_license_expression_required("MIT AND LicenseRef-custom");
        has_known_license = true;
        preserves_expression = true;
    }
    SECTION ("choice of custom or known license")
    {
        core.license = parse_spdx_license_expression_required("MIT OR LicenseRef-custom");
        preserves_expression = true;
    }
    SECTION ("selected feature adds known license with exception")
    {
        core.license = parse_spdx_license_expression_required("MIT");
        scfl.source_control_file->feature_paragraphs[0]->license =
            parse_spdx_license_expression_required("GPL-2.0-only WITH Classpath-exception-2.0");
        action.feature_list = {"core", "selected"};
        has_known_license = true;
        has_unknown_license = false;
        known_licenses.insert(known_licenses.begin(), "GPL-2.0-only WITH Classpath-exception-2.0");
    }
    SECTION ("selected feature adds unknown license")
    {
        core.license = parse_spdx_license_expression_required("MIT");
        scfl.source_control_file->feature_paragraphs[0]->license = ParsedSpdxLicenseDeclaration{NullTag{}};
        action.feature_list = {"core", "selected"};
        has_known_license = true;
        preserves_expression = true;
    }
    const auto doc =
        Json::parse_object(
            create_spdx3_sbom(action, {}, {}, {}, {}, "2026-11-13T20:41:35Z", "https://example.org/spdx3", {}), "test")
            .value(VCPKG_LINE_INFO);
    const auto& port = spdx3_element(doc, "https://example.org/spdx3#SPDXRef-port");
    CHECK_FALSE(port.contains("software_homePage"));
    CHECK_FALSE(port.contains("description"));
    CHECK(spdx3_annotations(doc, "https://example.org/spdx3#SPDXRef-port")["downloadLocation"].string(
              VCPKG_LINE_INFO) == "NOASSERTION");
    const auto& relationship =
        spdx3_relationship(doc, "https://example.org/spdx3#SPDXRef-binary", "hasConcludedLicense");
    const auto target = relationship["to"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO);
    if (has_known_license)
    {
        const auto& conjunction = spdx3_element(doc, target);
        CHECK(conjunction["type"].string(VCPKG_LINE_INFO) == "expandedlicensing_ConjunctiveLicenseSet");
        const auto& members = conjunction["expandedlicensing_member"].array(VCPKG_LINE_INFO);
        REQUIRE(members.size() == 2);
        std::vector<std::string> observed_licenses;
        bool observed_unknown_license = false;
        for (const auto& member : members)
        {
            const auto id = member.string(VCPKG_LINE_INFO);
            if (id != "expandedlicensing_NoAssertionLicense")
            {
                observed_licenses.push_back(
                    spdx3_element(doc, id)["simplelicensing_licenseExpression"].string(VCPKG_LINE_INFO).to_string());
            }
            else
            {
                observed_unknown_license = true;
            }
        }
        std::sort(observed_licenses.begin(), observed_licenses.end());
        CHECK(observed_licenses == known_licenses);
        CHECK(observed_unknown_license == has_unknown_license);
    }
    else
    {
        CHECK(target == "expandedlicensing_NoAssertionLicense");
    }
    const auto annotations = spdx3_annotations(doc, relationship["spdxId"].string(VCPKG_LINE_INFO));
    if (preserves_expression)
    {
        CHECK(annotations["licenseExpression"].string(VCPKG_LINE_INFO) == calculate_spdx_license(action));
    }
    else
    {
        CHECK(annotations.is_empty());
    }
}

TEST_CASE ("spdx3 preserves downloaded resources and additional resource metadata", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    SourceControlFileAndLocation scfl;
    scfl.source_control_file = Test::make_control_file("zlib", "");
    InstallPlanAction action({"zlib", Test::ARM_UWP},
                             scfl,
                             packages_dir_assigner,
                             RequestType::USER_REQUESTED,
                             UseHeadVersion::No,
                             Editable::No,
                             {},
                             {},
                             {});
    auto resources = Json::parse_object(R"json({"packages":[{
        "SPDXID":"SPDXRef-resource-0", "name":"source archive", "downloadLocation":"https://example.org/source.tar.gz",
        "packageFileName":"source.tar.gz", "licenseConcluded":"NOASSERTION", "licenseDeclared":"NOASSERTION",
        "copyrightText":"NOASSERTION", "checksums":[{"algorithm":"SHA512","checksumValue":"abcd"}],
        "extra":{"nested":["must survive",42]}
    }]})json",
                                        "test")
                         .value(VCPKG_LINE_INFO);
    auto& resource = resources["packages"].array(VCPKG_LINE_INFO)[0].object(VCPKG_LINE_INFO);
    bool has_filename = true;
    bool has_hash = true;
    SECTION ("archive with hash")
    {
    }
    SECTION ("archive without hash")
    {
        resource.remove("checksums");
        has_hash = false;
    }
    SECTION ("hash without archive filename")
    {
        resource.remove("packageFileName");
        has_filename = false;
    }
    const auto doc =
        Json::parse_object(
            create_spdx3_sbom(action, {}, {}, {}, {}, "2026-11-13T20:41:35Z", "https://example.org/spdx3", resources),
            "test")
            .value(VCPKG_LINE_INFO);
    const auto& translated = spdx3_element(doc, "https://example.org/spdx3#SPDXRef-resource-0");
    CHECK(translated["name"] == resource["name"]);
    CHECK(translated["software_downloadLocation"] == resource["downloadLocation"]);
    CHECK(translated["software_copyrightText"] == resource["copyrightText"]);
    CHECK(spdx3_annotations(doc, "https://example.org/spdx3#SPDXRef-resource-0")["extra"] == resource["extra"]);
    const Json::Object* hash_owner = &translated;
    if (has_filename)
    {
        const auto& relationship =
            spdx3_relationship(doc, "https://example.org/spdx3#SPDXRef-resource-0", "hasDistributionArtifact");
        hash_owner = &spdx3_element(doc, relationship["to"].array(VCPKG_LINE_INFO)[0].string(VCPKG_LINE_INFO));
        CHECK((*hash_owner)["name"] == resource["packageFileName"]);
    }
    if (has_hash)
    {
        const auto& hash = (*hash_owner)["verifiedUsing"].array(VCPKG_LINE_INFO)[0].object(VCPKG_LINE_INFO);
        CHECK(hash["algorithm"].string(VCPKG_LINE_INFO) == "sha512");
        CHECK(hash["hashValue"].string(VCPKG_LINE_INFO) == "abcd");
    }
    else
    {
        CHECK_FALSE(hash_owner->contains("verifiedUsing"));
    }
}

TEST_CASE ("spdx maximum serialization", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    scfl.spdx_location = "git://some-vcs-url";
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.summary = {"summary"};
    cpgh.description = {"description"};
    cpgh.homepage = "homepage";
    cpgh.license = parse_spdx_license_expression_required("MIT");
    cpgh.version_scheme = VersionScheme::Relaxed;
    cpgh.version = Version{"1.0", 5};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom =
        create_spdx_sbom(ipa,
                         std::vector<Path>{"vcpkg.json", "portfile.cmake", "patches/patch1.diff"},
                         std::vector<std::string>{"vcpkg.json-hash", "portfile.cmake-hash", "patch1.diff-hash"},
                         std::vector<Path>{"include/zlib.h", "lib/zlib.lib"},
                         std::vector<std::string>{"zlib-header-hash", "zlib-lib-hash"},
                         "now",
                         "https://test-document-namespace",
                         {});

    static constexpr StringLiteral expected_text = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.3/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace",
  "name": "zlib:arm-uwp@1.0#5 ABIHASH",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-port-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-port-file-1"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-port-file-2"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-binary-file-0"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-binary-file-1"
    }
  ],
  "packages": [
    {
      "name": "zlib",
      "SPDXID": "SPDXRef-port",
      "versionInfo": "1.0#5",
      "downloadLocation": "git://some-vcs-url",
      "homepage": "homepage",
      "licenseConcluded": "MIT",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "summary": "summary",
      "description": "description",
      "comment": "This is the port (recipe) consumed by vcpkg.",
      "externalRefs": [
        {
          "referenceCategory": "PACKAGE-MANAGER",
          "referenceType": "purl",
          "referenceLocator": "pkg:vcpkg/zlib@1.0?port_version=5&triplet=arm-uwp&vcs_url=git%3A%2F%2Fsome-vcs-url"
        }
      ]
    },
    {
      "name": "zlib:arm-uwp",
      "SPDXID": "SPDXRef-binary",
      "versionInfo": "ABIHASH",
      "downloadLocation": "NONE",
      "licenseConcluded": "MIT",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "This is a binary package built by vcpkg."
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "vcpkg.json-hash"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "portfile.cmake-hash"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./patches/patch1.diff",
      "SPDXID": "SPDXRef-port-file-2",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "patch1.diff-hash"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./include/zlib.h",
      "SPDXID": "SPDXRef-binary-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "zlib-header-hash"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./lib/zlib.lib",
      "SPDXID": "SPDXRef-binary-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "zlib-lib-hash"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    auto expected = Json::parse(expected_text, "test").value(VCPKG_LINE_INFO);
    auto doc = Json::parse(sbom, "test").value(VCPKG_LINE_INFO);
    Test::check_json_eq(expected.value, doc.value);

    CHECK(read_spdx_license_text(expected_text, "test") == "MIT");
}

TEST_CASE ("spdx minimum serialization", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version_scheme = VersionScheme::String;
    cpgh.version = Version{"1.0", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "deadbeef";

    const auto sbom = create_spdx_sbom(ipa,
                                       std::vector<Path>{"vcpkg.json", "portfile.cmake"},
                                       std::vector<std::string>{"hash-vcpkg.json", "hash-portfile.cmake"},
                                       {},
                                       {},
                                       "now+1",
                                       "https://test-document-namespace-2",
                                       {});

    static constexpr StringLiteral expected_text = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.3/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-port-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-port-file-1"
    }
  ],
  "packages": [
    {
      "name": "zlib",
      "SPDXID": "SPDXRef-port",
      "versionInfo": "1.0",
      "downloadLocation": "NOASSERTION",
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "This is the port (recipe) consumed by vcpkg.",
      "externalRefs": [
        {
          "referenceCategory": "PACKAGE-MANAGER",
          "referenceType": "purl",
          "referenceLocator": "pkg:vcpkg/zlib@1.0?triplet=arm-uwp"
        }
      ]
    },
    {
      "name": "zlib:arm-uwp",
      "SPDXID": "SPDXRef-binary",
      "versionInfo": "deadbeef",
      "downloadLocation": "NONE",
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "This is a binary package built by vcpkg."
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    auto expected = Json::parse(expected_text, "test").value(VCPKG_LINE_INFO);
    auto doc = Json::parse(sbom, "test").value(VCPKG_LINE_INFO);
    Test::check_json_eq(expected.value, doc.value);
    CHECK(!read_spdx_license_text(expected_text, "test").has_value());
}

static Optional<std::string> port_external_ref(StringView sbom, StringView reference_type)
{
    auto parsed = Json::parse(sbom, "test").value(VCPKG_LINE_INFO);
    const auto external_refs = parsed.value.object(VCPKG_LINE_INFO)["packages"]
                                   .array(VCPKG_LINE_INFO)[0]
                                   .object(VCPKG_LINE_INFO)["externalRefs"]
                                   .array(VCPKG_LINE_INFO);
    for (const auto& external_ref_value : external_refs)
    {
        const auto external_ref = external_ref_value.object(VCPKG_LINE_INFO);
        if (external_ref["referenceType"].string(VCPKG_LINE_INFO) == reference_type)
        {
            return external_ref["referenceLocator"].string(VCPKG_LINE_INFO).to_string();
        }
    }

    return nullopt;
}

static std::string port_purl(StringView sbom) { return port_external_ref(sbom, "purl").value_or_exit(VCPKG_LINE_INFO); }

static std::string port_download_location(StringView sbom)
{
    auto parsed = Json::parse(sbom, "test").value(VCPKG_LINE_INFO);
    return parsed.value.object(VCPKG_LINE_INFO)["packages"]
        .array(VCPKG_LINE_INFO)[0]
        .object(VCPKG_LINE_INFO)["downloadLocation"]
        .string(VCPKG_LINE_INFO)
        .to_string();
}

TEST_CASE ("spdx purl omits empty version", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"", 1};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib?triplet=arm-uwp");
}

TEST_CASE ("spdx purl percent-encodes the version", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"1.0+r2", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib@1.0%2Br2?triplet=arm-uwp");
}

TEST_CASE ("spdx purl includes vcs_url for a builtin git-tree", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    scfl.kind = PortSourceKind::Builtin;
    scfl.spdx_location = "git+https://github.com/Microsoft/vcpkg@84a143e4caf6b70db57f28d04c41df4a85c480fa";
    scfl.git_tree = "84a143e4caf6b70db57f28d04c41df4a85c480fa";
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"1.0", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    // The default microsoft/vcpkg registry is Builtin, so the locator has no repository_url.
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib@1.0?triplet=arm-uwp"
                             "&vcs_url=git%2Bhttps%3A%2F%2Fgithub.com%2FMicrosoft%2Fvcpkg%40"
                             "84a143e4caf6b70db57f28d04c41df4a85c480fa");
    CHECK(port_download_location(sbom) ==
          "git+https://github.com/Microsoft/vcpkg@84a143e4caf6b70db57f28d04c41df4a85c480fa");
    CHECK(port_external_ref(sbom, "gitoid").value_or_exit(VCPKG_LINE_INFO) ==
          "gitoid:tree:sha1:84a143e4caf6b70db57f28d04c41df4a85c480fa");
}

TEST_CASE ("spdx purl includes repository_url and vcs_url for a non-default git registry", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    scfl.kind = PortSourceKind::Git;
    scfl.spdx_location = "git+https://github.com/azure-sdk/vcpkg@84a143e4caf6b70db57f28d04c41df4a85c480fa";
    scfl.spdx_repository_url = "https://github.com/azure-sdk/vcpkg";
    scfl.git_tree = "84a143e4caf6b70db57f28d04c41df4a85c480fa";
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"1.0", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    // repository_url is the registry URL with the git+ prefix and @git-tree stripped; vcs_url keeps both.
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib@1.0?repository_url=https%3A%2F%2Fgithub.com%2Fazure-sdk%2Fvcpkg"
                             "&triplet=arm-uwp"
                             "&vcs_url=git%2Bhttps%3A%2F%2Fgithub.com%2Fazure-sdk%2Fvcpkg%40"
                             "84a143e4caf6b70db57f28d04c41df4a85c480fa");
    CHECK(port_download_location(sbom) ==
          "git+https://github.com/azure-sdk/vcpkg@84a143e4caf6b70db57f28d04c41df4a85c480fa");
    CHECK(port_external_ref(sbom, "gitoid").value_or_exit(VCPKG_LINE_INFO) ==
          "gitoid:tree:sha1:84a143e4caf6b70db57f28d04c41df4a85c480fa");
}

TEST_CASE ("spdx purl uses the dedicated repository_url field for git registries", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    scfl.kind = PortSourceKind::Git;
    scfl.spdx_location = "git+https://github.com/azure-sdk/vcpkg";
    scfl.spdx_repository_url = "https://example.com/registry";
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"1.0", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib@1.0?repository_url=https%3A%2F%2Fexample.com%2Fregistry"
                             "&triplet=arm-uwp"
                             "&vcs_url=git%2Bhttps%3A%2F%2Fgithub.com%2Fazure-sdk%2Fvcpkg");
}

TEST_CASE ("spdx purl omits vcs_url and repository_url for overlay ports", "[spdx]")
{
    PackagesDirAssigner packages_dir_assigner{"test_packages_root"};
    PackageSpec spec{"zlib", Test::ARM_UWP};
    SourceControlFileAndLocation scfl;
    scfl.kind = PortSourceKind::Overlay;
    auto& scf = *(scfl.source_control_file = std::make_unique<SourceControlFile>());
    auto& cpgh = *(scf.core_paragraph = std::make_unique<SourceParagraph>());
    cpgh.name = "zlib";
    cpgh.version = Version{"1.0", 0};

    InstallPlanAction ipa(
        spec, scfl, packages_dir_assigner, RequestType::USER_REQUESTED, UseHeadVersion::No, Editable::No, {}, {}, {});
    auto& abi = *(ipa.abi_info = AbiInfo{}).get();
    abi.package_abi = "ABIHASH";

    const auto sbom = create_spdx_sbom(ipa, {}, {}, {}, {}, "now", "https://test-document-namespace", {});
    CHECK(port_purl(sbom) == "pkg:vcpkg/zlib@1.0?triplet=arm-uwp");
    CHECK(!port_external_ref(sbom, "gitoid").has_value());
}

TEST_CASE ("spdx license parse edge cases", "[spdx]")
{
    CHECK(!read_spdx_license_text("this is not json", "test").has_value());

    static constexpr StringLiteral missing_packages = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(missing_packages, "test").has_value());

    static constexpr StringLiteral empty_packages = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": [],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(empty_packages, "test").has_value());

    static constexpr StringLiteral wrong_packages_type = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": {},
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(wrong_packages_type, "test").has_value());

    static constexpr StringLiteral wrong_packages_zero_type = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": [42],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(wrong_packages_zero_type, "test").has_value());

    static constexpr StringLiteral missing_license_block = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": [
    {
      "name": "zlib",
      "SPDXID": "SPDXRef-port",
      "versionInfo": "1.0",
      "downloadLocation": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "This is the port (recipe) consumed by vcpkg."
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(missing_license_block, "test").has_value());

    static constexpr StringLiteral wrong_license_type = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": [
    {
      "name": "zlib",
      "SPDXID": "SPDXRef-port",
      "versionInfo": "1.0",
      "downloadLocation": "NOASSERTION",
      "licenseConcluded": 42,
      "licenseDeclared": 42,
      "copyrightText": "NOASSERTION",
      "comment": "This is the port (recipe) consumed by vcpkg."
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(wrong_license_type, "test").has_value());

    static constexpr StringLiteral empty_license = R"json(
{
  "$schema": "https://raw.githubusercontent.com/spdx/spdx-spec/v2.2.1/schemas/spdx-schema.json",
  "spdxVersion": "SPDX-2.2",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "documentNamespace": "https://test-document-namespace-2",
  "name": "zlib:arm-uwp@1.0 deadbeef",
  "creationInfo": {
    "creators": [
      "Tool: vcpkg-2999-12-31-unknownhash"
    ],
    "created": "now+1"
  },
  "relationships": [
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "GENERATES",
      "relatedSpdxElement": "SPDXRef-binary"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-0"
    },
    {
      "spdxElementId": "SPDXRef-port",
      "relationshipType": "CONTAINS",
      "relatedSpdxElement": "SPDXRef-file-1"
    },
    {
      "spdxElementId": "SPDXRef-binary",
      "relationshipType": "GENERATED_FROM",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-0",
      "relationshipType": "DEPENDENCY_MANIFEST_OF",
      "relatedSpdxElement": "SPDXRef-port"
    },
    {
      "spdxElementId": "SPDXRef-file-1",
      "relationshipType": "CONTAINED_BY",
      "relatedSpdxElement": "SPDXRef-port"
    }
  ],
  "packages": [
    {
      "name": "zlib",
      "SPDXID": "SPDXRef-port",
      "versionInfo": "1.0",
      "downloadLocation": "NOASSERTION",
      "licenseConcluded": "",
      "licenseDeclared": "",
      "copyrightText": "NOASSERTION",
      "comment": "This is the port (recipe) consumed by vcpkg."
    },
    {
      "name": "zlib:arm-uwp",
      "SPDXID": "SPDXRef-binary",
      "versionInfo": "deadbeef",
      "downloadLocation": "NONE",
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "This is a binary package built by vcpkg."
    }
  ],
  "files": [
    {
      "fileName": "./vcpkg.json",
      "SPDXID": "SPDXRef-port-file-0",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-vcpkg.json"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    },
    {
      "fileName": "./portfile.cmake",
      "SPDXID": "SPDXRef-port-file-1",
      "checksums": [
        {
          "algorithm": "SHA256",
          "checksumValue": "hash-portfile.cmake"
        }
      ],
      "licenseConcluded": "NOASSERTION",
      "copyrightText": "NOASSERTION"
    }
  ]
})json";

    CHECK(!read_spdx_license_text(empty_license, "test").has_value());
}
