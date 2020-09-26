/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef WEBSITE_H
#define WEBSITE_H

#include "config.h"
#include "util.h"
#include "globals.h"
#include <curl/curl.h>
#include <json/json.h>
#include <fstream>

class Website
{
    public:
        Website();
        int Login(const std::string& email, const std::string& password);
        std::string getResponse(const std::string& url);
        Json::Value getGameDetailsJSON(const std::string& gameid);
        std::vector<gameItem> getGames();
        std::vector<wishlistItem> getWishlistItems();
        bool IsLoggedIn();
        virtual ~Website();
    protected:
    private:
        CURL* curlhandle;
        bool IsloggedInSimple();
        bool IsLoggedInComplex(const std::string& email);
        int retries;
};

#endif // WEBSITE_H
