/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef API_H
#define API_H

#include "globalconstants.h"

#include <iostream>
#include <vector>
#include <curl/curl.h>
extern "C" {
    #include <oauth.h>
}
#include <cstring>

class gameFile {
    public:
        gameFile(const bool& t_updated, const std::string& t_id, const std::string& t_name, const std::string& t_path, const std::string& t_size, const unsigned int& t_language = GlobalConstants::LANGUAGE_EN);
        bool updated;
        std::string id;
        std::string name;
        std::string path;
        std::string size;
        unsigned int language;
        virtual ~gameFile();
};

class gameDetails {
    public:
        std::vector<gameFile> extras;
        std::vector<gameFile> installers;
        std::vector<gameFile> patches;
        std::string gamename;
        std::string title;
        std::string icon;
};

class userDetails {
    public:
        std::string avatar_small;
        std::string avatar_big;
        std::string username;
        std::string email;
        unsigned long long id;
        int notifications_forum;
        int notifications_games;
        int notifications_messages;
};

class apiConfig {
    public:
        std::string oauth_authorize_temp_token;
        std::string oauth_get_temp_token;
        std::string oauth_get_token;
        std::string get_user_games;
        std::string get_user_details;
        std::string get_installer_link;
        std::string get_game_details;
        std::string get_extra_link;
        std::string set_app_status;
        std::string oauth_token;
        std::string oauth_secret;
};

size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp);

class API
{
    public:
        userDetails user;

        API(const std::string& token,const std::string& secret);
        int init();
        int login(const std::string& email, const std::string& password);
        int getAPIConfig();
        std::string getResponse(const std::string& url);
        std::string getResponseOAuth(const std::string& url);
        int getUserDetails();
        int getGames();
        gameDetails getGameDetails(const std::string& game_name, const unsigned int& type = GlobalConstants::PLATFORM_WINDOWS, const unsigned int& lang = GlobalConstants::LANGUAGE_EN);
        std::string getInstallerLink(const std::string& game_name, const std::string& id);
        std::string getExtraLink(const std::string& game_name, const std::string& id);
        std::string getPatchLink(const std::string& game_name, const std::string& id);
        std::string getXML(const std::string& game_name, const std::string& id);
        void clearError();
        bool getError() { return this->error; };
        std::string getErrorMessage() { return this->error_message; };
        std::string getToken() { return this->config.oauth_token; };
        std::string getSecret() { return this->config.oauth_secret; };
        template <typename T> CURLcode curlSetOpt(CURLoption option, T value) { return curl_easy_setopt(this->curlhandle, option, value); };
        virtual ~API();
    protected:
    private:
        apiConfig config;
        CURL* curlhandle;
        void setError(const std::string& err);
        bool error;
        std::string error_message;

        // API constants
        const std::string CONSUMER_KEY = "1f444d14ea8ec776585524a33f6ecc1c413ed4a5";
        const std::string CONSUMER_SECRET = "20d175147f9db9a10fc0584aa128090217b9cf88";
        const int OAUTH_VERIFIER_LENGTH = 14;
        const int OAUTH_TOKEN_LENGTH = 11;
        const int OAUTH_SECRET_LENGTH = 18;
};

#endif // API_H
