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
    bool res = false;

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

                chunk.offset_compressed = item.totalSizeCompressed;
                chunk.offset_uncompressed = item.totalSizeUncompressed;

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

Json::Value galaxyAPI::getProductInfo(const std::string& product_id)
{
    Json::Value json;

    std::string url = "https://api.gog.com/products/" + product_id + "?expand=downloads,expanded_dlcs,description,screenshots,videos,related_products,changelog";
    std::string response = this->getResponse(url);

    Json::Reader *jsonparser = new Json::Reader;
    jsonparser->parse(response, json);
    delete jsonparser;

    return json;
}

gameDetails galaxyAPI::productInfoJsonToGameDetails(const Json::Value& json, const DownloadConfig& dlConf)
{
    gameDetails gamedetails;

    gamedetails.gamename = json["slug"].asString();
    gamedetails.product_id = json["id"].asString();
    gamedetails.title = json["title"].asString();
    gamedetails.icon = "https:" + json["images"]["icon"].asString();

    if (json.isMember("changelog"))
        gamedetails.changelog = json["changelog"].asString();

    if (dlConf.bInstallers)
    {
        gamedetails.installers = this->installerJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["installers"], dlConf.iInstallerPlatform, dlConf.iInstallerLanguage, dlConf.bDuplicateHandler);
        for (unsigned int i = 0; i < gamedetails.installers.size(); ++i)
            gamedetails.installers[i].type |= GFTYPE_INSTALLER;
    }

    if (dlConf.bExtras)
    {
        gamedetails.extras = this->extraJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["bonus_content"]);
        for (unsigned int i = 0; i < gamedetails.extras.size(); ++i)
            gamedetails.extras[i].type |= GFTYPE_EXTRA;
    }

    if (dlConf.bPatches)
    {
        gamedetails.patches = this->patchJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["patches"], dlConf.iInstallerPlatform, dlConf.iInstallerLanguage, dlConf.bDuplicateHandler);
        for (unsigned int i = 0; i < gamedetails.patches.size(); ++i)
            gamedetails.patches[i].type |= GFTYPE_PATCH;
    }

    if (dlConf.bLanguagePacks)
    {
        gamedetails.languagepacks = this->languagepackJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["language_packs"], dlConf.iInstallerPlatform, dlConf.iInstallerLanguage, dlConf.bDuplicateHandler);
        for (unsigned int i = 0; i < gamedetails.languagepacks.size(); ++i)
            gamedetails.languagepacks[i].type |= GFTYPE_LANGPACK;
    }

    if (dlConf.bDLC)
    {
        if (json.isMember("expanded_dlcs"))
        {
            for (unsigned int i = 0; i < json["expanded_dlcs"].size(); ++i)
            {
                gameDetails dlc_gamedetails = this->productInfoJsonToGameDetails(json["expanded_dlcs"][i], dlConf);

                // Add DLC type to all DLC files
                for (unsigned int j = 0; j < dlc_gamedetails.installers.size(); ++j)
                    dlc_gamedetails.installers[j].type |= GFTYPE_DLC;
                for (unsigned int j = 0; j < dlc_gamedetails.extras.size(); ++j)
                    dlc_gamedetails.extras[j].type |= GFTYPE_DLC;
                for (unsigned int j = 0; j < dlc_gamedetails.patches.size(); ++j)
                    dlc_gamedetails.patches[j].type |= GFTYPE_DLC;
                for (unsigned int j = 0; j < dlc_gamedetails.languagepacks.size(); ++j)
                    dlc_gamedetails.languagepacks[j].type |= GFTYPE_DLC;

                // Add DLC only if it has any files
                if (!dlc_gamedetails.installers.empty() || !dlc_gamedetails.extras.empty() || !dlc_gamedetails.patches.empty() || !dlc_gamedetails.languagepacks.empty())
                    gamedetails.dlcs.push_back(dlc_gamedetails);
            }
        }
    }

    return gamedetails;
}

std::vector<gameFile> galaxyAPI::installerJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& platform, const unsigned int& lang, const bool& useDuplicateHandler)
{
    std::vector<gameFile> gamefiles;
    unsigned int iInfoNodes = json.size();
    for (unsigned int i = 0; i < iInfoNodes; ++i)
    {
        Json::Value infoNode = json[i];
        unsigned int iFiles = infoNode["files"].size();
        std::string os = infoNode["os"].asString();
        std::string language = infoNode["language"].asString();
        std::string name = infoNode["name"].asString();

        unsigned int iPlatform = GlobalConstants::PLATFORM_WINDOWS;
        if (os == "windows")
            iPlatform = GlobalConstants::PLATFORM_WINDOWS;
        else if (os == "linux")
            iPlatform = GlobalConstants::PLATFORM_LINUX;
        else if (os == "mac")
            iPlatform = GlobalConstants::PLATFORM_MAC;

        if (!(iPlatform & platform))
            continue;

        unsigned int iLanguage = GlobalConstants::LANGUAGE_EN;
        iLanguage = Util::getOptionValue(language, GlobalConstants::LANGUAGES);

        if (!(iLanguage & lang))
            continue;

        for (unsigned int j = 0; j < iFiles; ++j)
        {
            Json::Value fileNode = infoNode["files"][j];
            std::string downlink = fileNode["downlink"].asString();

            std::string downlinkResponse = this->getResponse(downlink);

            if (downlinkResponse.empty())
                continue;

            Json::Value downlinkJson;
            Json::Reader *jsonparser = new Json::Reader;
            jsonparser->parse(downlinkResponse, downlinkJson);
            delete jsonparser;

            std::string downlink_url = downlinkJson["downlink"].asString();
            std::string downlink_url_unescaped = (std::string)curl_easy_unescape(curlhandle, downlink_url.c_str(), downlink_url.size(), NULL);
            std::string path;

            // GOG has changed the url formatting few times between 2 different formats.
            // Try to get proper file name in both cases.
            size_t filename_end_pos;
            if (downlink_url_unescaped.find("?path=") != std::string::npos)
                filename_end_pos = downlink_url_unescaped.find_first_of("&");
            else
                filename_end_pos = downlink_url_unescaped.find_first_of("?");

            if (downlink_url_unescaped.find("/" + gamename + "/") != std::string::npos)
            {
                path.assign(downlink_url_unescaped.begin()+downlink_url_unescaped.find("/" + gamename + "/"), downlink_url_unescaped.begin()+filename_end_pos);
            }
            else
            {
                path.assign(downlink_url_unescaped.begin()+downlink_url_unescaped.find_last_of("/")+1, downlink_url_unescaped.begin()+filename_end_pos);
                path = "/" + gamename + "/" + path;
            }

            gameFile gf;
            gf.gamename = gamename;
            gf.id = fileNode["id"].asString();
            gf.platform = iPlatform;
            gf.language = iLanguage;
            gf.name = name;
            gf.path = path;
            gf.size = fileNode["size"].asString();
            gf.updated = 0; // assume not updated
            gf.galaxy_downlink_json_url = downlink;

            if (useDuplicateHandler)
            {
                bool bDuplicate = false;
                for (unsigned int k = 0; k < gamefiles.size(); ++k)
                {
                    if (gamefiles[k].path == gf.path)
                    {
                        gamefiles[k].language |= gf.language; // Add language code to installer
                        bDuplicate = true;
                        break;
                    }
                }
                if (bDuplicate)
                    continue;
            }
            gamefiles.push_back(gf);
        }
    }

    return gamefiles;
}

std::vector<gameFile> galaxyAPI::patchJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& platform, const unsigned int& lang, const bool& useDuplicateHandler)
{
    return this->installerJsonNodeToGameFileVector(gamename, json, platform, lang, useDuplicateHandler);
}

std::vector<gameFile> galaxyAPI::languagepackJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& platform, const unsigned int& lang, const bool& useDuplicateHandler)
{
    return this->installerJsonNodeToGameFileVector(gamename, json, platform, lang, useDuplicateHandler);
}

std::vector<gameFile> galaxyAPI::extraJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json)
{
    std::vector<gameFile> gamefiles;
    unsigned int iInfoNodes = json.size();
    for (unsigned int i = 0; i < iInfoNodes; ++i)
    {
        Json::Value infoNode = json[i];
        unsigned int iFiles = infoNode["files"].size();
        std::string name = infoNode["name"].asString();

        for (unsigned int j = 0; j < iFiles; ++j)
        {
            Json::Value fileNode = infoNode["files"][j];
            std::string downlink = fileNode["downlink"].asString();

            std::string downlinkResponse = this->getResponse(downlink);

            if (downlinkResponse.empty())
                continue;

            Json::Value downlinkJson;
            Json::Reader *jsonparser = new Json::Reader;
            jsonparser->parse(downlinkResponse, downlinkJson);
            delete jsonparser;

            std::string downlink_url = downlinkJson["downlink"].asString();
            std::string downlink_url_unescaped = (std::string)curl_easy_unescape(curlhandle, downlink_url.c_str(), downlink_url.size(), NULL);
            std::string path;

            // GOG has changed the url formatting few times between 2 different formats.
            // Try to get proper file name in both cases.
            size_t filename_end_pos;
            if (downlink_url_unescaped.find("?path=") != std::string::npos)
                filename_end_pos = downlink_url_unescaped.find_first_of("&");
            else
                filename_end_pos = downlink_url_unescaped.find_first_of("?");

            if (downlink_url_unescaped.find("/" + gamename + "/") != std::string::npos)
            {
                path.assign(downlink_url_unescaped.begin()+downlink_url_unescaped.find("/" + gamename + "/"), downlink_url_unescaped.begin()+filename_end_pos);
            }
            else
            {
                path.assign(downlink_url_unescaped.begin()+downlink_url_unescaped.find_last_of("/")+1, downlink_url_unescaped.begin()+filename_end_pos);
                path = "/" + gamename + "/extras/" + path;
            }

            gameFile gf;
            gf.gamename = gamename;
            gf.type = GFTYPE_EXTRA;
            gf.id = fileNode["id"].asString();
            gf.name = name;
            gf.path = path;
            gf.size = fileNode["size"].asString();
            gf.updated = 0; // assume not updated
            gf.galaxy_downlink_json_url = downlink;

            gamefiles.push_back(gf);
        }
    }

    return gamefiles;
}
