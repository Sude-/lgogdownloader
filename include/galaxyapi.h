/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef GALAXYAPI_H
#define GALAXYAPI_H

#include "globalconstants.h"
#include "globals.h"
#include "config.h"
#include "util.h"
#include "gamedetails.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <curl/curl.h>
#include <sys/time.h>

struct galaxyDepotItemChunk
{
    std::string md5_compressed;
    std::string md5_uncompressed;
    uintmax_t size_compressed;
    uintmax_t size_uncompressed;
    uintmax_t offset_compressed;
    uintmax_t offset_uncompressed;
};

struct galaxyDepotItem
{
    std::string path;
    std::vector<galaxyDepotItemChunk> chunks;
    uintmax_t totalSizeCompressed;
    uintmax_t totalSizeUncompressed;
    std::string md5;
    std::string product_id;
};

class galaxyAPI
{
    public:
        galaxyAPI(CurlConfig& conf);
        virtual ~galaxyAPI();
        int init();
        bool isTokenExpired();
        bool refreshLogin();
        Json::Value getProductBuilds(const std::string& product_id, const std::string& platform = "windows", const std::string& generation = "2");
        Json::Value getManifestV1(const std::string& product_id, const std::string& build_id, const std::string& manifest_id = "repository", const std::string& platform = "windows");
        Json::Value getManifestV2(std::string manifest_hash);
        Json::Value getSecureLink(const std::string& product_id, const std::string& path);
        std::string getResponse(const std::string& url, const bool& zlib_decompress = false);
        std::string hashToGalaxyPath(const std::string& hash);
        std::vector<galaxyDepotItem> getDepotItemsVector(const std::string& hash);
        Json::Value getProductInfo(const std::string& product_id);
        gameDetails productInfoJsonToGameDetails(const Json::Value& json, const DownloadConfig& dlConf);
    protected:
    private:
        CurlConfig curlConf;
        static size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp);
        CURL* curlhandle;
        std::vector<gameFile> installerJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const DownloadConfig& dlConf);
        std::vector<gameFile> patchJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const DownloadConfig& dlConf);
        std::vector<gameFile> languagepackJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const DownloadConfig& dlConf);
        std::vector<gameFile> extraJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json);
        std::vector<gameFile> fileJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& type = GFTYPE_INSTALLER, const unsigned int& platform = (GlobalConstants::PLATFORM_WINDOWS | GlobalConstants::PLATFORM_LINUX), const unsigned int& lang = GlobalConstants::LANGUAGE_EN, const bool& useDuplicateHandler = false);
};

#endif // GALAXYAPI_H
