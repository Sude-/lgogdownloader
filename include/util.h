/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef UTIL_H
#define UTIL_H

#include "globalconstants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <rhash.h>
#include <boost/filesystem.hpp>
#include <json/json.h>

struct gameSpecificConfig
{
    unsigned int iInstallerType;
    unsigned int iInstallerLanguage;
    bool bDLC;
    bool bIgnoreDLCCount;
};

namespace Util
{
    std::string makeFilepath(const std::string& directory, const std::string& path, const std::string& gamename, std::string subdirectory = "", const unsigned int& platformId = 0, const std::string& dlcname = "");
    std::string makeRelativeFilepath(const std::string& path, const std::string& gamename, std::string subdirectory = "");
    std::string getFileHash(const std::string& filename, unsigned hash_id);
    std::string getChunkHash(unsigned char* chunk, uintmax_t chunk_size, unsigned hash_id);
    int createXML(std::string filepath, size_t chunk_size, std::string xml_dir = std::string());
    int getGameSpecificConfig(std::string gamename, gameSpecificConfig* conf, std::string directory = std::string());
    int replaceString(std::string& str, const std::string& to_replace, const std::string& replace_with);
    void filepathReplaceReservedStrings(std::string& str, const std::string& gamename, const unsigned int& platformId = 0, const std::string& dlcname = "");
    void setFilePermissions(const boost::filesystem::path& path, const boost::filesystem::perms& permissions);
    int getTerminalWidth();
    void getDownloaderUrlsFromJSON(const Json::Value &root, std::vector<std::string> &urls);
    std::vector<std::string> getDLCNamesFromJSON(const Json::Value &root);
}

#endif // UTIL_H
