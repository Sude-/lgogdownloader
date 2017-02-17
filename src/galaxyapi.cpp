/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "galaxyapi.h"

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

GalaxyConfig Globals::galaxyConf;

size_t galaxyAPI::writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) {
    std::ostringstream *stream = (std::ostringstream*)userp;
    std::streamsize count = (std::streamsize) size * nmemb;
    stream->write(ptr, count);
    return count;
}

galaxyAPI::galaxyAPI(CurlConfig& conf)
{
    this->curlConf = conf;

    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, curlConf.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEFILE, curlConf.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEJAR, curlConf.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, curlConf.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, curlConf.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, curlConf.iDownloadRate);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_TIME, 30);
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_LIMIT, 200);

    if (!curlConf.sCACertPath.empty())
        curl_easy_setopt(curlhandle, CURLOPT_CAINFO, curlConf.sCACertPath.c_str());
}

galaxyAPI::~galaxyAPI()
{
    curl_easy_cleanup(curlhandle);
}

/* Initialize the API
    returns 0 if failed
    returns 1 if successful
*/
int galaxyAPI::init()
{
    int res = 0;

    if (!this->isTokenExpired())
    {
        res = 1;
    }
    else
        res = 0;

    return res;
}

bool galaxyAPI::refreshLogin()
{
    bool res = false;
    std::string refresh_url = "https://auth.gog.com/token?client_id=" + Globals::galaxyConf.getClientId()
                            + "&client_secret=" + Globals::galaxyConf.getClientSecret()
                            + "&grant_type=refresh_token"
                            + "&refresh_token=" + Globals::galaxyConf.getRefreshToken();

    std::string json = this->getResponse(refresh_url);
    if (!json.empty())
    {
        Json::Value token_json;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, token_json))
        {
            Globals::galaxyConf.setJSON(token_json);
            res = true;
        }
        delete jsonparser;
    }

    return res;
}

bool galaxyAPI::isTokenExpired()
{
    int res = false;

    if (Globals::galaxyConf.isExpired())
        res = true;

    return res;
}

std::string galaxyAPI::getResponse(const std::string& url, const bool& zlib_decompress)
{
    std::ostringstream memory;

    struct curl_slist *header = NULL;

    std::string access_token;
    if (!Globals::galaxyConf.isExpired())
        access_token = Globals::galaxyConf.getAccessToken();
    if (!access_token.empty())
    {
        std::string bearer = "Authorization: Bearer " + access_token;
        header = curl_slist_append(header, bearer.c_str());
    }
    curl_easy_setopt(curlhandle, CURLOPT_HTTPHEADER, header);

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, galaxyAPI::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_perform(curlhandle);
    std::string response = memory.str();
    memory.str(std::string());

    curl_easy_setopt(curlhandle, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(header);

    if (zlib_decompress)
    {
        std::string response_decompressed;
        boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
        in.push(boost::iostreams::zlib_decompressor(GlobalConstants::ZLIB_WINDOW_SIZE));
        in.push(boost::make_iterator_range(response));
        boost::iostreams::copy(in, boost::iostreams::back_inserter(response_decompressed));
        response = response_decompressed;
    }

    return response;
}

Json::Value galaxyAPI::getProductBuilds(const std::string& product_id, const std::string& platform, const std::string& generation)
{
    Json::Value json;

    std::string url = "https://content-system.gog.com/products/" + product_id + "/os/" + platform + "/builds?generation=" + generation;
    std::string response = this->getResponse(url);

    Json::Reader *jsonparser = new Json::Reader;
    jsonparser->parse(response, json);
    delete jsonparser;

    return json;
}

Json::Value galaxyAPI::getManifestV1(const std::string& product_id, const std::string& build_id, const std::string& manifest_id, const std::string& platform)
{
    Json::Value json;

    std::string url = "https://cdn.gog.com/content-system/v1/manifests/" + product_id + "/" + platform + "/" + build_id + "/" + manifest_id + ".json";
    std::string response = this->getResponse(url);

    Json::Reader *jsonparser = new Json::Reader;
    jsonparser->parse(response, json);
    delete jsonparser;

    return json;
}

Json::Value galaxyAPI::getManifestV2(std::string manifest_hash)
{
    Json::Value json;

    if (!manifest_hash.empty() && manifest_hash.find("/") == std::string::npos)
        manifest_hash = this->hashToGalaxyPath(manifest_hash);

    std::string url = "https://cdn.gog.com/content-system/v2/meta/" + manifest_hash;
    std::string response = this->getResponse(url, true);

    Json::Reader *jsonparser = new Json::Reader;
    jsonparser->parse(response, json);
    delete jsonparser;

    return json;
}

Json::Value galaxyAPI::getSecureLink(const std::string& product_id, const std::string& path)
{
    Json::Value json;

    std::string url = "https://content-system.gog.com/products/" + product_id + "/secure_link?generation=2&path=" + path + "&_version=2";
    std::string response = this->getResponse(url);

    Json::Reader *jsonparser = new Json::Reader;
    jsonparser->parse(response, json);
    delete jsonparser;

    return json;
}

std::string galaxyAPI::hashToGalaxyPath(const std::string& hash)
{
    std::string galaxy_path = hash;
    if (galaxy_path.find("/") == std::string::npos)
        galaxy_path.assign(hash.begin(), hash.begin()+2).append("/").append(hash.begin()+2, hash.begin()+4).append("/").append(hash);

    return galaxy_path;
}

std::vector<galaxyDepotItem> galaxyAPI::getDepotItemsVector(const std::string& hash)
{
    Json::Value json = this->getManifestV2(hash);

    std::vector<galaxyDepotItem> items;

    for (unsigned int i = 0; i < json["depot"]["items"].size(); ++i)
    {
        if (!json["depot"]["items"][i]["chunks"].empty())
        {
            galaxyDepotItem item;
            item.totalSizeCompressed = 0;
            item.totalSizeUncompressed = 0;
            item.path = json["depot"]["items"][i]["path"].asString();

            while (Util::replaceString(item.path, "\\", "/"));
            for (unsigned int j = 0; j < json["depot"]["items"][i]["chunks"].size(); ++j)
            {
                galaxyDepotItemChunk chunk;
                chunk.md5_compressed = json["depot"]["items"][i]["chunks"][j]["compressedMd5"].asString();
                chunk.md5_uncompressed = json["depot"]["items"][i]["chunks"][j]["md5"].asString();
                chunk.size_compressed = json["depot"]["items"][i]["chunks"][j]["compressedSize"].asLargestUInt();
                chunk.size_uncompressed = json["depot"]["items"][i]["chunks"][j]["size"].asLargestUInt();

                item.totalSizeCompressed += chunk.size_compressed;
                item.totalSizeUncompressed += chunk.size_uncompressed;
                item.chunks.push_back(chunk);
            }

            if (json["depot"]["items"][i].isMember("md5"))
                item.md5 = json["depot"]["items"][i]["md5"].asString();
            else if (json["depot"]["items"][i]["chunks"].size() == 1)
                item.md5 = json["depot"]["items"][i]["chunks"][0]["md5"].asString();

            items.push_back(item);
        }
    }

    return items;
}
