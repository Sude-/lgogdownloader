/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "catalogmetadata.h"
#include "globalconstants.h"

#include <iostream>
#include <string>

static int failures = 0;

static void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        failures++;
    }
}

int main()
{
    using CatalogMetadata::ResolutionFailure;

    Json::Value empty;
    expect(CatalogMetadata::validateFileResolution(empty, "") == ResolutionFailure::EmptyResponse,
           "empty response is incomplete metadata");

    Json::Value missing(Json::objectValue);
    expect(CatalogMetadata::validateFileResolution(missing, "game/setup.exe") == ResolutionFailure::MissingDownlink,
           "missing downlink is incomplete metadata");
    Json::Value malformed(Json::arrayValue);
    expect(CatalogMetadata::validateFileResolution(malformed, "game/setup.exe") == ResolutionFailure::MalformedResponse,
           "non-object response is incomplete metadata");

    Json::Value response(Json::objectValue);
    response["downlink"] = "https://cdn.example/game/setup.exe";
    expect(CatalogMetadata::validateFileResolution(response, "") == ResolutionFailure::EmptyPath,
           "empty resolved path is incomplete metadata");
    expect(CatalogMetadata::validateFileResolution(response, "game/secure") == ResolutionFailure::InvalidPath,
           "secure placeholder is incomplete metadata");
    expect(CatalogMetadata::validateFileResolution(response, "game/SECUREX") == ResolutionFailure::InvalidPath,
           "securex placeholder is rejected case-insensitively");
    expect(CatalogMetadata::validateFileResolution(response, "game/setup.exe") == ResolutionFailure::None,
           "valid downlink and path are complete metadata");

    Json::Value product(Json::objectValue);
    product["downloads"] = Json::Value(Json::objectValue);
    product["downloads"]["installers"] = Json::Value(Json::arrayValue);
    product["downloads"]["bonus_content"] = Json::Value(Json::arrayValue);
    expect(CatalogMetadata::countMissingDownloadSections(
               product,
               GlobalConstants::GFTYPE_INSTALLER | GlobalConstants::GFTYPE_EXTRA
           ) == 0,
           "present empty download arrays are complete metadata");
    product["downloads"].removeMember("installers");
    expect(CatalogMetadata::countMissingDownloadSections(product, GlobalConstants::GFTYPE_INSTALLER) == 1,
           "missing selected download section is incomplete metadata");
    product.removeMember("downloads");
    expect(CatalogMetadata::countMissingDownloadSections(product, GlobalConstants::GFTYPE_INSTALLER) == 1,
           "missing downloads object is one metadata failure");
    Json::Value malformed_product(Json::arrayValue);
    expect(CatalogMetadata::countMissingDownloadSections(
               malformed_product,
               GlobalConstants::GFTYPE_INSTALLER
           ) == 1,
           "non-object product is incomplete metadata");
    expect(!CatalogMetadata::hasCompleteDlcExpansion(malformed_product),
           "non-object product cannot have complete DLC expansion");

    Json::Value no_dlc(Json::objectValue);
    expect(CatalogMetadata::hasCompleteDlcExpansion(no_dlc), "product without DLCs needs no expansion");
    Json::Value with_dlc(Json::objectValue);
    with_dlc["dlcs"] = Json::Value(Json::objectValue);
    expect(!CatalogMetadata::hasCompleteDlcExpansion(with_dlc), "missing DLC expansion is incomplete metadata");
    with_dlc["expanded_dlcs"] = Json::Value(Json::arrayValue);
    expect(CatalogMetadata::hasCompleteDlcExpansion(with_dlc), "empty expanded DLC array is complete metadata");

    Json::Value group(Json::objectValue);
    group["count"] = 1;
    group["total_size"] = 123;
    group["os"] = "windows";
    group["language"] = "en";
    group["files"] = Json::Value(Json::arrayValue);
    expect(!CatalogMetadata::hasCompleteDownloadGroup(group, true),
           "advertised group without files is incomplete metadata");
    group["files"].append(Json::Value(Json::objectValue));
    expect(CatalogMetadata::hasCompleteDownloadGroup(group, true),
           "advertised group with platform, language, and files is structurally complete");
    group.removeMember("language");
    expect(!CatalogMetadata::hasCompleteDownloadGroup(group, true),
           "installer group without language is incomplete metadata");
    expect(CatalogMetadata::hasCompleteDownloadGroup(group, false),
           "extra group does not require platform or language");
    group.removeMember("count");
    expect(CatalogMetadata::hasCompleteDownloadGroup(group, false),
           "group with files does not require an aggregate count");
    group["files"] = Json::Value(Json::arrayValue);
    expect(!CatalogMetadata::hasCompleteDownloadGroup(group, false),
           "non-empty aggregate without files is incomplete metadata");
    group["count"] = 0;
    group["total_size"] = 0;
    expect(CatalogMetadata::hasCompleteDownloadGroup(group, false),
           "all-zero group without files is a valid empty placeholder");
    expect(CatalogMetadata::isEmptyDownloadGroup(group), "all-zero group is an empty placeholder");
    group["files"].append(Json::Value(Json::objectValue));
    group["count"] = Json::Value();
    expect(CatalogMetadata::isEmptyDownloadGroup(group),
           "GOG placeholder with a null count and file-shaped stubs remains empty");
    group["total_size"] = 123;
    expect(!CatalogMetadata::isEmptyDownloadGroup(group),
           "non-zero group is never an empty placeholder");

    // GOG lists some groups for information only, with count and total_size zero but
    // real file entries (upstream commit 209d831). isEmptyDownloadGroup must classify
    // exactly the groups upstream skips: an absent or null aggregate reads as zero.
    Json::Value informational(Json::objectValue);
    informational["os"] = "windows";
    informational["language"] = "english";
    informational["total_size"] = 0;
    informational["files"] = Json::Value(Json::arrayValue);
    Json::Value real_file(Json::objectValue);
    real_file["id"] = "en1installer0";
    real_file["size"] = "734003200";
    real_file["downlink"] = "https://api.example/downlink/real";
    informational["files"].append(real_file);
    expect(CatalogMetadata::hasCompleteDownloadGroup(informational, true),
           "informational group with files and no count is complete metadata");
    expect(CatalogMetadata::isEmptyDownloadGroup(informational),
           "an installer group with no count is a placeholder by total_size alone");
    informational.removeMember("total_size");
    expect(CatalogMetadata::isEmptyDownloadGroup(informational),
           "an absent total_size reads as zero, the way upstream coerces it");
    informational["total_size"] = Json::Value();
    expect(CatalogMetadata::isEmptyDownloadGroup(informational),
           "a null total_size reads as zero, the way upstream coerces it");
    informational["total_size"] = 734003200;
    expect(!CatalogMetadata::isEmptyDownloadGroup(informational),
           "a group with a real total size is never a placeholder");

    Json::Value file(Json::objectValue);
    file["downlink"] = "https://api.example/downlink/1";
    file["size"] = "123";
    expect(CatalogMetadata::hasCompleteFileEntry(file), "file with downlink and size is complete metadata");
    file.removeMember("size");
    expect(!CatalogMetadata::hasCompleteFileEntry(file), "file without size is incomplete metadata");

    expect(CatalogMetadata::scalarString(Json::Value("123")) == "123",
           "string identifier remains a string");
    expect(CatalogMetadata::scalarString(Json::Value(123)) == "123",
           "numeric identifier is preserved for ownership matching");
    expect(CatalogMetadata::scalarString(Json::Value(Json::objectValue)).empty(),
           "structured identifier is rejected safely");

    if (failures == 0)
        std::cout << "All catalog-metadata tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
