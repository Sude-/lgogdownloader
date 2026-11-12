/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef CATALOGMETADATA_H
#define CATALOGMETADATA_H

#include <string>

#include <json/json.h>

namespace CatalogMetadata
{
    enum class ResolutionFailure
    {
        None,
        EmptyResponse,
        MalformedResponse,
        MissingDownlink,
        EmptyPath,
        InvalidPath
    };

    ResolutionFailure validateFileResolution(
        const Json::Value& downlinkJson,
        const std::string& resolvedPath
    );
    unsigned int countMissingDownloadSections(
        const Json::Value& productJson,
        const unsigned int& includeTypes
    );
    bool hasCompleteDlcExpansion(const Json::Value& productJson);
    bool hasCompleteDownloadGroup(const Json::Value& group, bool requirePlatformLanguage);
    bool isEmptyDownloadGroup(const Json::Value& group);
    bool hasCompleteFileEntry(const Json::Value& file);
    std::string scalarString(const Json::Value& value);
}

#endif // CATALOGMETADATA_H
