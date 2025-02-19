/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef UTIL_H
#define UTIL_H

#include "globalconstants.h"
#include "config.h"
#include "globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <memory>
#include <rhash.h>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <json/json.h>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <curl/curl.h>
#include <tinyxml2.h>

typedef struct
{
    char *memory;
    curl_off_t size;
} ChunkMemoryStruct;

struct gameItem
{
    std::string name;
    std::string id;
    std::vector<std::string> dlcnames;
    Json::Value gamedetailsjson;
    int updates = 0;
    bool isnew;
};

struct wishlistItem
{
    std::string title;
    unsigned int platform;
    std::vector<std::string> tags;
    time_t release_date_time;
    std::string currency;
    std::string price;
    std::string discount_percent;
    std::string discount;
    std::string store_credit;
    std::string url;
    bool bIsBonusStoreCreditIncluded;
    bool bIsDiscounted;
};

namespace Util
{
    std::string getFileHash(const std::string& filename, unsigned hash_id);
    std::string getFileHashRange(const std::string& filepath, unsigned hash_id, off_t range_start = 0, off_t range_end = 0);
    std::string getChunkHash(unsigned char* chunk, uintmax_t chunk_size, unsigned hash_id);
    int createXML(std::string filepath, uintmax_t chunk_size, std::string xml_dir = std::string());
    int getGameSpecificConfig(std::string gamename, gameSpecificConfig* conf, std::string directory = std::string());
    int replaceString(std::string& str, const std::string& to_replace, const std::string& replace_with);
    int replaceAllString(std::string& str, const std::string& to_replace, const std::string& replace_with);
    void setFilePermissions(const boost::filesystem::path& path, const boost::filesystem::perms& permissions);
    int getTerminalWidth();
    void getManualUrlsFromJSON(const Json::Value &root, std::vector<std::string> &urls);
    std::vector<std::string> getDLCNamesFromJSON(const Json::Value &root);
    std::string getHomeDir();
    std::string getConfigHome();
    std::string getCacheHome();
    std::vector<std::string> tokenize(const std::string& str, const std::string& separator = ",");
    unsigned int getOptionValue(const std::string& str, const std::vector<GlobalConstants::optionsStruct>& options, const bool& bAllowStringToIntConversion = true);
    std::string getOptionNameString(const unsigned int& value, const std::vector<GlobalConstants::optionsStruct>& options);
    void parseOptionString(const std::string &option_string, std::vector<unsigned int> &priority, unsigned int &type, const std::vector<GlobalConstants::optionsStruct>& options);
    std::string getLocalFileHash(const std::string& xml_dir, const std::string& filepath, const std::string& gamename = std::string(), const bool& useFastCheck = true);
    void shortenStringToTerminalWidth(std::string& str);
    std::string getJsonUIntValueAsString(const Json::Value& json);
    std::string getStrippedString(std::string str);
    std::string makeEtaString(const unsigned long long& iBytesRemaining, const double& dlRate);
    std::string makeEtaString(const boost::posix_time::time_duration& duration);
    std::string CurlHandleGetInfoString(CURL* curlhandle, CURLINFO info);
    void CurlHandleSetDefaultOptions(CURL* curlhandle, const CurlConfig& conf);
    CURLcode CurlGetResponse(const std::string& url, std::string& response, int max_retries = -1);
    CURLcode CurlHandleGetResponse(CURL* curlhandle, std::string& response, int max_retries = -1);
    curl_off_t CurlWriteMemoryCallback(char *ptr, curl_off_t size, curl_off_t nmemb, void *userp);
    curl_off_t CurlWriteChunkMemoryCallback(void *contents, curl_off_t size, curl_off_t nmemb, void *userp);
    curl_off_t CurlReadChunkMemoryCallback(void *contents, curl_off_t size, curl_off_t nmemb, ChunkMemoryStruct *userp);
    std::string makeSizeString(const unsigned long long& iSizeInBytes);

    template<typename ... Args> std::string formattedString(const std::string& format, Args ... args)
    {
        std::size_t sz = std::snprintf(nullptr, 0, format.c_str(), args ...) + 1; // +1 for null terminator
        std::unique_ptr<char[]> buf(new char[sz]);
        std::snprintf(buf.get(), sz, format.c_str(), args ...);
        return std::string(buf.get(), buf.get() + sz - 1); // -1 because we don't want the null terminator
    }
    Json::Value readJsonFile(const std::string& path);
    std::string transformGamename(const std::string& gamename);
    std::string htmlToXhtml(const std::string& html);
    tinyxml2::XMLNode* nextXMLNode(tinyxml2::XMLNode* node);
}

#endif // UTIL_H
