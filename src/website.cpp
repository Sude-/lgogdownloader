/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "website.h"
#include "globalconstants.h"

#include <htmlcxx/html/ParserDom.h>
#include <boost/algorithm/string/case_conv.hpp>

Website::Website()
{
    this->retries = 0;

    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, Globals::globalConfig.curlConf.sUserAgent.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, Globals::globalConfig.curlConf.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEFILE, Globals::globalConfig.curlConf.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEJAR, Globals::globalConfig.curlConf.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, Globals::globalConfig.curlConf.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, Globals::globalConfig.curlConf.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, Globals::globalConfig.curlConf.iDownloadRate);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_TIME, 30);
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_LIMIT, 200);

    if (!Globals::globalConfig.curlConf.sCACertPath.empty())
        curl_easy_setopt(curlhandle, CURLOPT_CAINFO, Globals::globalConfig.curlConf.sCACertPath.c_str());
}

Website::~Website()
{
    curl_easy_cleanup(curlhandle);
}

size_t Website::writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp)
{
    std::ostringstream *stream = (std::ostringstream*)userp;
    size_t count = size * nmemb;
    stream->write(ptr, count);
    return count;
}

std::string Website::getResponse(const std::string& url)
{
    std::ostringstream memory;
    std::string response;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Website::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);

    CURLcode result;
    do
    {
        if (Globals::globalConfig.iWait > 0)
            usleep(Globals::globalConfig.iWait); // Delay the request by specified time
        result = curl_easy_perform(curlhandle);
        response = memory.str();
        memory.str(std::string());
    }
    while ((result != CURLE_OK) && response.empty() && (this->retries++ < Globals::globalConfig.iRetries));
    this->retries = 0; // reset retries counter

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
        if (result == CURLE_HTTP_RETURNED_ERROR)
        {
            long int response_code = 0;
            result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
            std::cout << "HTTP ERROR: ";
            if (result == CURLE_OK)
                std::cout << response_code << " (" << url << ")" << std::endl;
            else
                std::cout << "failed to get error code: " << curl_easy_strerror(result) << " (" << url << ")" << std::endl;
        }
        else if (result == CURLE_SSL_CACERT)
        {
            std::cout << "Try using CA certificate bundle from cURL: https://curl.haxx.se/ca/cacert.pem" << std::endl;
            std::cout << "Use --cacert to set the path for CA certificate bundle" << std::endl;
        }
    }

    return response;
}

Json::Value Website::getGameDetailsJSON(const std::string& gameid)
{
    std::string gameDataUrl = "https://www.gog.com/account/gameDetails/" + gameid + ".json";
    std::string json = this->getResponse(gameDataUrl);

    // Parse JSON
    Json::Value root;
    std::istringstream json_stream(json);

    try {
        json_stream >> root;
    } catch(const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Website::getGameDetailsJSON)" << std::endl << json << std::endl;
        #endif
        std::cout << exc.what();
        exit(1);
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Website::getGameDetailsJSON)" << std::endl << root << std::endl;
    #endif

    return root;
}

// Get list of games from account page
std::vector<gameItem> Website::getGames()
{
    std::vector<gameItem> games;
    Json::Value root;
    int i = 1;
    bool bAllPagesParsed = false;

    do
    {
        std::string response = this->getResponse("https://www.gog.com/account/getFilteredProducts?hasHiddenProducts=false&hiddenFlag=0&isUpdated=0&mediaType=1&sortBy=title&system=&page=" + std::to_string(i));
        std::istringstream json_stream(response);

        try {
            // Parse JSON
            json_stream >> root;
        } catch (const Json::Exception& exc) {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (Website::getGames)" << std::endl << response << std::endl;
            #endif
            std::cout << exc.what();
            if (!response.empty())
            {
                if(response[0] != '{')
                {
                    // Response was not JSON. Assume that cookies have expired.
                    std::cerr << "Response was not JSON. Cookies have most likely expired. Try --login first." << std::endl;
                }
            }
            exit(1);
        }
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Website::getGames)" << std::endl << root << std::endl;
        #endif
        if (root["page"].asInt() == root["totalPages"].asInt())
            bAllPagesParsed = true;
        if (root["products"].isArray())
        {
            for (unsigned int i = 0; i < root["products"].size(); ++i)
            {
                std::cerr << "\033[KGetting game names " << "(" << root["page"].asInt() << "/" << root["totalPages"].asInt() << ") " << i+1 << " / " << root["products"].size() << "\r" << std::flush;
                Json::Value product = root["products"][i];
                gameItem game;
                game.name = product["slug"].asString();
                game.id = product["id"].isInt() ? std::to_string(product["id"].asInt()) : product["id"].asString();

                if (product.isMember("updates"))
                {
                    if (product["updates"].isNull())
                    {
                        /* In some cases the value can be null.
                         * For example when user owns a dlc but not the base game
                         * https://github.com/Sude-/lgogdownloader/issues/101
                         * Assume that there are no updates in this case */
                        game.updates = 0;
                    }
                    else if (product["updates"].isInt())
                        game.updates = product["updates"].asInt();
                    else
                    {
                        try
                        {
                            game.updates = std::stoi(product["updates"].asString());
                        }
                        catch (std::invalid_argument& e)
                        {
                            game.updates = 0; // Assume no updates
                        }
                    }
                }

                unsigned int platform = 0;
                if (product["worksOn"]["Windows"].asBool())
                    platform |= GlobalConstants::PLATFORM_WINDOWS;
                if (product["worksOn"]["Mac"].asBool())
                    platform |= GlobalConstants::PLATFORM_MAC;
                if (product["worksOn"]["Linux"].asBool())
                    platform |= GlobalConstants::PLATFORM_LINUX;

                // Skip if platform doesn't match
                if (Globals::globalConfig.bPlatformDetection && !(platform & Globals::globalConfig.dlConf.iInstallerPlatform))
                    continue;

                // Filter the game list
                if (!Globals::globalConfig.sGameRegex.empty())
                {
                    // GameRegex filter aliases
                    if (Globals::globalConfig.sGameRegex == "all")
                        Globals::globalConfig.sGameRegex = ".*";

                    boost::regex expression(Globals::globalConfig.sGameRegex);
                    boost::match_results<std::string::const_iterator> what;
                    if (!boost::regex_search(game.name, what, expression)) // Check if name matches the specified regex
                        continue;
                }

                if (Globals::globalConfig.dlConf.bDLC)
                {
                    int dlcCount = product["dlcCount"].asInt();

                    bool bDownloadDLCInfo = (dlcCount != 0);

                    if (!bDownloadDLCInfo && !Globals::globalConfig.sIgnoreDLCCountRegex.empty())
                    {
                        boost::regex expression(Globals::globalConfig.sIgnoreDLCCountRegex);
                        boost::match_results<std::string::const_iterator> what;
                        if (boost::regex_search(game.name, what, expression)) // Check if name matches the specified regex
                        {
                            bDownloadDLCInfo = true;
                        }
                    }

                    if (!bDownloadDLCInfo && !Globals::globalConfig.gamehasdlc.empty())
                    {
                        if (Globals::globalConfig.gamehasdlc.isBlacklisted(game.name))
                            bDownloadDLCInfo = true;
                    }

                    // Check game specific config
                    if (!Globals::globalConfig.bUpdateCache) // Disable game specific config files for cache update
                    {
                        gameSpecificConfig conf;
                        conf.dlConf.bIgnoreDLCCount = bDownloadDLCInfo;
                        Util::getGameSpecificConfig(game.name, &conf);
                        bDownloadDLCInfo = conf.dlConf.bIgnoreDLCCount;
                    }

                    if (bDownloadDLCInfo && !Globals::globalConfig.sGameRegex.empty())
                    {
                        // don't download unnecessary info if user is only interested in a subset of his account
                        boost::regex expression(Globals::globalConfig.sGameRegex);
                        boost::match_results<std::string::const_iterator> what;
                        if (!boost::regex_search(game.name, what, expression))
                        {
                            bDownloadDLCInfo = false;
                        }
                    }

                    if (bDownloadDLCInfo)
                    {
                        game.gamedetailsjson = this->getGameDetailsJSON(game.id);
                        if (!game.gamedetailsjson.empty())
                            game.dlcnames = Util::getDLCNamesFromJSON(game.gamedetailsjson["dlcs"]);
                    }
                }
                games.push_back(game);
            }
        }
        i++;
    } while (!bAllPagesParsed);
    std::cerr << std::endl;

    return games;
}

// Get list of free games
std::vector<gameItem> Website::getFreeGames()
{
    Json::Value root;
    std::vector<gameItem> games;
    std::string json = this->getResponse("https://www.gog.com/games/ajax/filtered?mediaType=game&page=1&price=free&sort=title");
    std::istringstream json_stream(json);

    try {
        // Parse JSON
        json_stream >> root;
    } catch (const Json::Exception& exc) {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Website::getFreeGames)" << std::endl << json << std::endl;
        #endif
        std::cout << exc.what();
        exit(1);
    }
    
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Website::getFreeGames)" << std::endl << root << std::endl;
    #endif

    Json::Value products = root["products"];
    for (unsigned int i = 0; i < products.size(); ++i)
    {
        gameItem game;
        game.name = products[i]["slug"].asString();
        game.id = products[i]["id"].isInt() ? std::to_string(products[i]["id"].asInt()) : products[i]["id"].asString();
        games.push_back(game);
    }

    return games;
}

// Login to GOG website
int Website::Login(const std::string& email, const std::string& password)
{
    int res = 0;
    std::string postdata;
    std::ostringstream memory;
    std::string token;
    std::string tagname_username = "login[username]";
    std::string tagname_password = "login[password]";
    std::string tagname_login = "login[login]";
    std::string tagname_token;
    std::string auth_url = "https://auth.gog.com/auth?client_id=" + Globals::galaxyConf.getClientId() + "&redirect_uri=" + (std::string)curl_easy_escape(curlhandle, Globals::galaxyConf.getRedirectUri().c_str(), Globals::galaxyConf.getRedirectUri().size()) + "&response_type=code&layout=default&brand=gog";
    std::string auth_code;

    std::string login_form_html = this->getResponse(auth_url);
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Website::Login)" << std::endl;
        std::cerr << login_form_html << std::endl;
    #endif
    if (login_form_html.find("google.com/recaptcha") != std::string::npos)
    {
        std::cout   << "Login form contains reCAPTCHA (https://www.google.com/recaptcha/)" << std::endl
                    << "Try to login later" << std::endl;
        return res = 0;
    }

    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> login_dom = parser.parseTree(login_form_html);
    tree<htmlcxx::HTML::Node>::iterator login_it = login_dom.begin();
    tree<htmlcxx::HTML::Node>::iterator login_it_end = login_dom.end();
    for (; login_it != login_it_end; ++login_it)
    {
        if (login_it->tagName()=="input")
        {
            login_it->parseAttributes();
            if (login_it->attribute("id").second == "login__token")
            {
                token = login_it->attribute("value").second; // login token
                tagname_token = login_it->attribute("name").second;
            }
        }
    }

    if (token.empty())
    {
        std::cout << "Failed to get login token" << std::endl;
        return res = 0;
    }

    //Create postdata - escape characters in email/password to support special characters
    postdata = (std::string)curl_easy_escape(curlhandle, tagname_username.c_str(), tagname_username.size()) + "=" + (std::string)curl_easy_escape(curlhandle, email.c_str(), email.size())
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_password.c_str(), tagname_password.size()) + "=" + (std::string)curl_easy_escape(curlhandle, password.c_str(), password.size())
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_login.c_str(), tagname_login.size()) + "="
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_token.c_str(), tagname_token.size()) + "=" + (std::string)curl_easy_escape(curlhandle, token.c_str(), token.size());
    curl_easy_setopt(curlhandle, CURLOPT_URL, "https://login.gog.com/login_check");
    curl_easy_setopt(curlhandle, CURLOPT_POST, 1);
    curl_easy_setopt(curlhandle, CURLOPT_POSTFIELDS, postdata.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Website::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

    // Don't follow to redirect location because we need to check it for two step authorization.
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 0);
    CURLcode result = curl_easy_perform(curlhandle);
    memory.str(std::string());

    if (result != CURLE_OK)
    {
        // Expected to hit maximum amount of redirects so don't print error on it
        if (result != CURLE_TOO_MANY_REDIRECTS)
            std::cout << curl_easy_strerror(result) << std::endl;
    }

    // Get redirect url
    char *redirect_url;
    curl_easy_getinfo(curlhandle, CURLINFO_REDIRECT_URL, &redirect_url);

    // Handle two step authorization
    if (std::string(redirect_url).find("two_step") != std::string::npos)
    {
        std::string security_code;
        std::string tagname_two_step_send = "second_step_authentication[send]";
        std::string tagname_two_step_auth_letter_1 = "second_step_authentication[token][letter_1]";
        std::string tagname_two_step_auth_letter_2 = "second_step_authentication[token][letter_2]";
        std::string tagname_two_step_auth_letter_3 = "second_step_authentication[token][letter_3]";
        std::string tagname_two_step_auth_letter_4 = "second_step_authentication[token][letter_4]";
        std::string tagname_two_step_token;
        std::string token_two_step;
        std::string two_step_html = this->getResponse(redirect_url);
        redirect_url = NULL;

        tree<htmlcxx::HTML::Node> two_step_dom = parser.parseTree(two_step_html);
        tree<htmlcxx::HTML::Node>::iterator two_step_it = two_step_dom.begin();
        tree<htmlcxx::HTML::Node>::iterator two_step_it_end = two_step_dom.end();
        for (; two_step_it != two_step_it_end; ++two_step_it)
        {
            if (two_step_it->tagName()=="input")
            {
                two_step_it->parseAttributes();
                if (two_step_it->attribute("id").second == "second_step_authentication__token")
                {
                    token_two_step = two_step_it->attribute("value").second; // two step token
                    tagname_two_step_token = two_step_it->attribute("name").second;
                }
            }
        }

        std::cerr << "Security code: ";
        std::getline(std::cin,security_code);
        if (security_code.size() != 4)
        {
            std::cerr << "Security code must be 4 characters long" << std::endl;
            exit(1);
        }

        postdata = (std::string)curl_easy_escape(curlhandle, tagname_two_step_auth_letter_1.c_str(), tagname_two_step_auth_letter_1.size()) + "=" + security_code[0]
                + "&" + (std::string)curl_easy_escape(curlhandle, tagname_two_step_auth_letter_2.c_str(), tagname_two_step_auth_letter_2.size()) + "=" + security_code[1]
                + "&" + (std::string)curl_easy_escape(curlhandle, tagname_two_step_auth_letter_3.c_str(), tagname_two_step_auth_letter_3.size()) + "=" + security_code[2]
                + "&" + (std::string)curl_easy_escape(curlhandle, tagname_two_step_auth_letter_4.c_str(), tagname_two_step_auth_letter_4.size()) + "=" + security_code[3]
                + "&" + (std::string)curl_easy_escape(curlhandle, tagname_two_step_send.c_str(), tagname_two_step_send.size()) + "="
                + "&" + (std::string)curl_easy_escape(curlhandle, tagname_two_step_token.c_str(), tagname_two_step_token.size()) + "=" + (std::string)curl_easy_escape(curlhandle, token_two_step.c_str(), token_two_step.size());

        curl_easy_setopt(curlhandle, CURLOPT_URL, "https://login.gog.com/login/two_step");
        curl_easy_setopt(curlhandle, CURLOPT_POST, 1);
        curl_easy_setopt(curlhandle, CURLOPT_POSTFIELDS, postdata.c_str());
        curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Website::writeMemoryCallback);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
        curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 0);
        curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

        // Don't follow to redirect location because it doesn't work properly. Must clean up the redirect url first.
        curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 0);
        result = curl_easy_perform(curlhandle);
        memory.str(std::string());
        curl_easy_getinfo(curlhandle, CURLINFO_REDIRECT_URL, &redirect_url);
    }

    if (!std::string(redirect_url).empty())
    {
        long response_code;
        do
        {
            curl_easy_setopt(curlhandle, CURLOPT_URL, redirect_url);
            result = curl_easy_perform(curlhandle);
            memory.str(std::string());

            result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
            if ((response_code / 100) == 3)
                curl_easy_getinfo(curlhandle, CURLINFO_REDIRECT_URL, &redirect_url);

            std::string redir_url = std::string(redirect_url);
            boost::regex re(".*code=(.*?)([\?&].*|$)", boost::regex_constants::icase);
            boost::match_results<std::string::const_iterator> what;
            if (boost::regex_search(redir_url, what, re))
            {
                auth_code = what[1];
                if (!auth_code.empty())
                    break;
            }
        } while (result == CURLE_OK && (response_code / 100) == 3);
    }

    curl_easy_setopt(curlhandle, CURLOPT_URL, redirect_url);
    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    result = curl_easy_perform(curlhandle);

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
    }

    if (this->IsLoggedInComplex(email))
    {
        res = 1; // Login was successful
    }
    else
    {
        if (this->IsloggedInSimple())
            res = 1; // Login was successful
    }

    if (res == 1 && !auth_code.empty())
    {
        std::string token_url = "https://auth.gog.com/token?client_id=" + Globals::galaxyConf.getClientId()
                            + "&client_secret=" + Globals::galaxyConf.getClientSecret()
                            + "&grant_type=authorization_code&code=" + auth_code
                            + "&redirect_uri=" + (std::string)curl_easy_escape(curlhandle, Globals::galaxyConf.getRedirectUri().c_str(), Globals::galaxyConf.getRedirectUri().size());

        std::string json = this->getResponse(token_url);
        if (!json.empty())
        {
            Json::Value token_json;
			std::istringstream json_stream(json);
            try {
                json_stream >> token_json;

                Globals::galaxyConf.setJSON(token_json);
                res = 2;
            } catch (const Json::Exception& exc) {
                std::cerr << "Failed to parse json" << std::endl << json << std::endl;
                std::cerr << exc.what() << std::endl;
            }
        }
    }

    if (res >= 1)
        curl_easy_setopt(curlhandle, CURLOPT_COOKIELIST, "FLUSH"); // Write all known cookies to the file specified by CURLOPT_COOKIEJAR

    return res;
}

bool Website::IsLoggedIn()
{
    return this->IsloggedInSimple();
}

/* Complex login check. Check login by checking email address on the account settings page.
    returns true if we are logged in
    returns false if we are not logged in
*/
bool Website::IsLoggedInComplex(const std::string& email)
{
    bool bIsLoggedIn = false;
    std::string html = this->getResponse("https://www.gog.com/account/settings/security");
    std::string email_lowercase = boost::algorithm::to_lower_copy(email); // boost::algorithm::to_lower does in-place modification but "email" is read-only so we need to make a copy of it

    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    dom = parser.parseTree(html);
    it = dom.begin();
    end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="strong")
        {
            it->parseAttributes();
            if (it->attribute("class").second == "settings-item__value settings-item__section")
            {
                for (unsigned int i = 0; i < dom.number_of_children(it); ++i)
                {
                    tree<htmlcxx::HTML::Node>::iterator tag_it = dom.child(it, i);
                    if (!tag_it->isTag() && !tag_it->isComment())
                    {
                        std::string tag_text = boost::algorithm::to_lower_copy(tag_it->text());
                        if (tag_text == email_lowercase)
                        {
                            bIsLoggedIn = true; // We are logged in
                            break;
                        }
                    }
                }
            }
        }
        if (bIsLoggedIn) // We are logged in so no need to go through the remaining tags
            break;
    }

    return bIsLoggedIn;
}

/* Simple login check. Check login by trying to get account page. If response code isn't 200 then login failed.
    returns true if we are logged in
    returns false if we are not logged in
*/
bool Website::IsloggedInSimple()
{
    bool bIsLoggedIn = false;
    std::ostringstream memory;
    std::string url = "https://www.gog.com/account";
    long int response_code = 0;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 0);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Website::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_perform(curlhandle);
    memory.str(std::string());

    curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    if (response_code == 200)
        bIsLoggedIn = true; // We are logged in

    return bIsLoggedIn;
}

std::vector<wishlistItem> Website::getWishlistItems()
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    int i = 1;
    bool bAllPagesParsed = false;
    std::vector<wishlistItem> wishlistItems;

    do
    {
        std::string response(this->getResponse("https://www.gog.com/account/wishlist/search?hasHiddenProducts=false&hiddenFlag=0&isUpdated=0&mediaType=0&sortBy=title&system=&page=" + std::to_string(i)));
        std::istringstream response_stream(response);

        try {
            // Parse JSON
            response_stream >> root;
        } catch(const Json::Exception exc) {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (Website::getWishlistItems)" << std::endl << response << std::endl;
            #endif
            std::cout << exc.what();
            exit(1);
        }
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Website::getWishlistItems)" << std::endl << root << std::endl;
        #endif
        if (root["page"].asInt() >= root["totalPages"].asInt())
            bAllPagesParsed = true;
        if (root["products"].isArray())
        {
            for (unsigned int i = 0; i < root["products"].size(); ++i)
            {
                wishlistItem item;
                Json::Value product = root["products"][i];

                item.platform = 0;
                std::string platforms_text;
                bool bIsMovie = product["isMovie"].asBool();
                if (!bIsMovie)
                {
                    if (product["worksOn"]["Windows"].asBool())
                        item.platform |= GlobalConstants::PLATFORM_WINDOWS;
                    if (product["worksOn"]["Mac"].asBool())
                        item.platform |= GlobalConstants::PLATFORM_MAC;
                    if (product["worksOn"]["Linux"].asBool())
                        item.platform |= GlobalConstants::PLATFORM_LINUX;

                    // Skip if platform doesn't match
                    if (Globals::globalConfig.bPlatformDetection && !(item.platform & Globals::globalConfig.dlConf.iInstallerPlatform))
                        continue;
                }

                if (product["isComingSoon"].asBool())
                    item.tags.push_back("Coming soon");
                if (product["isDiscounted"].asBool())
                    item.tags.push_back("Discount");
                if (bIsMovie)
                    item.tags.push_back("Movie");

                item.release_date_time = 0;
                if (product.isMember("releaseDate") && product["isComingSoon"].asBool())
                {
                    if (!product["releaseDate"].empty())
                    {
                        if (product["releaseDate"].isInt())
                        {
                            item.release_date_time = product["releaseDate"].asInt();
                        }
                        else
                        {
                            std::string release_date_time_string = product["releaseDate"].asString();
                            if (!release_date_time_string.empty())
                            {
                                try
                                {
                                    item.release_date_time = std::stoi(release_date_time_string);
                                }
                                catch (std::invalid_argument& e)
                                {
                                    item.release_date_time = 0;
                                }
                            }
                        }
                    }
                }

                item.currency = product["price"]["symbol"].asString();
                item.price = product["price"]["finalAmount"].isDouble() ? std::to_string(product["price"]["finalAmount"].asDouble()) + item.currency : product["price"]["finalAmount"].asString() + item.currency;
                item.discount_percent = product["price"]["discountPercentage"].isInt() ? std::to_string(product["price"]["discountPercentage"].asInt()) + "%" : product["price"]["discountPercentage"].asString() + "%";
                item.discount = product["price"]["discountDifference"].isDouble() ? std::to_string(product["price"]["discountDifference"].asDouble()) + item.currency : product["price"]["discountDifference"].asString() + item.currency;
                item.store_credit = product["price"]["bonusStoreCreditAmount"].isDouble() ? std::to_string(product["price"]["bonusStoreCreditAmount"].asDouble()) + item.currency : product["price"]["bonusStoreCreditAmount"].asString() + item.currency;

                item.url = product["url"].asString();
                if (item.url.find("/game/") == 0)
                    item.url = "https://www.gog.com" + item.url;
                else if (item.url.find("/movie/") == 0)
                    item.url = "https://www.gog.com" + item.url;

                item.title = product["title"].asString();
                item.bIsBonusStoreCreditIncluded = product["price"]["isBonusStoreCreditIncluded"].asBool();
                item.bIsDiscounted = product["isDiscounted"].asBool();

                wishlistItems.push_back(item);
            }
        }
        i++;
    } while (!bAllPagesParsed);

    return wishlistItems;
}
