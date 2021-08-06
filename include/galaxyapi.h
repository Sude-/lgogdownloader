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
    bool isDependency = false;
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
        Json::Value getManifestV1(const std::string& manifest_url);
        Json::Value getManifestV2(std::string manifest_hash, const bool& is_dependency = false);
        Json::Value getSecureLink(const std::string& product_id, const std::string& path);
        Json::Value getDependencyLink(const std::string& path);
        std::string getResponse(const std::string& url, const bool& zlib_decompress = false);
        Json::Value getResponseJson(const std::string& url, const bool& zlib_decompress = false);
        std::string hashToGalaxyPath(const std::string& hash);
        std::vector<galaxyDepotItem> getDepotItemsVector(const std::string& hash, const bool& is_dependency = false);
        Json::Value getProductInfo(const std::string& product_id);
        gameDetails productInfoJsonToGameDetails(const Json::Value& json, const DownloadConfig& dlConf);
        Json::Value getUserData();
        Json::Value getDependenciesJson();
        std::vector<galaxyDepotItem> getFilteredDepotItemsVectorFromJson(const Json::Value& depot_json, const std::string& galaxy_language, const std::string& galaxy_arch, const bool& is_dependency = false);
        std::string getPathFromDownlinkUrl(const std::string& downlink_url, const std::string& gamename);
    protected:
    private:
        CurlConfig curlConf;
        static size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp);
        CURL* curlhandle;
        std::vector<gameFile> fileJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& type, const DownloadConfig& dlConf);
};

#endif // GALAXYAPI_H
