/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "galaxyapi.h"
#include "message.h"
#include "ziputil.h"

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <sstream>

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
    Util::CurlHandleSetDefaultOptions(curlhandle, this->curlConf);
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

bool galaxyAPI::refreshLogin(const std::string &clientId, const std::string &clientSecret, const std::string &refreshToken, bool newSession)
{
    std::string refresh_url = "https://auth.gog.com/token?client_id=" + clientId
                            + "&client_secret=" + clientSecret
                            + "&grant_type=refresh_token"
                            + "&refresh_token=" + refreshToken
                            + (newSession ? "" : "&without_new_session=1");

    // std::cout << refresh_url << std::endl;
    Json::Value token_json = this->getResponseJson(refresh_url);

    if (token_json.empty())
        return false;

    token_json["client_id"] = clientId;
    token_json["client_secret"] = clientSecret;

    Globals::galaxyConf.setJSON(token_json);

    return true;
}

bool galaxyAPI::refreshLogin()
{
    return refreshLogin(Globals::galaxyConf.getClientId(), Globals::galaxyConf.getClientSecret(), Globals::galaxyConf.getRefreshToken(), true);
}

bool galaxyAPI::isTokenExpired()
{
    bool res = false;

    if (Globals::galaxyConf.isExpired())
        res = true;

    return res;
}

std::string galaxyAPI::getResponse(const std::string& url, const char *encoding)
{
    struct curl_slist *header = NULL;

    std::string access_token;
    if (!Globals::galaxyConf.isExpired())
        access_token = Globals::galaxyConf.getAccessToken();
    if (!access_token.empty())
    {
        std::string bearer = "Authorization: Bearer " + access_token;
        header = curl_slist_append(header, bearer.c_str());
    }

    if(encoding) {
        auto accept = "Accept: " + std::string(encoding);
        header = curl_slist_append(header, accept.c_str());
    }

    curl_easy_setopt(curlhandle, CURLOPT_HTTPHEADER, header);
    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_ACCEPT_ENCODING, "");

    int max_retries = std::min(3, Globals::globalConfig.iRetries);
    std::string response;
    auto res = Util::CurlHandleGetResponse(curlhandle, response, max_retries);

    if(res) {
        long int response_code = 0;
        curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);

        if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
            std::cout << "Response code for " << url << " is [" << response_code << ']' << std::endl;
    }

    curl_easy_setopt(curlhandle, CURLOPT_ACCEPT_ENCODING, NULL);
    curl_easy_setopt(curlhandle, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(header);

    return response;
}

Json::Value galaxyAPI::getResponseJson(const std::string& url, const char *encoding)
{
    std::istringstream response(this->getResponse(url, encoding));
    Json::Value json;

    if (!response.str().empty())
    {
        try
        {
            response >> json;
        }
        catch(const Json::Exception& exc)
        {
            // Failed to parse json response
            // Check for zlib header and decompress if header found
            response.seekg(0, response.beg);
            uint16_t header = ZipUtil::readUInt16(&response);
            std::vector<uint16_t> zlib_headers = { 0x0178, 0x5e78, 0x9c78, 0xda78 };
            bool is_zlib_compressed = std::any_of(
                                zlib_headers.begin(), zlib_headers.end(),
                                [header](uint16_t zlib_header)
                                {
                                    return header == zlib_header;
                                }
                            );

            if (is_zlib_compressed)
            {
                std::string response_compressed = response.str();
                std::stringstream response_decompressed;
                boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
                in.push(boost::iostreams::zlib_decompressor(GlobalConstants::ZLIB_WINDOW_SIZE));
                in.push(boost::make_iterator_range(response_compressed));
                boost::iostreams::copy(in, response_decompressed);

                try
                {
                    response_decompressed >> json;
                }
                catch(const Json::Exception& exc)
                {
                    // Failed to parse json

                    std::cout << "Failed to parse json: " << exc.what();
                }
            }
            else {
                std::cout << "Failed to parse json: " << exc.what();
            }
        }
    }

    return json;
}

Json::Value galaxyAPI::getProductBuilds(const std::string& product_id, const std::string& platform, const std::string& generation)
{
    std::string url = "https://content-system.gog.com/products/" + product_id + "/os/" + platform + "/builds?generation=" + generation;

    return this->getResponseJson(url);
}

Json::Value galaxyAPI::getManifestV1(const std::string& product_id, const std::string& build_id, const std::string& manifest_id, const std::string& platform)
{
    std::string url = "https://cdn.gog.com/content-system/v1/manifests/" + product_id + "/" + platform + "/" + build_id + "/" + manifest_id + ".json";

    return this->getManifestV1(url);
}

Json::Value galaxyAPI::getManifestV1(const std::string& manifest_url)
{
    return this->getResponseJson(manifest_url);
}

Json::Value galaxyAPI::getManifestV2(std::string manifest_hash, const bool& is_dependency)
{
    if (!manifest_hash.empty() && manifest_hash.find("/") == std::string::npos)
        manifest_hash = this->hashToGalaxyPath(manifest_hash);

    std::string url;
    if (is_dependency)
        url = "https://cdn.gog.com/content-system/v2/dependencies/meta/" + manifest_hash;
    else
        url = "https://cdn.gog.com/content-system/v2/meta/" + manifest_hash;

    return this->getResponseJson(url);
}

Json::Value galaxyAPI::getCloudPathAsJson(const std::string &clientId) {
    std::string url = "https://remote-config.gog.com/components/galaxy_client/clients/" + clientId + "?component_version=2.0.51";

    return this->getResponseJson(url);
}

Json::Value galaxyAPI::getSecureLink(const std::string& product_id, const std::string& path)
{
    std::string url = "https://content-system.gog.com/products/" + product_id + "/secure_link?generation=2&path=" + path + "&_version=2";

    return this->getResponseJson(url);
}

Json::Value galaxyAPI::getDependencyLink(const std::string& path)
{
    std::string url = "https://content-system.gog.com/open_link?generation=2&_version=2&path=/dependencies/store/" + path;

    return this->getResponseJson(url);
}


std::string galaxyAPI::hashToGalaxyPath(const std::string& hash)
{
    std::string galaxy_path = hash;
    if (galaxy_path.find("/") == std::string::npos)
        galaxy_path.assign(hash.begin(), hash.begin()+2).append("/").append(hash.begin()+2, hash.begin()+4).append("/").append(hash);

    return galaxy_path;
}

std::vector<galaxyDepotItem> galaxyAPI::getDepotItemsVector(const std::string& hash, const bool& is_dependency)
{
    Json::Value json = this->getManifestV2(hash, is_dependency);

    std::vector<galaxyDepotItem> items;
    if (json["depot"].isMember("smallFilesContainer"))
    {
        if (json["depot"]["smallFilesContainer"]["chunks"].isArray())
        {
            galaxyDepotItem item;
            item.totalSizeCompressed = 0;
            item.totalSizeUncompressed = 0;
            item.path = "galaxy_smallfilescontainer";
            item.isDependency = is_dependency;
            item.isSmallFilesContainer = true;

            for (unsigned int i = 0; i < json["depot"]["smallFilesContainer"]["chunks"].size(); ++i)
            {
                Json::Value json_chunk = json["depot"]["smallFilesContainer"]["chunks"][i];

                galaxyDepotItemChunk chunk;
                chunk.md5_compressed = json_chunk["compressedMd5"].asString();
                chunk.md5_uncompressed = json_chunk["md5"].asString();
                chunk.size_compressed = json_chunk["compressedSize"].asLargestUInt();
                chunk.size_uncompressed = json_chunk["size"].asLargestUInt();

                chunk.offset_compressed = item.totalSizeCompressed;
                chunk.offset_uncompressed = item.totalSizeUncompressed;

                item.totalSizeCompressed += chunk.size_compressed;
                item.totalSizeUncompressed += chunk.size_uncompressed;
                item.chunks.push_back(chunk);
            }

            if (json["depot"]["smallFilesContainer"].isMember("md5"))
                item.md5 = json["depot"]["smallFilesContainer"]["md5"].asString();
            else if (json["depot"]["smallFilesContainer"]["chunks"].size() == 1)
                item.md5 = json["depot"]["smallFilesContainer"]["chunks"][0]["md5"].asString();
            else
                item.md5 = std::string();

            items.push_back(item);
        }
    }

    for (unsigned int i = 0; i < json["depot"]["items"].size(); ++i)
    {
        if (json["depot"]["items"][i]["chunks"].isArray())
        {
            galaxyDepotItem item;
            item.totalSizeCompressed = 0;
            item.totalSizeUncompressed = 0;
            item.path = json["depot"]["items"][i]["path"].asString();
            item.isDependency = is_dependency;

            if (json["depot"]["items"][i].isMember("sfcRef"))
            {
                item.isInSFC = true;
                item.sfc_offset = json["depot"]["items"][i]["sfcRef"]["offset"].asLargestUInt();
                item.sfc_size = json["depot"]["items"][i]["sfcRef"]["size"].asLargestUInt();
            }

            while (Util::replaceString(item.path, "\\", "/"));
            for (unsigned int j = 0; j < json["depot"]["items"][i]["chunks"].size(); ++j)
            {
                Json::Value json_chunk = json["depot"]["items"][i]["chunks"][j];

                galaxyDepotItemChunk chunk;
                chunk.md5_compressed = json_chunk["compressedMd5"].asString();
                chunk.md5_uncompressed = json_chunk["md5"].asString();
                chunk.size_compressed = json_chunk["compressedSize"].asLargestUInt();
                chunk.size_uncompressed = json_chunk["size"].asLargestUInt();

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
            else
                item.md5 = std::string();

            items.push_back(item);
        }
    }

    return items;
}

Json::Value galaxyAPI::getProductInfo(const std::string& product_id)
{
    std::string url = "https://api.gog.com/products/" + product_id + "?expand=downloads,expanded_dlcs,description,screenshots,videos,related_products,changelog&locale=en-US";

    return this->getResponseJson(url);
}

gameDetails galaxyAPI::productInfoJsonToGameDetails(const Json::Value& json, const DownloadConfig& dlConf)
{
    gameDetails gamedetails;

    gamedetails.gamename = json["slug"].asString();
    gamedetails.product_id = json["id"].asString();
    gamedetails.title = json["title"].asString();
    gamedetails.icon = "https:" + json["images"]["icon"].asString();
    gamedetails.logo = "https:" + json["images"]["logo"].asString();

    Util::replaceString(gamedetails.logo, "_glx_logo.jpg", ".jpg");

    if (json.isMember("changelog"))
        gamedetails.changelog = json["changelog"].asString();

    if (dlConf.iInclude & GlobalConstants::GFTYPE_INSTALLER)
    {
        gamedetails.installers = this->fileJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["installers"], GlobalConstants::GFTYPE_BASE_INSTALLER, dlConf);
    }

    if (dlConf.iInclude & GlobalConstants::GFTYPE_EXTRA)
    {
        gamedetails.extras = this->fileJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["bonus_content"], GlobalConstants::GFTYPE_BASE_EXTRA, dlConf);
    }

    if (dlConf.iInclude & GlobalConstants::GFTYPE_PATCH)
    {
        gamedetails.patches = this->fileJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["patches"], GlobalConstants::GFTYPE_BASE_PATCH, dlConf);
    }

    if (dlConf.iInclude & GlobalConstants::GFTYPE_LANGPACK)
    {
        gamedetails.languagepacks = this->fileJsonNodeToGameFileVector(gamedetails.gamename, json["downloads"]["language_packs"], GlobalConstants::GFTYPE_BASE_LANGPACK, dlConf);
    }

    if (dlConf.iInclude & GlobalConstants::GFTYPE_DLC)
    {
        if (json.isMember("expanded_dlcs"))
        {
            for (unsigned int i = 0; i < json["expanded_dlcs"].size(); ++i)
            {
                std::string dlc_id = json["expanded_dlcs"][i]["id"].asString();

                if (!Globals::vOwnedGamesIds.empty())
                {
                    if (std::find(Globals::vOwnedGamesIds.begin(), Globals::vOwnedGamesIds.end(), dlc_id) == Globals::vOwnedGamesIds.end())
                        continue;
                }

                gameDetails dlc_gamedetails = this->productInfoJsonToGameDetails(json["expanded_dlcs"][i], dlConf);

                // Add DLC type to all DLC files
                for (unsigned int j = 0; j < dlc_gamedetails.installers.size(); ++j)
                    dlc_gamedetails.installers[j].type = GlobalConstants::GFTYPE_DLC_INSTALLER;
                for (unsigned int j = 0; j < dlc_gamedetails.extras.size(); ++j)
                    dlc_gamedetails.extras[j].type = GlobalConstants::GFTYPE_DLC_EXTRA;
                for (unsigned int j = 0; j < dlc_gamedetails.patches.size(); ++j)
                    dlc_gamedetails.patches[j].type = GlobalConstants::GFTYPE_DLC_PATCH;
                for (unsigned int j = 0; j < dlc_gamedetails.languagepacks.size(); ++j)
                    dlc_gamedetails.languagepacks[j].type = GlobalConstants::GFTYPE_DLC_LANGPACK;

                // Add DLC only if it has any files
                if (!dlc_gamedetails.installers.empty() || !dlc_gamedetails.extras.empty() || !dlc_gamedetails.patches.empty() || !dlc_gamedetails.languagepacks.empty())
                    gamedetails.dlcs.push_back(dlc_gamedetails);
            }
        }
    }

    return gamedetails;
}

std::vector<gameFile> galaxyAPI::fileJsonNodeToGameFileVector(const std::string& gamename, const Json::Value& json, const unsigned int& type, const DownloadConfig& dlConf)
{
    std::vector<gameFile> gamefiles;
    unsigned int iInfoNodes = json.size();
    for (unsigned int i = 0; i < iInfoNodes; ++i)
    {
        Json::Value infoNode = json[i];
        unsigned int iFiles = infoNode["files"].size();
        std::string name = infoNode["name"].asString();
        std::string version = "";
        if (!infoNode["version"].empty())
            version = infoNode["version"].asString();

        unsigned int iPlatform = GlobalConstants::PLATFORM_WINDOWS;
        unsigned int iLanguage = GlobalConstants::LANGUAGE_EN;
        if (!(type & GlobalConstants::GFTYPE_EXTRA))
        {
            iPlatform = Util::getOptionValue(infoNode["os"].asString(), GlobalConstants::PLATFORMS);
            iLanguage = Util::getOptionValue(infoNode["language"].asString(), GlobalConstants::LANGUAGES);

            if (!(iPlatform & dlConf.iInstallerPlatform))
                continue;

            if (!(iLanguage & dlConf.iInstallerLanguage))
                continue;
        }

        // Skip file if count and total_size is zero
        // https://github.com/Sude-/lgogdownloader/issues/200
        unsigned int count = infoNode["count"].asUInt();
        uintmax_t total_size = infoNode["total_size"].asLargestUInt();
        if (count == 0 && total_size == 0)
            continue;

        for (unsigned int j = 0; j < iFiles; ++j)
        {
            Json::Value fileNode = infoNode["files"][j];
            std::string downlink = fileNode["downlink"].asString();

            Json::Value downlinkJson = this->getResponseJson(downlink);
            if (downlinkJson.empty())
                continue;

            std::string downlink_url = downlinkJson["downlink"].asString();
            std::string path = this->getPathFromDownlinkUrl(downlink_url, gamename);

            // Check to see if path ends in "/secure" or "/securex" which means that we got invalid path for some reason
            boost::regex path_re("/securex?$", boost::regex::perl | boost::regex::icase);
            boost::match_results<std::string::const_iterator> what;
            if (boost::regex_search(path, what, path_re))
                continue;

            gameFile gf;
            gf.gamename = gamename;
            gf.type = type;
            gf.id = fileNode["id"].asString();
            gf.name = name;
            gf.path = path;
            gf.size = Util::getJsonUIntValueAsString(fileNode["size"]);
            gf.updated = 0; // assume not updated
            gf.galaxy_downlink_json_url = downlink;
            gf.version = version;

            if (!(type & GlobalConstants::GFTYPE_EXTRA))
            {
                gf.platform = iPlatform;
                gf.language = iLanguage;
            }

            if (dlConf.bDuplicateHandler)
            {
                bool bDuplicate = false;
                for (unsigned int k = 0; k < gamefiles.size(); ++k)
                {
                    if (gamefiles[k].path == gf.path)
                    {
                        if (!(type & GlobalConstants::GFTYPE_EXTRA))
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

Json::Value galaxyAPI::getUserData()
{
    std::string url = "https://embed.gog.com/userData.json";

    return this->getResponseJson(url);
}

Json::Value galaxyAPI::getDependenciesJson()
{
    std::string url = "https://content-system.gog.com/dependencies/repository?generation=2";
    Json::Value dependencies;
    Json::Value repository = this->getResponseJson(url);

    if (!repository.empty())
    {
        if (repository.isMember("repository_manifest"))
        {
            std::string manifest_url = repository["repository_manifest"].asString();
            dependencies = this->getResponseJson(manifest_url);
        }
    }

    return dependencies;
}

std::vector<galaxyDepotItem> galaxyAPI::getFilteredDepotItemsVectorFromJson(const Json::Value& depot_json, const std::string& galaxy_language, const std::string& galaxy_arch, const bool& is_dependency)
{
    std::vector<galaxyDepotItem> items;

    bool bSelectedLanguage = false;
    bool bSelectedArch = false;
    boost::regex language_re("^(" + galaxy_language + ")$", boost::regex::perl | boost::regex::icase);
    boost::match_results<std::string::const_iterator> what;
    for (unsigned int j = 0; j < depot_json["languages"].size(); ++j)
    {
        std::string language = depot_json["languages"][j].asString();
        if (language == "*" || boost::regex_search(language, what, language_re))
            bSelectedLanguage = true;
    }

    if (depot_json.isMember("osBitness"))
    {
        for (unsigned int j = 0; j < depot_json["osBitness"].size(); ++j)
        {
            std::string osBitness = depot_json["osBitness"][j].asString();
            if (osBitness == "*" || osBitness == galaxy_arch)
                bSelectedArch = true;
        }
    }
    else
    {
        // No osBitness found, assume that we want this depot
        bSelectedArch = true;
    }

    if (bSelectedLanguage && bSelectedArch)
    {
        std::string depotHash = depot_json["manifest"].asString();
        std::string depot_product_id = depot_json["productId"].asString();

        items = this->getDepotItemsVector(depotHash, is_dependency);

        // Set product id for items
        if (!depot_product_id.empty())
        {
            for (auto it = items.begin(); it != items.end(); ++it)
            {
                it->product_id = depot_product_id;
            }
        }
    }

    return items;
}

std::string galaxyAPI::getPathFromDownlinkUrl(const std::string& downlink_url, const std::string& gamename)
{
    std::string path;
    std::string downlink_url_unescaped = (std::string)curl_easy_unescape(curlhandle, downlink_url.c_str(), downlink_url.size(), NULL);
    size_t filename_start_pos = 0;

    // If url ends in "/" then remove it
    if (downlink_url_unescaped.back() == '/')
        downlink_url_unescaped.assign(downlink_url_unescaped.begin(), downlink_url_unescaped.end()-1);

    // Assume that filename starts after last "/" in url
    if (downlink_url_unescaped.find_last_of("/") != std::string::npos)
        filename_start_pos = downlink_url_unescaped.find_last_of("/") + 1;

    // Url contains "/gamename/"
    if (downlink_url_unescaped.find("/" + gamename + "/") != std::string::npos)
        filename_start_pos = downlink_url_unescaped.find("/" + gamename + "/");

    // Assume that filename ends at the end of url
    size_t filename_end_pos = downlink_url_unescaped.length();

    // Check to see if url has any query strings
    if (downlink_url_unescaped.find("?") != std::string::npos)
    {
        // Assume that filename ends at first "?"
        filename_end_pos = downlink_url_unescaped.find_first_of("?");

        // Check for "?path="
        if (downlink_url_unescaped.find("?path=") != std::string::npos)
        {
            size_t token_pos = downlink_url_unescaped.find("&token=");
            size_t access_token_pos = downlink_url_unescaped.find("&access_token=");
            if ((token_pos != std::string::npos) && (access_token_pos != std::string::npos))
            {
                filename_end_pos = std::min(token_pos, access_token_pos);
            }
            else
            {
                if (downlink_url_unescaped.find_first_of("&") != std::string::npos)
                    filename_end_pos = downlink_url_unescaped.find_first_of("&");
            }
        }
    }

    path.assign(downlink_url_unescaped.begin()+filename_start_pos, downlink_url_unescaped.begin()+filename_end_pos);

    // Make sure that path contains "/gamename/"
    if (path.find("/" + gamename + "/") == std::string::npos)
        path = "/" + gamename + "/" + path;

    // Workaround for filename issue caused by different (currently unknown) url formatting scheme
    // https://github.com/Sude-/lgogdownloader/issues/126
    if (path.find("?") != std::string::npos)
    {
        if (path.find_last_of("?") > path.find_last_of("/"))
        {
            path.assign(path.begin(), path.begin()+path.find_last_of("?"));
        }
    }

    return path;
}

std::vector<std::string> galaxyAPI::cdnUrlTemplatesFromJson(const Json::Value& json, const std::vector<std::string>& cdnPriority)
{
    // Handle priority of CDNs
    struct urlPriority
    {
        std::string url;
        int priority;
    };
    std::vector<urlPriority> cdnUrls;

    // Build a vector of all urls and their priority score
    for (unsigned int i = 0; i < json["urls"].size(); ++i)
    {
        std::string endpoint_name = json["urls"][i]["endpoint_name"].asString();

        unsigned int score = cdnPriority.size();
        for (unsigned int idx = 0; idx < score; ++idx)
        {
            if (endpoint_name == cdnPriority[idx])
            {
                score = idx;
                break;
            }
        }

        // Couldn't find a match when assigning score
        if (score == cdnPriority.size())
        {
            // Add index value to score
            // This way unknown CDNs have priority based on the order they appear in json
            score += i;
        }

        // Build url according to url_format
        std::string url = json["urls"][i]["url_format"].asString();
        for (auto cdn_url_template_param : json["urls"][i]["parameters"].getMemberNames())
        {
            std::string template_to_replace = "{" + cdn_url_template_param + "}";
            std::string replacement = json["urls"][i]["parameters"][cdn_url_template_param].asString();

            // Add our own template to path
            if (template_to_replace == "{path}")
            {
                replacement += "{LGOGDOWNLOADER_GALAXY_PATH}";
            }

            while(Util::replaceString(url, template_to_replace, replacement));
        }

        urlPriority cdnurl;
        cdnurl.url = url;
        cdnurl.priority = score;
        cdnUrls.push_back(cdnurl);
    }

    if (!cdnUrls.empty())
    {
        // Sort urls by priority (lowest score first)
        std::sort(cdnUrls.begin(), cdnUrls.end(),
            [](urlPriority a, urlPriority b)
            {
                return (a.priority < b.priority);
            }
        );
    }

    std::vector<std::string> cdnUrlTemplates;
    for (auto cdnurl : cdnUrls)
        cdnUrlTemplates.push_back(cdnurl.url);

    return cdnUrlTemplates;
}
