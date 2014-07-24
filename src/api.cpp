/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "api.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <jsoncpp/json/json.h>

size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) {
    std::ostringstream *stream = (std::ostringstream*)userp;
    size_t count = size * nmemb;
    stream->write(ptr, count);
    return count;
}

gameFile::gameFile(const int& t_updated, const std::string& t_id, const std::string& t_name, const std::string& t_path, const std::string& t_size, const unsigned int& t_language, const int& t_silent)
{
    this->updated = t_updated;
    this->id = t_id;
    this->name = t_name;
    this->path = t_path;
    this->size = t_size;
    this->language = t_language;
    this->silent = t_silent;
}

gameFile::~gameFile()
{

}

API::API(const std::string& token, const std::string& secret)
{
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);

    this->error = false;
    this->config.oauth_token = token;
    this->config.oauth_secret = secret;
}

int API::init()
{
    int res = 0;
    this->getAPIConfig();

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
    int res = 0;

    std::string json = this->getResponse(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
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
            res = 1;
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getAPIConfig)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
            res = 0;
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
        res = 0;
    }

    return res;
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
    int res = 0;
    std::string url;

    url = this->config.get_user_details;
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
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
            res = 1;
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getUserDetails)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
            res = 0;
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
        res = 0;
    }

    return res;
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
            this->setError("HTTP ERROR: " + std::to_string(response_code));
        else
            this->setError("HTTP ERROR: failed to get error code: " + static_cast<std::string>(curl_easy_strerror(result)));
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
    unsigned int type = platform;

    url = this->config.get_game_details + game_name + "/" + "installer_win_en"; // can't get game details without file id, any file id seems to return all details which is good for us
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getGameDetails)" << std::endl << root << std::endl;
            #endif
            game.gamename = game_name;
            game.title = root["game"]["title"].asString();
            game.icon = root["game"]["icon"].asString();


            // FIXME: Replace this ugly hack when GOG makes the API responses for Linux better
            bool bIsLinux = false;
            bool bIsMac = false;
            if (type & (GlobalConstants::PLATFORM_LINUX | GlobalConstants::PLATFORM_MAC) )
            {
                if (type & GlobalConstants::PLATFORM_LINUX)
                    bIsLinux = true;
                else
                    bIsLinux = false;
                if (type & GlobalConstants::PLATFORM_MAC)
                    bIsMac = true;
                else
                    bIsMac = false;
                type |= GlobalConstants::PLATFORM_MAC; // For some reason Linux installers are under Mac installer node so add Mac to installer type
            }


            // Installer details
            // Create a list of installers from JSON
            std::vector<std::pair<Json::Value,unsigned int>> installers;
            for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
            {   // Check against the specified platforms
                if (type & GlobalConstants::PLATFORMS[i].platformId)
                {
                    std::string installer = "installer_" + GlobalConstants::PLATFORMS[i].platformCode + "_";
                    for (unsigned int j = 0; j < GlobalConstants::LANGUAGES.size(); ++j)
                    {   // Check against the specified languages
                        if (lang & GlobalConstants::LANGUAGES[j].languageId)
                        {   // Make sure that the installer exists in the JSON
                            if (root["game"].isMember(installer+GlobalConstants::LANGUAGES[j].languageCode))
                                installers.push_back(std::make_pair(root["game"][installer+GlobalConstants::LANGUAGES[j].languageCode],GlobalConstants::LANGUAGES[j].languageId));
                        }
                    }
                }
            }

            for ( unsigned int i = 0; i < installers.size(); ++i )
            {
                for ( unsigned int index = 0; index < installers[i].first.size(); ++index )
                {
                    Json::Value installer = installers[i].first[index];
                    unsigned int language = installers[i].second;

                    // Check for duplicate installers in different languages and add languageId of duplicate installer to the original installer
                    // https://secure.gog.com/forum/general/introducing_the_beta_release_of_the_new_gogcom_downloader/post1483
                    if (useDuplicateHandler)
                    {
                        bool bDuplicate = false;
                        for (unsigned int j = 0; j < game.installers.size(); ++j)
                        {
                            if (game.installers[j].path == installer["link"].asString())
                            {
                                game.installers[j].language |= language; // Add language code to installer
                                bDuplicate = true;
                                break;
                            }
                        }
                        if (bDuplicate)
                            continue;
                    }

                    // FIXME: Replace this ugly hack when GOG makes the API responses for Linux better
                    if (bIsLinux && !bIsMac)
                    {
                        if (installer["link"].asString().find("/mac/") != std::string::npos)
                            continue;
                    }
                    if (!bIsLinux && bIsMac)
                    {
                        if (installer["link"].asString().find("/linux/") != std::string::npos)
                            continue;
                    }

                    game.installers.push_back(
                                                gameFile(   installer["notificated"].isInt() ? installer["notificated"].asInt() : std::stoi(installer["notificated"].asString()),
                                                            installer["id"].isInt() ? std::to_string(installer["id"].asInt()) : installer["id"].asString(),
                                                            installer["name"].asString(),
                                                            installer["link"].asString(),
                                                            installer["size"].asString(),
                                                            language,
                                                            installer["silent"].isInt() ? installer["silent"].asInt() : std::stoi(installer["silent"].asString())
                                                         )
                                            );
                }
            }

            // Extra details
            const Json::Value extras = root["game"]["extras"];
            for ( unsigned int index = 0; index < extras.size(); ++index )
            {
                Json::Value extra = extras[index];

                game.extras.push_back(
                                        gameFile(   false, /* extras don't have "updated" flag */
                                                    extra["id"].isInt() ? std::to_string(extra["id"].asInt()) : extra["id"].asString(),
                                                    extra["name"].asString(),
                                                    extra["link"].asString(),
                                                    extra["size_mb"].asString()
                                                 )
                                    );
            }

            // Patch details
            for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
            {   // Check against the specified languages
                if (lang & GlobalConstants::LANGUAGES[i].languageId)
                {
                    // Try to find a patch
                    unsigned int patch_number = 0;
                    const unsigned int maxTries = 8;
                    std::vector<std::string> patchnames;
                    while (patch_number < maxTries)
                    {
                        unsigned int patch_number_file = 0;
                        while (patch_number_file < maxTries)
                        {
                            std::string patchname = GlobalConstants::LANGUAGES[i].languageCode + std::to_string(patch_number) + "patch" + std::to_string(patch_number_file);
                            if (root["game"].isMember(patchname)) // Check that patch node exists
                            {
                                unsigned int platformId;
                                if (root["game"][patchname]["link"].asString().find("/mac/") != std::string::npos)
                                    platformId = GlobalConstants::PLATFORM_MAC;
                                else if (root["game"][patchname]["link"].asString().find("/linux/") != std::string::npos)
                                    platformId = GlobalConstants::PLATFORM_LINUX;
                                else
                                    platformId = GlobalConstants::PLATFORM_WINDOWS;

                                if (type & platformId)
                                    patchnames.push_back(patchname);
                            }
                            patch_number_file++;
                        }
                        patch_number++;
                    }

                    if (!patchnames.empty()) // found at least one patch
                    {
                        for (unsigned int i = 0; i < patchnames.size(); ++i)
                        {
                            Json::Value patchnode = root["game"][patchnames[i]];
                            if (patchnode.isArray()) // Patch has multiple files
                            {
                                for ( unsigned int index = 0; index < patchnode.size(); ++index )
                                {
                                    Json::Value patch = patchnode[index];

                                    // Check for duplicate patches in different languages and add languageId of duplicate patch to the original patch
                                    if (useDuplicateHandler)
                                    {
                                        bool bDuplicate = false;
                                        for (unsigned int j = 0; j < game.patches.size(); ++j)
                                        {
                                            if (game.patches[j].path == patch["link"].asString())
                                            {
                                                game.patches[j].language |= GlobalConstants::LANGUAGES[i].languageId; // Add language code to patch
                                                bDuplicate = true;
                                                break;
                                            }
                                        }
                                        if (bDuplicate)
                                            continue;
                                    }

                                    game.patches.push_back(
                                                            gameFile(   false, /* patches don't have "updated" flag */
                                                                        patch["id"].isInt() ? std::to_string(patch["id"].asInt()) : patch["id"].asString(),
                                                                        patch["name"].asString(),
                                                                        patch["link"].asString(),
                                                                        patch["size"].asString(),
                                                                        GlobalConstants::LANGUAGES[i].languageId
                                                                    )
                                                        );
                                }
                            }
                            else // Patch is a single file
                            {
                                // Check for duplicate patches in different languages and add languageId of duplicate patch to the original patch
                                if (useDuplicateHandler)
                                {
                                    bool bDuplicate = false;
                                    for (unsigned int j = 0; j < game.patches.size(); ++j)
                                    {
                                        if (game.patches[j].path == patchnode["link"].asString())
                                        {
                                            game.patches[j].language |= GlobalConstants::LANGUAGES[i].languageId; // Add language code to patch
                                            bDuplicate = true;
                                            break;
                                        }
                                    }
                                    if (bDuplicate)
                                        continue;
                                }

                                game.patches.push_back(
                                                        gameFile(   false, /* patches don't have "updated" flag */
                                                                    patchnode["id"].isInt() ? std::to_string(patchnode["id"].asInt()) : patchnode["id"].asString(),
                                                                    patchnode["name"].asString(),
                                                                    patchnode["link"].asString(),
                                                                    patchnode["size"].asString(),
                                                                    GlobalConstants::LANGUAGES[i].languageId
                                                                 )
                                                        );
                            }
                        }
                    }
                }
            }

            // Language pack details
            for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
            {   // Check against the specified languages
                if (lang & GlobalConstants::LANGUAGES[i].languageId)
                {
                    // Try to find a language pack
                    unsigned int lang_pack_number = 0;
                    const unsigned int maxTries = 4;
                    std::vector<std::string> langpacknames;
                    while (lang_pack_number < maxTries)
                    {
                        unsigned int lang_pack_number_file = 0;
                        while (lang_pack_number_file < maxTries)
                        {
                            std::string langpackname = GlobalConstants::LANGUAGES[i].languageCode + std::to_string(lang_pack_number) + "langpack" + std::to_string(lang_pack_number_file);
                            if (root["game"].isMember(langpackname)) // Check that language pack node exists
                                langpacknames.push_back(langpackname);
                            lang_pack_number_file++;
                        }
                        lang_pack_number++;
                    }

                    if (!langpacknames.empty()) // found at least one language pack
                    {
                        for (unsigned int i = 0; i < langpacknames.size(); ++i)
                        {
                            Json::Value langpack = root["game"][langpacknames[i]];
                            game.languagepacks.push_back(
                                                        gameFile(   false, /* language packs don't have "updated" flag */
                                                                    langpack["id"].isInt() ? std::to_string(langpack["id"].asInt()) : langpack["id"].asString(),
                                                                    langpack["name"].asString(),
                                                                    langpack["link"].asString(),
                                                                    langpack["size"].asString(),
                                                                    GlobalConstants::LANGUAGES[i].languageId
                                                            )
                                                    );
                        }
                    }
                }
            }
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getGameDetails)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
    }

    return game;
}


std::string API::getInstallerLink(const std::string& game_name, const std::string& id)
{
    std::string url, link;
    url = this->config.get_installer_link + game_name + "/" + id + "/";
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getInstallerLink)" << std::endl << root << std::endl;
            #endif
            int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
            if (available)
                link = root["file"]["link"].asString();
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getInstallerLink)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
    }

    return link;
}

std::string API::getExtraLink(const std::string& game_name, const std::string& id)
{
    std::string url, link;
    url = this->config.get_extra_link + game_name + "/" + id + "/";
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getExtraLink)" << std::endl << root << std::endl;
            #endif
            int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
            if (available)
                link = root["file"]["link"].asString();
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getExtraLink)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
    }

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

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getXML)" << std::endl << root << std::endl;
            #endif
            int available = root["file"]["available"].isInt() ? root["file"]["available"].asInt() : std::stoi(root["file"]["available"].asString());
            if (available)
            {
                url = root["file"]["link"].asString();
                XML = this->getResponse(url);
            }
        }
        else
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (API::getXML)" << std::endl << json << std::endl;
            #endif
            this->setError(jsonparser->getFormatedErrorMessages());
        }
        delete jsonparser;
    }
    else
    {
        this->setError("Found nothing in " + url);
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
