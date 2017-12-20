/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "api.h"
#include "gamefile.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <json/json.h>
#include <unistd.h>

#if (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) >= 40900
#   define _regex_namespace_ std
#   include <regex>
#else
#   define _regex_namespace_ boost
#   include <boost/regex.hpp>
#endif

size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) {
    std::ostringstream *stream = (std::ostringstream*)userp;
    std::streamsize count = (std::streamsize) size * nmemb;
    stream->write(ptr, count);
    return count;
}

API::API(const std::string& token, const std::string& secret)
{
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);

    this->error = false;
    this->config.oauth_token = token;
    this->config.oauth_secret = secret;
}

/* Initialize the API
    returns 0 if failed
    returns 1 if successful
*/
int API::init()
{
    int res = 0;

    this->getAPIConfig();

    if (!this->getError())
        res = 1;
    else
        this->clearError();

    return res;
}

/* Login check
    returns false if not logged in
    returns true if logged in
*/
bool API::isLoggedIn()
{
    int res = 0;

    // Check if we already have token and secret
    if (!this->config.oauth_token.empty() && !this->config.oauth_secret.empty())
    {
        // Test authorization by getting user details
        res = this->getUserDetails(); // res = 1 if successful
    }

    return res;
}

int API::getAPIConfig()
{
    std::string url = "https://api.gog.com/downloader2/status/stable/"; // Stable API
    //std::string url = "https://api.gog.com/downloader2/status/beta/"; // Beta API
    //std::string url = "https://api.gog.com/downloader2/status/e77989ed21758e78331b20e477fc5582/"; // Development API? Not sure because the downloader version number it reports is lower than beta.

    std::string json = this->getResponse(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return 0;
    }

    Json::Value root;
    std::istringstream json_stream(json);

    try {
        json_stream >> root;
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getAPIConfig)" << std::endl << json << std::endl;
        #endif
        this->setError(exc.what());
        return 0;
    }

    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getAPIConfig)" << std::endl << root << std::endl;
    #endif
    this->config.oauth_authorize_temp_token = root["config"]["oauth_authorize_temp_token"].asString() + "/";
    this->config.oauth_get_temp_token = root["config"]["oauth_get_temp_token"].asString() + "/";
    this->config.oauth_get_token = root["config"]["oauth_get_token"].asString() + "/";
    this->config.get_user_games = root["config"]["get_user_games"].asString() + "/";
    this->config.get_user_details = root["config"]["get_user_details"].asString() + "/";
    this->config.get_installer_link = root["config"]["get_installer_link"].asString() + "/";
    this->config.get_game_details = root["config"]["get_game_details"].asString() + "/";
    this->config.get_extra_link = root["config"]["get_extra_link"].asString() + "/";
    this->config.set_app_status = root["config"]["set_app_status"].asString() + "/";
    return 1;
}

int API::login(const std::string& email, const std::string& password)
{
    int res = 0;
    std::string url;

    std::string token, secret;

    // Get temporary request token
    url = oauth_sign_url2(this->config.oauth_get_temp_token.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY.c_str(), CONSUMER_SECRET.c_str(), NULL /* token */, NULL /* secret */);

    std::string request_token_resp = this->getResponse(url);

    char **rv = NULL;
    int rc = oauth_split_url_parameters(request_token_resp.c_str(), &rv);
    qsort(rv, rc, sizeof(char *), oauth_cmpstringp);
    if (rc == 3 && !strncmp(rv[1], "oauth_token=", OAUTH_TOKEN_LENGTH) && !strncmp(rv[2], "oauth_token_secret=", OAUTH_SECRET_LENGTH)) {
        token = rv[1]+OAUTH_TOKEN_LENGTH+1;
        secret = rv[2]+OAUTH_SECRET_LENGTH+1;
        rv = NULL;
    }
    else
    {
        return res;
    }

    usleep(500); // Wait to avoid "429 Too Many Requests"

    // Authorize temporary token and get verifier
    url = this->config.oauth_authorize_temp_token + "?username=" + oauth_url_escape(email.c_str()) + "&password=" + oauth_url_escape(password.c_str());
    url = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY.c_str(), CONSUMER_SECRET.c_str(), token.c_str(), secret.c_str());
    std::string authorize_resp = this->getResponse(url);

    std::string verifier;
    rc = oauth_split_url_parameters(authorize_resp.c_str(), &rv);
    qsort(rv, rc, sizeof(char *), oauth_cmpstringp);
    if (rc == 2 && !strncmp(rv[1], "oauth_verifier=", OAUTH_VERIFIER_LENGTH)) {
        verifier = rv[1]+OAUTH_VERIFIER_LENGTH+1;
        rv = NULL;
    }
    else
    {
        return res;
    }

    usleep(500); // Wait to avoid "429 Too Many Requests"

    // Get final token and secret
    url = this->config.oauth_get_token + "?oauth_verifier=" + verifier;
    url = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY.c_str(), CONSUMER_SECRET.c_str(), token.c_str(), secret.c_str());
    std::string token_resp = this->getResponse(url);

    rc = oauth_split_url_parameters(token_resp.c_str(), &rv);
    qsort(rv, rc, sizeof(char *), oauth_cmpstringp);
    if (rc == 2 && !strncmp(rv[0], "oauth_token=", OAUTH_TOKEN_LENGTH) && !strncmp(rv[1], "oauth_token_secret=", OAUTH_SECRET_LENGTH)) {
        this->config.oauth_token = rv[0]+OAUTH_TOKEN_LENGTH+1;
        this->config.oauth_secret = rv[1]+OAUTH_SECRET_LENGTH+1;
        free(rv);
        res = 1;
    }

    return res;
}

int API::getUserDetails()
{
    std::string url;

    url = this->config.get_user_details;
    std::string json = this->getResponseOAuth(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return 0;
    }

    Json::Value root;
    std::istringstream json_stream(json);
    
    try {
        json_stream >> root;
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getUserDetails)" << std::endl << json << std::endl;
        #endif
        this->setError(exc.what());
        return 0;
    }

    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getUserDetails)" << std::endl << root << std::endl;
    #endif
    this->user.id = std::stoull(root["user"]["id"].asString());
    this->user.username = root["user"]["xywka"].asString();
    this->user.email = root["user"]["email"].asString();
    this->user.avatar_big = root["user"]["avatar"]["big"].asString();
    this->user.avatar_small = root["user"]["avatar"]["small"].asString();
    this->user.notifications_forum = root["user"]["notifications"]["forum"].isInt() ? root["user"]["notifications"]["forum"].asInt() : std::stoi(root["user"]["notifications"]["forum"].asString());
    this->user.notifications_games = root["user"]["notifications"]["games"].isInt() ? root["user"]["notifications"]["games"].asInt() : std::stoi(root["user"]["notifications"]["games"].asString());
    this->user.notifications_messages = root["user"]["notifications"]["messages"].isInt() ? root["user"]["notifications"]["messages"].asInt() : std::stoi(root["user"]["notifications"]["messages"].asString());
    return 1;
}


int API::getGames()
{
    // Not implemented on the server side currently

    //std::string json = this->getResponseOAuth(this->config.get_user_games);

    return 0;
}

std::string API::getResponse(const std::string& url)
{
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getResponse)" << std::endl << "URL: " << url << std::endl;
    #endif
    std::ostringstream memory;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    CURLcode result = curl_easy_perform(curlhandle);
    std::string response = memory.str();
    memory.str(std::string());

    if (result == CURLE_HTTP_RETURNED_ERROR)
    {
        long int response_code = 0;
        result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
        if (result == CURLE_OK)
            this->setError("HTTP ERROR: " + std::to_string(response_code) + " (" + url + ")");
        else
            this->setError("HTTP ERROR: failed to get error code: " + static_cast<std::string>(curl_easy_strerror(result)) + " (" + url + ")");

        #ifdef DEBUG
            curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, false);
            result = curl_easy_perform(curlhandle);
            std::string debug_response = memory.str();
            memory.str(std::string());
            std::cerr << "Response (CURLE_HTTP_RETURNED_ERROR):";
            if (debug_response.empty())
                std::cerr << " Response was empty" << std::endl;
            else
                std::cerr << std::endl << debug_response << std::endl;
            curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
        #endif
    }

    return response;
}

std::string API::getResponseOAuth(const std::string& url)
{
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getResponseOAuth)" << std::endl << "URL: " << url << std::endl;
    #endif
    std::string url_oauth = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY.c_str(), CONSUMER_SECRET.c_str(), this->config.oauth_token.c_str(), this->config.oauth_secret.c_str());
    std::string response = this->getResponse(url_oauth);

    return response;
}

gameDetails API::getGameDetails(const std::string& game_name, const unsigned int& platform, const unsigned int& lang, const bool& useDuplicateHandler)
{
    std::string url;
    gameDetails game;
    struct gameFileInfo
    {
        Json::Value jsonNode;
        unsigned int platform;
        unsigned int language;
    };

    url = this->config.get_game_details + game_name + "/" + "installer_win_en"; // can't get game details without file id, any file id seems to return all details which is good for us
    std::string json = this->getResponseOAuth(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return game;
    }

    Json::Value root;
    std::istringstream json_stream(json);

    try {
        json_stream >> root;
    } catch (Json::Exception exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getGameDetails)" << std::endl << json << std::endl;
        #endif
        this->setError(exc.what());
        return game;
    }

    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getGameDetails)" << std::endl << root << std::endl;
    #endif
    game.gamename = game_name;
    game.title = root["game"]["title"].asString();
    game.icon = root["game"]["icon"].asString();
    std::vector<std::string> membernames = root["game"].getMemberNames();
    
    // Installer details
    // Create a list of installers from JSON
    std::vector<gameFileInfo> installers;
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {   // Check against the specified platforms
        if (platform & GlobalConstants::PLATFORMS[i].id)
        {
            std::string installer = "installer_" + GlobalConstants::PLATFORMS[i].code + "_";
            for (unsigned int j = 0; j < GlobalConstants::LANGUAGES.size(); ++j)
            {   // Check against the specified languages
                if (lang & GlobalConstants::LANGUAGES[j].id)
                {   // Make sure that the installer exists in the JSON
                    if (root["game"].isMember(installer+GlobalConstants::LANGUAGES[j].code))
                    {
                        gameFileInfo installerInfo;
                        installerInfo.jsonNode = root["game"][installer+GlobalConstants::LANGUAGES[j].code];
                        installerInfo.platform = GlobalConstants::PLATFORMS[i].id;
                        installerInfo.language = GlobalConstants::LANGUAGES[j].id;
                        installers.push_back(installerInfo);
                    }
                }
            }
        }
    }

    for ( unsigned int i = 0; i < installers.size(); ++i )
    {
        for ( unsigned int index = 0; index < installers[i].jsonNode.size(); ++index )
        {
            Json::Value installer = installers[i].jsonNode[index];
            unsigned int language = installers[i].language;
            std::string path = installer["link"].asString();
            path = (std::string)curl_easy_unescape(curlhandle, path.c_str(), path.size(), NULL);

            // Check for duplicate installers in different languages and add languageId of duplicate installer to the original installer
            // https://secure.gog.com/forum/general/introducing_the_beta_release_of_the_new_gogcom_downloader/post1483
            if (useDuplicateHandler)
            {
                bool bDuplicate = false;
                for (unsigned int j = 0; j < game.installers.size(); ++j)
                {
                    if (game.installers[j].path == path)
                    {
                        game.installers[j].language |= language; // Add language code to installer
                        bDuplicate = true;
                        break;
                    }
                }
                if (bDuplicate)
                    continue;
            }

            gameFile gf;
            gf.type = GFTYPE_INSTALLER;
            gf.gamename = game.gamename;
            gf.updated = installer["notificated"].isInt() ? installer["notificated"].asInt() : std::stoi(installer["notificated"].asString());
            gf.id = installer["id"].isInt() ? std::to_string(installer["id"].asInt()) : installer["id"].asString();
            gf.name = installer["name"].asString();
            gf.path = path;
            gf.size = installer["size"].asString();
            gf.language = language;
            gf.platform = installers[i].platform;
            gf.silent = installer["silent"].isInt() ? installer["silent"].asInt() : std::stoi(installer["silent"].asString());

            game.installers.push_back(gf);
        }
    }

    // Extra details
    const Json::Value extras = root["game"]["extras"];
    for ( unsigned int index = 0; index < extras.size(); ++index )
    {
        Json::Value extra = extras[index];

        gameFile gf;
        gf.type = GFTYPE_EXTRA;
        gf.gamename = game.gamename;
        gf.updated = false; // extras don't have "updated" flag
        gf.id = extra["id"].isInt() ? std::to_string(extra["id"].asInt()) : extra["id"].asString();
        gf.name = extra["name"].asString();
        gf.path = extra["link"].asString();
        gf.path = (std::string)curl_easy_unescape(curlhandle, gf.path.c_str(), gf.path.size(), NULL);
        gf.size = extra["size_mb"].asString();

        game.extras.push_back(gf);
    }

    // Patch details
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {   // Check against the specified languages
        if (lang & GlobalConstants::LANGUAGES[i].id)
        {
            // Try to find a patch
            _regex_namespace_::regex re(GlobalConstants::LANGUAGES[i].code + "\\d+patch\\d+", _regex_namespace_::regex_constants::icase); // regex for patch node names
            std::vector<gameFileInfo> patches;
            for (unsigned int j = 0; j < membernames.size(); ++j)
            {
                if (_regex_namespace_::regex_match(membernames[j], re))
                {   // Regex matches, we have a patch node
                    gameFileInfo patchInfo;
                    patchInfo.jsonNode = root["game"][membernames[j]];
                    patchInfo.language = GlobalConstants::LANGUAGES[i].id;
                    if (patchInfo.jsonNode["link"].asString().find("/mac/") != std::string::npos)
                        patchInfo.platform = GlobalConstants::PLATFORM_MAC;
                    else if (patchInfo.jsonNode["link"].asString().find("/linux/") != std::string::npos)
                        patchInfo.platform = GlobalConstants::PLATFORM_LINUX;
                    else
                        patchInfo.platform = GlobalConstants::PLATFORM_WINDOWS;

                    if (platform & patchInfo.platform)
                        patches.push_back(patchInfo);
                }
            }

            if (!patches.empty()) // found at least one patch
            {
                for (unsigned int j = 0; j < patches.size(); ++j)
                {
                    Json::Value patchnode = patches[j].jsonNode;
                    if (patchnode.isArray()) // Patch has multiple files
                    {
                        for ( unsigned int index = 0; index < patchnode.size(); ++index )
                        {
                            Json::Value patch = patchnode[index];
                            std::string path = patch["link"].asString();
                            path = (std::string)curl_easy_unescape(curlhandle, path.c_str(), path.size(), NULL);

                            // Check for duplicate patches in different languages and add languageId of duplicate patch to the original patch
                            if (useDuplicateHandler)
                            {
                                bool bDuplicate = false;
                                for (unsigned int j = 0; j < game.patches.size(); ++j)
                                {
                                    if (game.patches[j].path == path)
                                    {
                                        game.patches[j].language |= GlobalConstants::LANGUAGES[i].id; // Add language code to patch
                                        bDuplicate = true;
                                        break;
                                    }
                                }
                                if (bDuplicate)
                                    continue;
                            }

                            gameFile gf;
                            gf.type = GFTYPE_PATCH;
                            gf.gamename = game.gamename;
                            gf.updated = patch["notificated"].isInt() ? patch["notificated"].asInt() : std::stoi(patch["notificated"].asString());
                            gf.id = patch["id"].isInt() ? std::to_string(patch["id"].asInt()) : patch["id"].asString();
                            gf.name = patch["name"].asString();
                            gf.path = path;
                            gf.size = patch["size"].asString();
                            gf.language = GlobalConstants::LANGUAGES[i].id;
                            gf.platform = patches[j].platform;

                            game.patches.push_back(gf);
                        }
                    }
                    else // Patch is a single file
                    {
                        std::string path = patchnode["link"].asString();
                        path = (std::string)curl_easy_unescape(curlhandle, path.c_str(), path.size(), NULL);

                        // Check for duplicate patches in different languages and add languageId of duplicate patch to the original patch
                        if (useDuplicateHandler)
                        {
                            bool bDuplicate = false;
                            for (unsigned int k = 0; k < game.patches.size(); ++k)
                            {
                                if (game.patches[k].path == path)
                                {
                                    game.patches[k].language |= GlobalConstants::LANGUAGES[i].id; // Add language code to patch
                                    bDuplicate = true;
                                    break;
                                }
                            }
                            if (bDuplicate)
                                continue;
                        }

                        gameFile gf;
                        gf.type = GFTYPE_PATCH;
                        gf.gamename = game.gamename;
                        gf.updated = patchnode["notificated"].isInt() ? patchnode["notificated"].asInt() : std::stoi(patchnode["notificated"].asString());
                        gf.id = patchnode["id"].isInt() ? std::to_string(patchnode["id"].asInt()) : patchnode["id"].asString();
                        gf.name = patchnode["name"].asString();
                        gf.path = path;
                        gf.size = patchnode["size"].asString();
                        gf.language = GlobalConstants::LANGUAGES[i].id;
                        gf.platform = patches[j].platform;

                        game.patches.push_back(gf);
                    }
                }
            }
        }
    }

    // Language pack details
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {   // Check against the specified languages
        if (lang & GlobalConstants::LANGUAGES[i].id)
        {
            // Try to find a language pack
            _regex_namespace_::regex re(GlobalConstants::LANGUAGES[i].code + "\\d+langpack\\d+", _regex_namespace_::regex_constants::icase); // regex for language pack node names
            std::vector<std::string> langpacknames;
            for (unsigned int j = 0; j < membernames.size(); ++j)
            {
                if (_regex_namespace_::regex_match(membernames[j], re))
                    langpacknames.push_back(membernames[j]);
            }

            if (!langpacknames.empty()) // found at least one language pack
            {
                for (unsigned int j = 0; j < langpacknames.size(); ++j)
                {
                    Json::Value langpack = root["game"][langpacknames[j]];

                    gameFile gf;
                    gf.type = GFTYPE_LANGPACK;
                    gf.gamename = game.gamename;
                    gf.updated = false; // language packs don't have "updated" flag
                    gf.id = langpack["id"].isInt() ? std::to_string(langpack["id"].asInt()) : langpack["id"].asString();
                    gf.name = langpack["name"].asString();
                    gf.path = langpack["link"].asString();
                    gf.path = (std::string)curl_easy_unescape(curlhandle, gf.path.c_str(), gf.path.size(), NULL);
                    gf.size = langpack["size"].asString();
                    gf.language = GlobalConstants::LANGUAGES[i].id;

                    game.languagepacks.push_back(gf);
                }
            }
        }
    }

    return game;
}


std::string API::getInstallerLink(const std::string& game_name, const std::string& id)
{
    std::string url, link;
    url = this->config.get_installer_link + game_name + "/" + id + "/";
    std::string json = this->getResponseOAuth(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return link;
    }

    Json::Value root;
    std::istringstream json_stream(json);

    try {
        json_stream >> root;
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getInstallerLink)" << std::endl << json << std::endl;
        #endif
        this->setError(exc.what());
        return link;
    }

    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getInstallerLink)" << std::endl << root << std::endl;
    #endif
    int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
    if (available)
        link = root["file"]["link"].asString();

    return link;
}

std::string API::getExtraLink(const std::string& game_name, const std::string& id)
{
    std::string url, link;
    url = this->config.get_extra_link + game_name + "/" + id + "/";
    std::string json = this->getResponseOAuth(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return link;
    }

    Json::Value root;
    std::istringstream json_stream(json);

    try {
        json_stream >> root;
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getExtraLink)" << std::endl << json << std::endl;
        #endif
            this->setError(exc.what());
        return link;
    }

    #ifdef DEBUG
        std::cerr << "DEBUG INFO (API::getExtraLink)" << std::endl << root << std::endl;
    #endif
    int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
    if (available)
        link = root["file"]["link"].asString();

    return link;
}

std::string API::getPatchLink(const std::string& game_name, const std::string& id)
{
    return this->getInstallerLink(game_name, id);
}

std::string API::getLanguagePackLink(const std::string& game_name, const std::string& id)
{
    return this->getInstallerLink(game_name, id);
}

std::string API::getXML(const std::string& game_name, const std::string& id)
{
    std::string url, XML;
    url = this->config.get_installer_link + game_name + "/" + id + "/crc/";
    std::string json = this->getResponseOAuth(url);

    if (json.empty()) {
        this->setError("Found nothing in " + url);
        return XML;
    }

    try {
        std::istringstream iss(json);
        Json::Value root;
        iss >> root;

        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getXML)" << std::endl << root << std::endl;
        #endif
        int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
        if (available)
        {
            url = root["file"]["link"].asString();
            XML = this->getResponse(url);
        }
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (API::getXML)" << std::endl << json << std::endl;
        #endif
        this->setError(exc.what());
    }

    return XML;
}

void API::clearError()
{
    this->error = false;
    this->error_message = "";
}

void API::setError(const std::string& err)
{
    this->error = true;
    if (this->error_message.empty())
        this->error_message = err;
    else
        this->error_message += "\n" + err;
}

API::~API()
{
    curl_easy_cleanup(curlhandle);
}
