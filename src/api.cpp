/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

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

gameFile::gameFile(const bool& t_updated, const std::string& t_id, const std::string& t_name, const std::string& t_path, const std::string& t_size, const unsigned int& t_language)
{
    this->updated = t_updated;
    this->id = t_id;
    this->name = t_name;
    this->path = t_path;
    this->size = t_size;
    this->language = t_language;
}

gameFile::~gameFile()
{

}

API::API(const std::string& token, const std::string& secret, const bool& verbose)
{
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, verbose);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, 10);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, 0);

    this->error = false;
    this->getAPIConfig();
    this->config.oauth_token = token;
    this->config.oauth_secret = secret;
}

int API::init()
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
    std::string url = "https://api.gog.com/en/downloader2/status/stable/"; // Stable API
    //std::string url = "https://api.gog.com/en/downloader2/status/beta/"; // Beta API
    //std::string url = "https://api.gog.com/en/downloader2/status/e77989ed21758e78331b20e477fc5582/"; // Development API? Not sure because the downloader version number it reports is lower than beta.
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
            this->config.oauth_authorize_temp_token = root["config"]["oauth_authorize_temp_token"].asString();
            this->config.oauth_get_temp_token = root["config"]["oauth_get_temp_token"].asString();
            this->config.oauth_get_token = root["config"]["oauth_get_token"].asString();
            this->config.get_user_games = root["config"]["get_user_games"].asString();
            this->config.get_user_details = root["config"]["get_user_details"].asString();
            this->config.get_installer_link = root["config"]["get_installer_link"].asString();
            this->config.get_game_details = root["config"]["get_game_details"].asString();
            this->config.get_extra_link = root["config"]["get_extra_link"].asString();
            this->config.set_app_status = root["config"]["set_app_status"].asString();
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
    url = oauth_sign_url2(this->config.oauth_get_temp_token.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY, CONSUMER_SECRET, NULL /* token */, NULL /* secret */);

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
    url = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY, CONSUMER_SECRET, token.c_str(), secret.c_str());
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
    url = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY, CONSUMER_SECRET, token.c_str(), secret.c_str());
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
    std::string url_oauth = oauth_sign_url2(url.c_str(), NULL, OA_HMAC, NULL, CONSUMER_KEY, CONSUMER_SECRET, this->config.oauth_token.c_str(), this->config.oauth_secret.c_str());
    std::string response = this->getResponse(url_oauth);

    return response;
}

gameDetails API::getGameDetails(const std::string& game_name, const unsigned int& type, const unsigned int& lang)
{
    std::string url;
    gameDetails game;

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

            // Installer details
            std::vector<std::pair<Json::Value,unsigned int>> installers;
            if (type & INSTALLER_WINDOWS)
            {
                if (lang & LANGUAGE_EN)
                    installers.push_back(std::make_pair(root["game"]["installer_win_en"],LANGUAGE_EN));
                if (lang & LANGUAGE_DE)
                    installers.push_back(std::make_pair(root["game"]["installer_win_de"],LANGUAGE_DE));
                if (lang & LANGUAGE_FR)
                    installers.push_back(std::make_pair(root["game"]["installer_win_fr"],LANGUAGE_FR));
                if (lang & LANGUAGE_PL)
                    installers.push_back(std::make_pair(root["game"]["installer_win_pl"],LANGUAGE_PL));
                if (lang & LANGUAGE_RU)
                    installers.push_back(std::make_pair(root["game"]["installer_win_ru"],LANGUAGE_RU));
                if (lang & LANGUAGE_CN)
                    installers.push_back(std::make_pair(root["game"]["installer_win_cn"],LANGUAGE_CN));
                if (lang & LANGUAGE_CZ)
                    installers.push_back(std::make_pair(root["game"]["installer_win_cz"],LANGUAGE_CZ));
                if (lang & LANGUAGE_ES)
                    installers.push_back(std::make_pair(root["game"]["installer_win_es"],LANGUAGE_ES));
                if (lang & LANGUAGE_HU)
                    installers.push_back(std::make_pair(root["game"]["installer_win_hu"],LANGUAGE_HU));
                if (lang & LANGUAGE_IT)
                    installers.push_back(std::make_pair(root["game"]["installer_win_it"],LANGUAGE_IT));
                if (lang & LANGUAGE_JP)
                    installers.push_back(std::make_pair(root["game"]["installer_win_jp"],LANGUAGE_JP));
                if (lang & LANGUAGE_TR)
                    installers.push_back(std::make_pair(root["game"]["installer_win_tr"],LANGUAGE_TR));
            }
            if (type & INSTALLER_MAC)
            {
                if (lang & LANGUAGE_EN)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_en"],LANGUAGE_EN));
                if (lang & LANGUAGE_DE)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_de"],LANGUAGE_DE));
                if (lang & LANGUAGE_FR)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_fr"],LANGUAGE_FR));
                if (lang & LANGUAGE_PL)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_pl"],LANGUAGE_PL));
                if (lang & LANGUAGE_RU)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_ru"],LANGUAGE_RU));
                if (lang & LANGUAGE_CN)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_cn"],LANGUAGE_CN));
                if (lang & LANGUAGE_CZ)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_cz"],LANGUAGE_CZ));
                if (lang & LANGUAGE_ES)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_es"],LANGUAGE_ES));
                if (lang & LANGUAGE_HU)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_hu"],LANGUAGE_HU));
                if (lang & LANGUAGE_IT)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_it"],LANGUAGE_IT));
                if (lang & LANGUAGE_JP)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_jp"],LANGUAGE_JP));
                if (lang & LANGUAGE_TR)
                    installers.push_back(std::make_pair(root["game"]["installer_mac_tr"],LANGUAGE_TR));
            }
            for ( unsigned int i = 0; i < installers.size(); ++i )
            {
                for ( unsigned int index = 0; index < installers[i].first.size(); ++index )
                {
                    Json::Value installer = installers[i].first[index];
                    unsigned int language = installers[i].second;

                    game.installers.push_back(
                                                gameFile(   installer["#updated"].isBool() ? installer["#updated"].asBool() : false,
                                                            installer["id"].isInt() ? std::to_string(installer["id"].asInt()) : installer["id"].asString(),
                                                            installer["#name"].asString(),
                                                            installer["link"].asString(),
                                                            installer["size"].asString(),
                                                            language
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
    std::stringstream ss;
    ss << this->config.get_installer_link << game_name << "/" << id << "/";
    url = ss.str();
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            /*
            std::string result = root["result"].asString();
            int timestamp = root["timestamp"].asInt();
            int available = root["file"]["available"].asInt();
            std::string link = root["file"]["link"].asString();
            std::string message = root["file"]["message"].asString();
            */
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
    std::stringstream ss;
    ss << this->config.get_extra_link << game_name << "/" << id << "/";
    url = ss.str();
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            /*
            std::string result = root["result"].asString();
            int timestamp = root["timestamp"].asInt();
            int available = root["file"]["available"].asInt();
            std::string link = root["file"]["link"].asString();
            std::string type = root["file"]["type"].asString();
            std::string name = root["file"]["name"].asString();
            std::string message = root["file"]["message"].asString();
            */
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


std::string API::getXML(const std::string& game_name, const std::string& id)
{
    std::string url, XML;
    std::stringstream ss;
    ss << this->config.get_installer_link << game_name << "/" << id << "/crc/";
    url = ss.str();
    std::string json = this->getResponseOAuth(url);

    if (!json.empty())
    {
        Json::Value root;
        Json::Reader *jsonparser = new Json::Reader;
        if (jsonparser->parse(json, root))
        {
            /*
            std::string result = root["result"].asString();
            int timestamp = root["timestamp"].asInt();
            int available = root["file"]["available"].asInt();
            std::string link = root["file"]["link"].asString();
            std::string message = root["file"]["message"].asString();
            */
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
