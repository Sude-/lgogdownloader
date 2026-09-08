/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "catalogmetadata.h"
#include "globalconstants.h"

#include <algorithm>
#include <cctype>

CatalogMetadata::ResolutionFailure CatalogMetadata::validateFileResolution(
    const Json::Value& downlinkJson,
    const std::string& resolvedPath
)
{
    if (downlinkJson.isNull())
        return ResolutionFailure::EmptyResponse;
    if (!downlinkJson.isObject())
        return ResolutionFailure::MalformedResponse;
    if (!downlinkJson.isMember("downlink") || !downlinkJson["downlink"].isString()
        || downlinkJson["downlink"].asString().empty())
        return ResolutionFailure::MissingDownlink;
    if (resolvedPath.empty())
        return ResolutionFailure::EmptyPath;

    std::string lowercase_path = resolvedPath;
    std::transform(lowercase_path.begin(), lowercase_path.end(), lowercase_path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto ends_with = [&lowercase_path](const std::string& suffix) {
        return lowercase_path.size() >= suffix.size()
            && lowercase_path.compare(lowercase_path.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with("/secure") || ends_with("/securex"))
        return ResolutionFailure::InvalidPath;

    return ResolutionFailure::None;
}

unsigned int CatalogMetadata::countMissingDownloadSections(
    const Json::Value& productJson,
    const unsigned int& includeTypes
)
{
    if (!productJson.isObject() || !productJson.isMember("downloads")
        || !productJson["downloads"].isObject())
        return 1;

    const Json::Value& downloads = productJson["downloads"];
    unsigned int missing = 0;
    const auto require_array = [&downloads, &missing](const char* name) {
        if (!downloads.isMember(name) || !downloads[name].isArray())
            missing++;
    };

    if (includeTypes & GlobalConstants::GFTYPE_INSTALLER)
        require_array("installers");
    if (includeTypes & GlobalConstants::GFTYPE_EXTRA)
        require_array("bonus_content");
    if (includeTypes & GlobalConstants::GFTYPE_PATCH)
        require_array("patches");
    if (includeTypes & GlobalConstants::GFTYPE_LANGPACK)
        require_array("language_packs");

    return missing;
}

bool CatalogMetadata::hasCompleteDlcExpansion(const Json::Value& productJson)
{
    if (!productJson.isObject())
        return false;
    return !productJson["dlcs"].isObject() || productJson["expanded_dlcs"].isArray();
}

bool CatalogMetadata::hasCompleteDownloadGroup(const Json::Value& group, bool requirePlatformLanguage)
{
    if (!group.isObject())
        return false;

    const bool has_files = group.isMember("files") && group["files"].isArray()
        && !group["files"].empty();
    if (!has_files)
        return isEmptyDownloadGroup(group);

    if (requirePlatformLanguage
        && (!group.isMember("os") || !group["os"].isString() || group["os"].asString().empty()
            || !group.isMember("language") || !group["language"].isString()
            || group["language"].asString().empty()))
    {
        return false;
    }

    return true;
}

bool CatalogMetadata::isEmptyDownloadGroup(const Json::Value& group)
{
    if (!group.isObject())
        return false;

    // Upstream reads these with asUInt()/asLargestUInt(), which coerce an absent or
    // null value to zero. Match that, so this predicate classifies exactly the
    // groups upstream skips and no others.
    const auto readsAsZero = [&group](const char* name) {
        const Json::Value& value = group[name];
        return value.isNull() || (value.isNumeric() && value.asLargestUInt() == 0);
    };

    return readsAsZero("total_size") && readsAsZero("count");
}

bool CatalogMetadata::hasCompleteFileEntry(const Json::Value& file)
{
    if (!file.isObject() || !file.isMember("downlink") || !file["downlink"].isString()
        || file["downlink"].asString().empty() || !file.isMember("size"))
    {
        return false;
    }

    const Json::Value& size = file["size"];
    return size.isNumeric() || (size.isString() && !size.asString().empty());
}

std::string CatalogMetadata::scalarString(const Json::Value& value)
{
    return value.isString() || value.isNumeric() ? value.asString() : std::string();
}
