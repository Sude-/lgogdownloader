/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "website.h"
#include "globalconstants.h"
#include "message.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <tinyxml2.h>

#ifdef USE_QT_GUI_LOGIN
    #include "gui_login.h"
#endif

Website::Website()
{
    this->retries = 0;

    curlhandle = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(curlhandle, Globals::globalConfig.curlConf);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEJAR, Globals::globalConfig.curlConf.sCookiePath.c_str());
}

Website::~Website()
{
    curl_easy_cleanup(curlhandle);
}

std::string Website::getResponse(const std::string& url)
{
    std::string response;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    int max_retries = std::min(3, Globals::globalConfig.iRetries);

    CURLcode result = Util::CurlHandleGetResponse(curlhandle, response, max_retries);

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

Json::Value Website::getResponseJson(const std::string& url)
{
    std::istringstream response(this->getResponse(url));
    Json::Value json;

    if (!response.str().empty())
    {
        try
        {
            response >> json;
        }
        catch(const Json::Exception& exc)
        {
            if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_DEBUG)
                std::cerr << "DEBUG INFO (Website::getResponseJson)" << std::endl << json << std::endl;

            std::cout << "Failed to parse json: " << exc.what();
        }
    }

    return json;
}

Json::Value Website::getGameDetailsJSON(const std::string& gameid)
{
    std::string gameDataUrl = "https://www.gog.com/account/gameDetails/" + gameid + ".json";
    Json::Value json = this->getResponseJson(gameDataUrl);

    return json;
}

// Get list of games from account page
std::vector<gameItem> Website::getGames()
{
    std::vector<gameItem> games;
    Json::Value root;
    int i = 1;
    bool bAllPagesParsed = false;
    int iUpdated = Globals::globalConfig.bUpdated ? 1 : 0;
    int iHidden = 0;
    std::string tags;
    for (auto tag : Globals::globalConfig.dlConf.vTags)
    {
        if (tags.empty())
            tags = tag;
        else
            tags += "," + tag;
    }

    Globals::vOwnedGamesIds = this->getOwnedGamesIds();
    do
    {
        std::string url = "https://www.gog.com/account/getFilteredProducts?hiddenFlag=" + std::to_string(iHidden) + "&isUpdated=" + std::to_string(iUpdated) + "&mediaType=1&sortBy=title&system=&page=" + std::to_string(i);
        if (!tags.empty())
            url += "&tags=" + tags;

        Json::Value root = this->getResponseJson(url);
        if (root.empty())
            continue;

        if (root["page"].asInt() == root["totalPages"].asInt() || root["totalPages"].asInt() == 0)
            bAllPagesParsed = true;

        // Make the next loop handle hidden products
        if (Globals::globalConfig.bIncludeHiddenProducts && bAllPagesParsed && iHidden == 0)
        {
            bAllPagesParsed = false;
            iHidden = 1;
            i = 0; // Set to 0 because we increment it at the end of the loop
        }

        if (root["products"].isArray())
        {
            for (unsigned int i = 0; i < root["products"].size(); ++i)
            {
                std::cerr << "\033[KGetting game names " << "(" << root["page"].asInt() << "/" << root["totalPages"].asInt() << ") " << i+1 << " / " << root["products"].size() << "\r" << std::flush;
                Json::Value product = root["products"][i];
                gameItem game;
                game.name = product["slug"].asString();
                game.id = product["id"].isInt() ? std::to_string(product["id"].asInt()) : product["id"].asString();
                game.isnew = product["isNew"].asBool();

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

                // Skip if not new and flag is set
                if (Globals::globalConfig.bNew && !game.isnew)
                    continue;

                // Skip if platform doesn't match
                if (Globals::globalConfig.bPlatformDetection && !(platform & Globals::globalConfig.dlConf.iInstallerPlatform))
                    continue;

                // Filter the game list
                if (!Globals::globalConfig.sGameRegex.empty())
                {
                    boost::regex expression(Globals::globalConfig.sGameRegex);
                    boost::match_results<std::string::const_iterator> what;
                    if (!boost::regex_search(game.name, what, expression)) // Check if name matches the specified regex
                        continue;
                }

                if (Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC)
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

    if (Globals::globalConfig.bIncludeHiddenProducts)
    {
        std::sort(games.begin(), games.end(), [](const gameItem& i, const gameItem& j) -> bool { return i.name < j.name; });
    }

    return games;
}

// Login to GOG website
int Website::Login(const std::string& email, const std::string& password)
{
    // Reset client id and client secret to ensure we can log-in
    Globals::galaxyConf.resetClient();

    int res = 0;
    std::string auth_code;

    auth_code = this->LoginGetAuthCode(email, password);

    if (!auth_code.empty())
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
                res = 1;
            } catch (const Json::Exception& exc) {
                std::cerr << "Failed to parse json" << std::endl << json << std::endl;
                std::cerr << exc.what() << std::endl;
            }
        }
    }
    else
    {
        std::cout << "Failed to get auth code" << std::endl;
        res = 0;
    }

    if (res >= 1)
        curl_easy_setopt(curlhandle, CURLOPT_COOKIELIST, "FLUSH"); // Write all known cookies to the file specified by CURLOPT_COOKIEJAR

    return res;
}

std::string Website::LoginGetAuthCode(const std::string& email, const std::string& password)
{
    std::string auth_code;
    bool bRecaptcha = false;
    bool bForceGUI = false;
    #ifdef USE_QT_GUI_LOGIN
        bForceGUI = Globals::globalConfig.bForceGUILogin;
    #endif

    std::string auth_url = "https://auth.gog.com/auth?client_id=" + Globals::galaxyConf.getClientId() + "&redirect_uri=" + (std::string)curl_easy_escape(curlhandle, Globals::galaxyConf.getRedirectUri().c_str(), Globals::galaxyConf.getRedirectUri().size()) + "&response_type=code&layout=default&brand=gog";

    std::string login_form_html = this->getResponse(auth_url);
    if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_DEBUG)
    {
        std::cerr << "DEBUG INFO (Website::LoginGetAuthCode)" << std::endl;
        std::cerr << login_form_html << std::endl;
    }

    if (login_form_html.find("class=\"g-recaptcha form__recaptcha\"") != std::string::npos)
    {
        bRecaptcha = true;
    }

    // Try normal login if GUI is not forced
    if (!bForceGUI)
    {
        auth_code = this->LoginGetAuthCodeCurl(login_form_html, email, password);
    }

    #ifdef USE_QT_GUI_LOGIN
    if ((Globals::globalConfig.bEnableLoginGUI && auth_code.empty()) || bForceGUI)
    {
        auth_code = this->LoginGetAuthCodeGUI(email, password);

        // If GUI is forced then stop here and don't offer browser login
        if (bForceGUI)
            return auth_code;
    }
    #endif

    if (auth_code.empty() && bRecaptcha)
        auth_code = this->LoginGetAuthCodeBrowser(auth_url);

    return auth_code;
}

std::string Website::LoginGetAuthCodeCurl(const std::string& login_form_html, const std::string& email, const std::string& password)
{
    std::string auth_code;
    std::string postdata;
    std::ostringstream memory;
    std::string token;
    std::string tagname_username = "login[username]";
    std::string tagname_password = "login[password]";
    std::string tagname_login = "login[login]";
    std::string tagname_token = "login[_token]";

    std::string login_form_xhtml = Util::htmlToXhtml(login_form_html);

    tinyxml2::XMLDocument doc;
    doc.Parse(login_form_xhtml.c_str());
    tinyxml2::XMLNode* node = doc.FirstChildElement("html");
    while(node)
    {
        tinyxml2::XMLElement *element = node->ToElement();
        if (element->Name() && !std::string(element->Name()).compare("input"))
        {
            std::string name = element->Attribute("name");
            if (name == tagname_token)
            {
                token = element->Attribute("value");
                break;
            }
        }

        node = Util::nextXMLNode(node);
    }

    if (token.empty())
    {
        std::cout << "Failed to get login token" << std::endl;
        return std::string();
    }

    //Create postdata - escape characters in email/password to support special characters
    postdata = (std::string)curl_easy_escape(curlhandle, tagname_username.c_str(), tagname_username.size()) + "=" + (std::string)curl_easy_escape(curlhandle, email.c_str(), email.size())
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_password.c_str(), tagname_password.size()) + "=" + (std::string)curl_easy_escape(curlhandle, password.c_str(), password.size())
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_login.c_str(), tagname_login.size()) + "="
            + "&" + (std::string)curl_easy_escape(curlhandle, tagname_token.c_str(), tagname_token.size()) + "=" + (std::string)curl_easy_escape(curlhandle, token.c_str(), token.size());
    curl_easy_setopt(curlhandle, CURLOPT_URL, "https://login.gog.com/login_check");
    curl_easy_setopt(curlhandle, CURLOPT_POST, 1);
    curl_easy_setopt(curlhandle, CURLOPT_POSTFIELDS, postdata.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
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

    // Handle two step authorization
    std::string redirect_url = Util::CurlHandleGetInfoString(curlhandle, CURLINFO_REDIRECT_URL);
    if (redirect_url.find("two_step") != std::string::npos)
    {
        std::string security_code;
        std::string tagname_two_step_send = "second_step_authentication[send]";
        std::string tagname_two_step_auth_letter_1 = "second_step_authentication[token][letter_1]";
        std::string tagname_two_step_auth_letter_2 = "second_step_authentication[token][letter_2]";
        std::string tagname_two_step_auth_letter_3 = "second_step_authentication[token][letter_3]";
        std::string tagname_two_step_auth_letter_4 = "second_step_authentication[token][letter_4]";
        std::string tagname_two_step_token = "second_step_authentication[_token]";
        std::string token_two_step;
        std::string two_step_html = this->getResponse(redirect_url);
        redirect_url = "";

        std::string two_step_xhtml = Util::htmlToXhtml(two_step_html);
        doc.Parse(two_step_xhtml.c_str());
        node = doc.FirstChildElement("html");
        while(node)
        {
            tinyxml2::XMLElement *element = node->ToElement();
            if (element->Name() && !std::string(element->Name()).compare("input"))
            {
                std::string name = element->Attribute("name");
                if (name == tagname_two_step_token)
                {
                    token_two_step = element->Attribute("value");
                    break;
                }
            }

            node = Util::nextXMLNode(node);
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
        curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
        curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 0);
        curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

        // Don't follow to redirect location because it doesn't work properly. Must clean up the redirect url first.
        curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 0);
        result = curl_easy_perform(curlhandle);
        memory.str(std::string());
        redirect_url = Util::CurlHandleGetInfoString(curlhandle, CURLINFO_REDIRECT_URL);
    }

    if (!redirect_url.empty())
    {
        long response_code;
        do
        {
            curl_easy_setopt(curlhandle, CURLOPT_URL, redirect_url.c_str());
            result = curl_easy_perform(curlhandle);
            memory.str(std::string());

            result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
            if ((response_code / 100) == 3)
                redirect_url = Util::CurlHandleGetInfoString(curlhandle, CURLINFO_REDIRECT_URL);

            boost::regex re(".*code=(.*?)([\?&].*|$)", boost::regex_constants::icase);
            boost::match_results<std::string::const_iterator> what;
            if (boost::regex_search(redirect_url, what, re))
            {
                auth_code = what[1];
                if (!auth_code.empty())
                    break;
            }
        } while (result == CURLE_OK && (response_code / 100) == 3);
    }

    curl_easy_setopt(curlhandle, CURLOPT_URL, redirect_url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    result = curl_easy_perform(curlhandle);

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
    }

    return auth_code;
}

std::string Website::LoginGetAuthCodeBrowser(const std::string& auth_url)
{
    std::string auth_code;
    std::string url;

    std::cerr << "Login using browser at the following url" << std::endl;
    std::cerr << auth_url << std::endl << std::endl;
    std::cerr << "Copy & paste the full url from your browser here after login is complete" << std::endl;
    std::cerr << "URL: ";
    std::getline(std::cin, url);

    boost::regex re(".*code=(.*?)([\?&].*|$)", boost::regex_constants::icase);
    boost::match_results<std::string::const_iterator> what;
    if (boost::regex_search(url, what, re))
    {
        auth_code = what[1];
    }

    std::ostringstream memory;
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    CURLcode result = curl_easy_perform(curlhandle);
    memory.str(std::string());

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
    }

    return auth_code;
}

#ifdef USE_QT_GUI_LOGIN
std::string Website::LoginGetAuthCodeGUI(const std::string& email, const std::string& password)
{
    std::string auth_code;
    GuiLogin gl;
    gl.Login(email, password);

    auto cookies = gl.getCookies();
    for (auto cookie : cookies)
    {
        curl_easy_setopt(curlhandle, CURLOPT_COOKIELIST, cookie.c_str());
    }
    auth_code = gl.getCode();
    return auth_code;
}
#endif

bool Website::IsLoggedIn()
{
    return this->IsloggedInSimple();
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
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
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
        std::string url = "https://www.gog.com/account/wishlist/search?hasHiddenProducts=false&hiddenFlag=0&isUpdated=0&mediaType=0&sortBy=title&system=&page=" + std::to_string(i);
        Json::Value root = this->getResponseJson(url);
        if (root.empty())
            continue;

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

std::map<std::string, std::string> Website::getTags()
{
    std::string url = "https://www.gog.com/account/getFilteredProducts?mediaType=1&sortBy=title&system=&page=1";
    std::string response = this->getResponse(url);
    std::istringstream json_stream(response);
    Json::Value json;
    std::map<std::string, std::string> tags;

    try
    {
        // Parse JSON
        json_stream >> json;
    }
    catch (const Json::Exception& exc)
    {
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

    tags = this->getTagsFromJson(json["tags"]);

    return tags;
}

std::map<std::string, std::string> Website::getTagsFromJson(const Json::Value& json)
{
    std::map<std::string, std::string> tags;

    if (!json.empty())
    {
        for (auto node : json)
        {
            tags[node["id"].asString()] = node["name"].asString();
        }
    }

    return tags;
}

std::vector<std::string> Website::getOwnedGamesIds()
{
    std::vector<std::string> vOwnedGamesIds;
    Json::Value owned_json = this->getResponseJson("https://www.gog.com/user/data/games");

    if (owned_json.isMember("owned"))
    {
        for (auto id : owned_json["owned"])
            vOwnedGamesIds.push_back(id.asString());
    }

    return vOwnedGamesIds;
}
