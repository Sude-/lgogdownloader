/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "util.h"
#include "globalconstants.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <tinyxml.h>
#include <json/json.h>
#include <htmlcxx/html/ParserDom.h>
#include <htmlcxx/html/Uri.h>
#include <boost/algorithm/string/case_conv.hpp>

namespace bptime = boost::posix_time;

Downloader::Downloader(Config &conf)
{
    this->config = conf;
}

Downloader::~Downloader()
{
    if (config.bReport)
        if (this->report_ofs)
            this->report_ofs.close();
    delete progressbar;
    delete gogAPI;
    curl_easy_cleanup(curlhandle);
    curl_global_cleanup();
    // Make sure that cookie file is only readable/writable by owner
    Util::setFilePermissions(config.sCookiePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
}


/* Initialize the downloader
    returns 0 if successful
    returns 1 if failed
*/
int Downloader::init()
{
    this->resume_position = 0;
    this->retries = 0;

    // Initialize curl and set curl options
    curl_global_init(CURL_GLOBAL_ALL);
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, config.sVersionString.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, config.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEFILE, config.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEJAR, config.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, config.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSFUNCTION, Downloader::progressCallback);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, config.iDownloadRate);

    // Create new API handle and set curl options for the API
    gogAPI = new API(config.sToken, config.sSecret);
    gogAPI->curlSetOpt(CURLOPT_VERBOSE, config.bVerbose);
    gogAPI->curlSetOpt(CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    gogAPI->curlSetOpt(CURLOPT_CONNECTTIMEOUT, config.iTimeout);

    progressbar = new ProgressBar(config.bUnicode, config.bColor);

    bool bInitOK = gogAPI->init(); // Initialize the API
    if (!bInitOK)
        return 1;

    if (config.bCover && config.bDownload && !config.bUpdateCheck)
        coverXML = this->getResponse(config.sCoverList);

    // updateCheck() calls getGameList() if needed
    // getGameList() is not needed when using cache unless we want to list games from account
    if ( !config.bUpdateCheck && (!config.bUseCache || (config.bUseCache && config.bList)) )
        this->getGameList();

    if (config.bReport && (config.bDownload || config.bRepair))
    {
        this->report_ofs.open(config.sReportFilePath);
        if (!this->report_ofs)
        {
            std::cout << "Failed to create " << config.sReportFilePath << std::endl;
            return 1;
        }
    }

    return 0;
}

/* Login
    returns 0 if login fails
    returns 1 if successful
*/
int Downloader::login()
{
    char *pwd;
    std::string email;
    std::cout << "Email: ";
    std::getline(std::cin,email);
    pwd = getpass("Password: ");
    std::string password = (std::string)pwd;
    if (email.empty() || password.empty())
    {
        std::cout << "Email and/or password empty" << std::endl;
        return 0;
    }
    else
    {
        // Login to website
        if (!HTTP_Login(email, password))
        {
            std::cout << "HTTP: Login failed" << std::endl;
            return 0;
        }
        else
        {
            std::cout << "HTTP: Login successful" << std::endl;
        }
        // Login to API
        if (!gogAPI->login(email, password))
        {
            std::cout << "API: Login failed" << std::endl;
            return 0;
        }
        else
        {
            std::cout << "API: Login successful" << std::endl;
            config.sToken = gogAPI->getToken();
            config.sSecret = gogAPI->getSecret();
            return 1;
        }
    }
}

void Downloader::updateCheck()
{
    std::cout << "New forum replies: " << gogAPI->user.notifications_forum << std::endl;
    std::cout << "New private messages: " << gogAPI->user.notifications_messages << std::endl;
    std::cout << "Updated games: " << gogAPI->user.notifications_games << std::endl;

    if (gogAPI->user.notifications_games)
    {
        config.sGameRegex = ".*"; // Always check all games
        if (config.bList || config.bListDetails || config.bDownload)
        {
            if (config.bList)
                config.bListDetails = true; // Always list details
            this->getGameList();
            if (config.bDownload)
                this->download();
            else
                this->listGames();
        }
    }
}

void Downloader::getGameList()
{
    gameItems = this->getGames();

    // Filter the game list
    if (!config.sGameRegex.empty())
    {
        // GameRegex filter aliases
        if (config.sGameRegex == "all")
            config.sGameRegex = ".*";

        if (config.sGameRegex == "free")
        {
            gameItems = this->getFreeGames();
        }
        else
        {   // Filter the names
            std::vector<gameItem> gameItemsFiltered;
            boost::regex expression(config.sGameRegex);
            boost::match_results<std::string::const_iterator> what;
            for (unsigned int i = 0; i < gameItems.size(); ++i)
            {
                if (boost::regex_search(gameItems[i].name, what, expression)) // Check if name matches the specified regex
                    gameItemsFiltered.push_back(gameItems[i]);
            }
            gameItems = gameItemsFiltered;
        }
    }
}

/* Get detailed info about the games
    returns 0 if successful
    returns 1 if fails
*/
int Downloader::getGameDetails()
{
    if (config.bUseCache && !config.bUpdateCache)
    {
        // GameRegex filter alias for all games
        if (config.sGameRegex == "all")
            config.sGameRegex = ".*";
        else if (config.sGameRegex == "free")
            std::cout << "Warning: regex alias \"free\" doesn't work with cached details" << std::endl;

        int result = this->loadGameDetailsCache();
        if (result == 0)
        {
            for (unsigned int i = 0; i < this->games.size(); ++i)
                this->games[i].makeFilepaths(config);
            return 0;
        }
        else
        {
            if (result == 1)
            {
                std::cout << "Cache doesn't exist." << std::endl;
                std::cout << "Create cache with --update-cache" << std::endl;
            }
            else if (result == 3)
            {
                std::cout << "Cache is too old." << std::endl;
                std::cout << "Update cache with --update-cache or use bigger --cache-valid" << std::endl;
            }
            return 1;
        }
    }

    gameDetails game;
    int updated = 0;
    for (unsigned int i = 0; i < gameItems.size(); ++i)
    {
        std::cout << "Getting game info " << i+1 << " / " << gameItems.size() << "\r" << std::flush;
        bool bHasDLC = !gameItems[i].dlcnames.empty();

        gameSpecificConfig conf;
        conf.bDLC = config.bDLC;
        conf.iInstallerLanguage = config.iInstallerLanguage;
        conf.iInstallerType = config.iInstallerType;
        if (!config.bUpdateCache) // Disable game specific config files for cache update
        {
            if (Util::getGameSpecificConfig(gameItems[i].name, &conf) > 0)
                std::cout << std::endl << gameItems[i].name << " - Language: " << conf.iInstallerLanguage << ", Platform: " << conf.iInstallerType << ", DLC: " << (conf.bDLC ? "true" : "false") << std::endl;
        }

        game = gogAPI->getGameDetails(gameItems[i].name, conf.iInstallerType, conf.iInstallerLanguage, config.bDuplicateHandler);
        if (!gogAPI->getError())
        {
            game.filterWithPriorities(config);
            std::string gameDetailsHTML;

            if (game.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
            {
                gameDetailsHTML = this->getGameDetailsHTML(gameItems[i].name, gameItems[i].id);
                game.extras = this->getExtrasFromHTML(gameDetailsHTML, gameItems[i].name, gameItems[i].id);
            }
            if (config.bSaveSerials)
            {
                if (gameDetailsHTML.empty())
                    gameDetailsHTML = this->getGameDetailsHTML(gameItems[i].name, gameItems[i].id);
                game.serials = this->getSerialsFromHTML(gameDetailsHTML);
            }
            if (game.dlcs.empty() && bHasDLC && conf.bDLC)
            {
                for (unsigned int j = 0; j < gameItems[i].dlcnames.size(); ++j)
                {
                    gameDetails dlc;
                    std::string gameDetailsHTML_dlc;
                    dlc = gogAPI->getGameDetails(gameItems[i].dlcnames[j], conf.iInstallerType, conf.iInstallerLanguage, config.bDuplicateHandler);
                    dlc.filterWithPriorities(config);
                    if (dlc.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
                    {
                        gameDetailsHTML_dlc = this->getGameDetailsHTML(gameItems[i].dlcnames[j], gameItems[i].id);
                        dlc.extras = this->getExtrasFromHTML(gameDetailsHTML_dlc, gameItems[i].dlcnames[j], gameItems[i].id);
                    }
                    if (config.bSaveSerials)
                    {
                        if (gameDetailsHTML_dlc.empty())
                            gameDetailsHTML_dlc = this->getGameDetailsHTML(gameItems[i].dlcnames[j], gameItems[i].id);
                        dlc.serials = this->getSerialsFromHTML(gameDetailsHTML_dlc);
                    }
                    game.dlcs.push_back(dlc);
                }
            }

            game.makeFilepaths(config);

            if (!config.bUpdateCheck)
                games.push_back(game);
            else
            { // Update check, only add games that have updated files
                for (unsigned int j = 0; j < game.installers.size(); ++j)
                {
                    if (game.installers[j].updated)
                    {
                        games.push_back(game);
                        updated++;
                        break; // add the game only once
                    }
                }
                if (updated >= gogAPI->user.notifications_games)
                { // Gone through all updated games. No need to go through the rest.
                    std::cout << std::endl << "Got info for all updated games. Moving on..." << std::endl;
                    break;
                }
            }
        }
        else
        {
            std::cout << gogAPI->getErrorMessage() << std::endl;
            gogAPI->clearError();
            continue;
        }
    }
    std::cout << std::endl;
    return 0;
}

void Downloader::listGames()
{
    if (config.bListDetails) // Detailed list
    {
        if (this->games.empty())
            this->getGameDetails();

        for (unsigned int i = 0; i < games.size(); ++i)
        {
            std::cout   << "gamename: " << games[i].gamename << std::endl
                        << "title: " << games[i].title << std::endl
                        << "icon: " << "http://static.gog.com" << games[i].icon << std::endl;
            if (!games[i].serials.empty())
                std::cout << "serials:" << std::endl << games[i].serials << std::endl;

            // List installers
            if (config.bInstallers)
            {
                std::cout << "installers: " << std::endl;
                for (unsigned int j = 0; j < games[i].installers.size(); ++j)
                {
                    if (!config.bUpdateCheck || games[i].installers[j].updated) // Always list updated files
                    {
                        std::string filepath = games[i].installers[j].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::string languages;
                        for (unsigned int k = 0; k < GlobalConstants::LANGUAGES.size(); k++) // Check which languages the installer supports
                        {
                            if (games[i].installers[j].language & GlobalConstants::LANGUAGES[k].languageId)
                                languages += (languages.empty() ? "" : ", ")+GlobalConstants::LANGUAGES[k].languageString;
                        }

                        std::cout   << "\tid: " << games[i].installers[j].id << std::endl
                                    << "\tname: " << games[i].installers[j].name << std::endl
                                    << "\tpath: " << games[i].installers[j].path << std::endl
                                    << "\tsize: " << games[i].installers[j].size << std::endl
                                    << "\tupdated: " << (games[i].installers[j].updated ? "True" : "False") << std::endl
                                    << "\tlanguage: " << languages << std::endl
                                    << std::endl;
                    }
                }
            }
            // List extras
            if (config.bExtras && !config.bUpdateCheck && !games[i].extras.empty())
            {
                std::cout << "extras: " << std::endl;
                for (unsigned int j = 0; j < games[i].extras.size(); ++j)
                {
                    std::string filepath = games[i].extras[j].getFilepath();
                    if (config.blacklist.isBlacklisted(filepath))
                    {
                        if (config.bVerbose)
                            std::cout << "skipped blacklisted file " << filepath << std::endl;
                        continue;
                    }

                    std::cout   << "\tid: " << games[i].extras[j].id << std::endl
                                << "\tname: " << games[i].extras[j].name << std::endl
                                << "\tpath: " << games[i].extras[j].path << std::endl
                                << "\tsize: " << games[i].extras[j].size << std::endl
                                << std::endl;
                }
            }
            // List patches
            if (config.bPatches && !config.bUpdateCheck && !games[i].patches.empty())
            {
                std::cout << "patches: " << std::endl;
                for (unsigned int j = 0; j < games[i].patches.size(); ++j)
                {
                    std::string filepath = games[i].patches[j].getFilepath();
                    if (config.blacklist.isBlacklisted(filepath))
                    {
                        if (config.bVerbose)
                            std::cout << "skipped blacklisted file " << filepath << std::endl;
                        continue;
                    }

                    std::string languages;
                    for (unsigned int k = 0; k < GlobalConstants::LANGUAGES.size(); k++) // Check which languages the patch supports
                    {
                        if (games[i].patches[j].language & GlobalConstants::LANGUAGES[k].languageId)
                            languages += (languages.empty() ? "" : ", ")+GlobalConstants::LANGUAGES[k].languageString;
                    }

                    std::cout   << "\tid: " << games[i].patches[j].id << std::endl
                                << "\tname: " << games[i].patches[j].name << std::endl
                                << "\tpath: " << games[i].patches[j].path << std::endl
                                << "\tsize: " << games[i].patches[j].size << std::endl
                                << "\tupdated: " << (games[i].patches[j].updated ? "True" : "False") << std::endl
                                << "\tlanguage: " << languages << std::endl
                                << std::endl;
                }
            }
            // List language packs
            if (config.bLanguagePacks && !config.bUpdateCheck && !games[i].languagepacks.empty())
            {
                std::cout << "language packs: " << std::endl;
                for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
                {
                    std::string filepath = games[i].languagepacks[j].getFilepath();
                    if (config.blacklist.isBlacklisted(filepath))
                    {
                        if (config.bVerbose)
                            std::cout << "skipped blacklisted file " << filepath << std::endl;
                        continue;
                    }

                    std::cout   << "\tid: " << games[i].languagepacks[j].id << std::endl
                                << "\tname: " << games[i].languagepacks[j].name << std::endl
                                << "\tpath: " << games[i].languagepacks[j].path << std::endl
                                << "\tsize: " << games[i].languagepacks[j].size << std::endl
                                << std::endl;
                }
            }
            if (config.bDLC && !games[i].dlcs.empty())
            {
                std::cout << "DLCs: " << std::endl;
                for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
                {
                    if (!games[i].dlcs[j].serials.empty())
                    {
                        std::cout   << "\tDLC gamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tserials:" << games[i].dlcs[j].serials << std::endl;
                    }

                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].installers[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].installers[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].installers[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].installers[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].installers[k].size << std::endl
                                    << "\tupdated: " << (games[i].dlcs[j].installers[k].updated ? "True" : "False") << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].patches[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].patches[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].patches[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].patches[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].patches[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].extras[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].extras[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].extras[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].extras[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].extras[k].size << std::endl
                                    << std::endl;
                    }
                }
            }
        }
    }
    else
    {   // List game names
        for (unsigned int i = 0; i < gameItems.size(); ++i)
        {
            std::cout << gameItems[i].name << std::endl;
            for (unsigned int j = 0; j < gameItems[i].dlcnames.size(); ++j)
                std::cout << "+> " << gameItems[i].dlcnames[j] << std::endl;
        }
    }

}

void Downloader::repair()
{
    if (this->games.empty())
        this->getGameDetails();

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        // Installers (use remote or local file)
        if (config.bInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                std::string filepath = games[i].installers[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Get XML data
                std::string XML = "";
                if (config.bRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                }

                // Repair
                bool bUseLocalXML = !config.bRemoteXML;
                if (!XML.empty() || bUseLocalXML)
                {
                    std::string url = gogAPI->getInstallerLink(games[i].gamename, games[i].installers[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                    std::cout << "Repairing file " << filepath << std::endl;
                    this->repairFile(url, filepath, XML, games[i].gamename);
                    std::cout << std::endl;
                }
            }
        }

        // Extras (GOG doesn't provide XML data for extras, use local file)
        if (config.bExtras)
        {
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                std::string filepath = games[i].extras[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                std::string url = gogAPI->getExtraLink(games[i].gamename, games[i].extras[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }
                std::cout << "Repairing file " << filepath << std::endl;
                this->repairFile(url, filepath, std::string(), games[i].gamename);
                std::cout << std::endl;
            }
        }

        // Patches (use remote or local file)
        if (config.bPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                std::string filepath = games[i].patches[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Get XML data
                std::string XML = "";
                if (config.bRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                    }
                }

                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }
                std::cout << "Repairing file " << filepath << std::endl;
                this->repairFile(url, filepath, XML, games[i].gamename);
                std::cout << std::endl;
            }
        }

        // Language packs (GOG doesn't provide XML data for language packs, use local file)
        if (config.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                std::string filepath = games[i].languagepacks[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                std::string url = gogAPI->getLanguagePackLink(games[i].gamename, games[i].languagepacks[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }
                std::cout << "Repairing file " << filepath << std::endl;
                this->repairFile(url, filepath, std::string(), games[i].gamename);
                std::cout << std::endl;
            }
        }
        if (config.bDLC && !games[i].dlcs.empty())
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (config.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].installers[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get XML data
                        std::string XML = "";
                        if (config.bRemoteXML)
                        {
                            XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                            if (gogAPI->getError())
                            {
                                std::cout << gogAPI->getErrorMessage() << std::endl;
                                gogAPI->clearError();
                                continue;
                            }
                        }

                        // Repair
                        bool bUseLocalXML = !config.bRemoteXML;
                        if (!XML.empty() || bUseLocalXML)
                        {
                            std::string url = gogAPI->getInstallerLink(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                            if (gogAPI->getError())
                            {
                                std::cout << gogAPI->getErrorMessage() << std::endl;
                                gogAPI->clearError();
                                continue;
                            }
                            std::cout << "Repairing file " << filepath << std::endl;
                            this->repairFile(url, filepath, XML, games[i].dlcs[j].gamename);
                            std::cout << std::endl;
                        }
                    }
                }
                if (config.bPatches)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].patches[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get XML data
                        std::string XML = "";
                        if (config.bRemoteXML)
                        {
                            XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                            if (gogAPI->getError())
                            {
                                std::cout << gogAPI->getErrorMessage() << std::endl;
                                gogAPI->clearError();
                            }
                        }

                        std::string url = gogAPI->getPatchLink(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }
                        std::cout << "Repairing file " << filepath << std::endl;
                        this->repairFile(url, filepath, XML, games[i].dlcs[j].gamename);
                        std::cout << std::endl;
                    }
                }
                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].extras[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::string url = gogAPI->getExtraLink(games[i].dlcs[j].gamename, games[i].dlcs[j].extras[k].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }
                        std::cout << "Repairing file " << filepath << std::endl;
                        this->repairFile(url, filepath, std::string(), games[i].dlcs[j].gamename);
                        std::cout << std::endl;
                    }
                }
            }
        }
    }
}

void Downloader::download()
{
    if (this->games.empty())
        this->getGameDetails();

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        if (config.bSaveSerials && !games[i].serials.empty())
        {
            std::string filepath = games[i].getSerialsFilepath();
            this->saveSerials(games[i].serials, filepath);
        }

        // Download covers
        if (config.bCover && !config.bUpdateCheck)
        {
            if (!games[i].installers.empty())
            {
                // Take path from installer path because for some games the base directory for installer/extra path is not "gamename"
                std::string filepath = games[i].installers[0].getFilepath();

                // Get base directory from filepath
                boost::match_results<std::string::const_iterator> what;
                boost::regex expression("(.*)/.*");
                boost::regex_match(filepath, what, expression);
                std::string directory = what[1];

                this->downloadCovers(games[i].gamename, directory, coverXML);
            }
        }
        // Download installers
        if (config.bInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                // Not updated, skip to next installer
                if (config.bUpdateCheck && !games[i].installers[j].updated)
                    continue;

                std::string filepath = games[i].installers[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Get link
                std::string url = gogAPI->getInstallerLink(games[i].gamename, games[i].installers[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (config.bRemoteXML)
                    {
                        XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                        }
                    }
                    if (!games[i].installers[j].name.empty())
                        std::cout << "Downloading: " << games[i].installers[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    this->downloadFile(url, filepath, XML, games[i].gamename);
                    std::cout << std::endl;
                }
            }
        }
        // Download extras
        if (config.bExtras && !config.bUpdateCheck)
        { // Save some time and don't process extras when running update check. Extras don't have updated flag, all of them would be skipped anyway.
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                // Get link
                std::string url = gogAPI->getExtraLink(games[i].gamename, games[i].extras[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = games[i].extras[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Download
                if (!url.empty())
                {
                    if (!games[i].extras[j].name.empty())
                        std::cout << "Downloading: " << games[i].extras[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    CURLcode result = this->downloadFile(url, filepath);
                    std::cout << std::endl;
                    if (result==CURLE_OK && config.sXMLFile == "automatic")
                    {
                        std::cout << "Starting automatic XML creation" << std::endl;
                        std::string xml_dir = config.sXMLDirectory + "/" + games[i].gamename;
                        Util::createXML(filepath, config.iChunkSize, xml_dir);
                        std::cout << std::endl;
                    }
                }
            }
        }
        // Download patches
        if (config.bPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                // Not updated, skip to next patch
                if (config.bUpdateCheck && !games[i].patches[j].updated)
                    continue;

                // Get link
                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = games[i].patches[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (config.bRemoteXML)
                    {
                        XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                        }
                    }
                    if (!games[i].patches[j].name.empty())
                        std::cout << "Downloading: " << games[i].patches[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    this->downloadFile(url, filepath, XML, games[i].gamename);
                    std::cout << std::endl;
                }
            }
        }
        // Download language packs
        if (config.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                // Get link
                std::string url = gogAPI->getLanguagePackLink(games[i].gamename, games[i].languagepacks[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = games[i].languagepacks[j].getFilepath();
                if (config.blacklist.isBlacklisted(filepath))
                {
                    if (config.bVerbose)
                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (config.bRemoteXML)
                    {
                        XML = gogAPI->getXML(games[i].gamename, games[i].languagepacks[j].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                        }
                    }
                    if (!games[i].languagepacks[j].name.empty())
                        std::cout << "Downloading: " << games[i].gamename << " " << games[i].languagepacks[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    this->downloadFile(url, filepath, XML, games[i].gamename);
                    std::cout << std::endl;
                }
            }
        }
        if (config.bDLC && !games[i].dlcs.empty())
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (config.bSaveSerials && !games[i].dlcs[j].serials.empty())
                {
                    std::string filepath = games[i].dlcs[j].getSerialsFilepath();
                    this->saveSerials(games[i].dlcs[j].serials, filepath);
                }

                if (config.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].installers[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get link
                        std::string url = gogAPI->getInstallerLink(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }

                        // Download
                        if (!url.empty())
                        {
                            std::string XML;
                            if (config.bRemoteXML)
                            {
                                XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                                if (gogAPI->getError())
                                {
                                    std::cout << gogAPI->getErrorMessage() << std::endl;
                                    gogAPI->clearError();
                                }
                            }
                            if (!games[i].dlcs[j].installers[k].name.empty())
                                std::cout << "Downloading: " << games[i].dlcs[j].installers[k].name << std::endl;
                            std::cout << filepath << std::endl;
                            this->downloadFile(url, filepath, XML, games[i].dlcs[j].gamename);
                            std::cout << std::endl;
                        }
                    }
                }
                if (config.bPatches)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].patches[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get link
                        std::string url = gogAPI->getPatchLink(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }

                        // Download
                        if (!url.empty())
                        {
                            std::string XML;
                            if (config.bRemoteXML)
                            {
                                XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                                if (gogAPI->getError())
                                {
                                    std::cout << gogAPI->getErrorMessage() << std::endl;
                                    gogAPI->clearError();
                                }
                            }
                            if (!games[i].dlcs[j].patches[k].name.empty())
                                std::cout << "Downloading: " << games[i].dlcs[j].patches[k].name << std::endl;
                            std::cout << filepath << std::endl;
                            this->downloadFile(url, filepath, XML, games[i].dlcs[j].gamename);
                            std::cout << std::endl;
                        }
                    }
                }
                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].extras[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath))
                        {
                            if (config.bVerbose)
                                std::cout << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get link
                        std::string url = gogAPI->getExtraLink(games[i].dlcs[j].gamename, games[i].dlcs[j].extras[k].id);
                        if (gogAPI->getError())
                        {
                            std::cout << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }

                        // Download
                        if (!url.empty())
                        {
                            if (!games[i].dlcs[j].extras[k].name.empty())
                                std::cout << "Dowloading: " << games[i].dlcs[j].extras[k].name << std::endl;
                            CURLcode result = this->downloadFile(url, filepath);
                            std::cout << std::endl;
                            if (result==CURLE_OK && config.sXMLFile == "automatic")
                            {
                                std::cout << "Starting automatic XML creation" << std::endl;
                                std::string xml_dir = config.sXMLDirectory + "/" + games[i].dlcs[j].gamename;
                                Util::createXML(filepath, config.iChunkSize, xml_dir);
                                std::cout << std::endl;
                            }
                        }
                    }
                }
            }
        }
    }
}

// Download a file, resume if possible
CURLcode Downloader::downloadFile(const std::string& url, const std::string& filepath, const std::string& xml_data, const std::string& gamename)
{
    CURLcode res = CURLE_RECV_ERROR; // assume network error
    bool bResume = false;
    FILE *outfile;
    size_t offset=0;

    // Get directory from filepath
    boost::filesystem::path pathname = filepath;
    std::string directory = pathname.parent_path().string();
    std::string filenameXML = pathname.filename().string() + ".xml";
    std::string xml_directory;
    if (!gamename.empty())
        xml_directory = config.sXMLDirectory + "/" + gamename;
    else
        xml_directory = config.sXMLDirectory;

    // Using local XML data for version check before resuming
    boost::filesystem::path local_xml_file;
    local_xml_file = xml_directory + "/" + filenameXML;

    bool bSameVersion = true; // assume same version
    bool bLocalXMLExists = boost::filesystem::exists(local_xml_file); // This is additional check to see if remote xml should be saved to speed up future version checks

    if (!xml_data.empty())
    {
        std::string localHash = this->getLocalFileHash(filepath, gamename);
        // Do version check if local hash exists
        if (!localHash.empty())
        {
            TiXmlDocument remote_xml;
            remote_xml.Parse(xml_data.c_str());
            TiXmlNode *fileNodeRemote = remote_xml.FirstChild("file");
            if (fileNodeRemote)
            {
                TiXmlElement *fileElemRemote = fileNodeRemote->ToElement();
                std::string remoteHash = fileElemRemote->Attribute("md5");
                if (remoteHash != localHash)
                    bSameVersion = false;
            }
        }
    }

    // Check that directory exists and create subdirectories
    boost::filesystem::path path = directory;
    if (boost::filesystem::exists(path))
    {
        if (!boost::filesystem::is_directory(path))
        {
            std::cout << path << " is not directory" << std::endl;
            return res;
        }
    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    // Check if file exists
    if ((outfile=fopen(filepath.c_str(), "r"))!=NULL)
    {
        if (bSameVersion)
        {
            // File exists, resume
            if ((outfile = freopen(filepath.c_str(), "r+", outfile))!=NULL )
            {
                bResume = true;
                fseek(outfile, 0, SEEK_END);
                offset = ftell(outfile);
                curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, offset);
                this->resume_position = offset;
            }
            else
            {
                std::cout << "Failed to reopen " << filepath << std::endl;
                return res;
            }
        }
        else
        {   // File exists but is not the same version
            fclose(outfile);
            std::cout << "Remote file is different, renaming local file" << std::endl;
            std::string date_old = "." + bptime::to_iso_string(bptime::second_clock::local_time()) + ".old";
            boost::filesystem::path new_name = filepath + date_old; // Rename old file by appending date and ".old" to filename
            boost::system::error_code ec;
            boost::filesystem::rename(pathname, new_name, ec); // Rename the file
            if (ec)
            {
                std::cout << "Failed to rename " << filepath << " to " << new_name.string() << std::endl;
                std::cout << "Skipping file" << std::endl;
                return res;
            }
            else
            {
                // Create new file
                if ((outfile=fopen(filepath.c_str(), "w"))!=NULL)
                {
                    curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, 0); // start downloading from the beginning of file
                    this->resume_position = 0;
                }
                else
                {
                    std::cout << "Failed to create " << filepath << std::endl;
                    return res;
                }
            }
        }
    }
    else
    {
        // File doesn't exist, create new file
        if ((outfile=fopen(filepath.c_str(), "w"))!=NULL)
        {
            curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, 0); // start downloading from the beginning of file
            this->resume_position = 0;
        }
        else
        {
            std::cout << "Failed to create " << filepath << std::endl;
            return res;
        }
    }

    // Save remote XML
    if (!xml_data.empty())
    {
        if ((bLocalXMLExists && (!bSameVersion || config.bRepair)) || !bLocalXMLExists)
        {
            // Check that directory exists and create subdirectories
            boost::filesystem::path path = xml_directory;
            if (boost::filesystem::exists(path))
            {
                if (!boost::filesystem::is_directory(path))
                {
                    std::cout << path << " is not directory" << std::endl;
                }
            }
            else
            {
                if (!boost::filesystem::create_directories(path))
                {
                    std::cout << "Failed to create directory: " << path << std::endl;
                }
            }
            std::ofstream ofs(local_xml_file.string().c_str());
            if (ofs)
            {
                ofs << xml_data;
                ofs.close();
            }
            else
            {
                std::cout << "Can't create " << local_xml_file.string() << std::endl;
            }
        }
    }

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
    res = this->beginDownload();

    fclose(outfile);

    // Download failed and was not a resume attempt so delete the file
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE && !bResume)
    {
        boost::filesystem::path path = filepath;
        if (boost::filesystem::exists(path))
            if (!boost::filesystem::remove(path))
                std::cout << "Failed to delete " << path << std::endl;
    }

    if (config.bReport)
    {
        std::string status = static_cast<std::string>(curl_easy_strerror(res));
        if (bResume && res == CURLE_RANGE_ERROR) // CURLE_RANGE_ERROR on resume attempts is not an error that user needs to know about
            status = "No error";
        std::string report_line = "Downloaded [" + status + "] " + filepath;
        this->report_ofs << report_line << std::endl;
    }

    // Retry partially downloaded file
    if (res == CURLE_PARTIAL_FILE && (this->retries < config.iRetries) )
    {
        this->retries++;
        res = this->downloadFile(url, filepath, xml_data, gamename);
    }
    else
    {
        this->retries = 0; // Reset retries counter
    }

    return res;
}

// Repair file
int Downloader::repairFile(const std::string& url, const std::string& filepath, const std::string& xml_data, const std::string& gamename)
{
    int res = 0;
    FILE *outfile;
    size_t offset=0, from_offset, to_offset, filesize;
    std::string filehash;
    int chunks;
    std::vector<size_t> chunk_from, chunk_to;
    std::vector<std::string> chunk_hash;
    bool bParsingFailed = false;

    // Get filename
    boost::filesystem::path pathname = filepath;
    std::string filename = pathname.filename().string();
    std::string xml_directory;
    if (!gamename.empty())
        xml_directory = config.sXMLDirectory + "/" + gamename;
    else
        xml_directory = config.sXMLDirectory;
    std::string xml_file = xml_directory + "/" + filename + ".xml";
    bool bFileExists = boost::filesystem::exists(pathname);
    bool bLocalXMLExists = boost::filesystem::exists(xml_file);

    TiXmlDocument xml;
    if (!xml_data.empty()) // Parse remote XML data
    {
        std::cout << "XML: Using remote file" << std::endl;
        xml.Parse(xml_data.c_str());
    }
    else
    {   // Parse local XML data
        std::cout << "XML: Using local file" << std::endl;
        if (!bLocalXMLExists)
            std::cout << "XML: File doesn't exist (" << xml_file << ")" << std::endl;
        xml.LoadFile(xml_file);
    }

    // Check if file node exists in XML data
    TiXmlNode *fileNode = xml.FirstChild("file");
    if (!fileNode)
    {   // File node doesn't exist
        std::cout << "XML: Parsing failed / not valid XML" << std::endl;
        if (config.bDownload)
            bParsingFailed = true;
        else
            return res;
    }
    else
    {   // File node exists --> valid XML
        std::cout << "XML: Valid XML" << std::endl;
        TiXmlElement *fileElem = fileNode->ToElement();
        filename = fileElem->Attribute("name");
        filehash = fileElem->Attribute("md5");
        std::stringstream(fileElem->Attribute("chunks")) >> chunks;
        std::stringstream(fileElem->Attribute("total_size")) >> filesize;

        //Iterate through all chunk nodes
        TiXmlNode *chunkNode = fileNode->FirstChild();
        while (chunkNode)
        {
            TiXmlElement *chunkElem = chunkNode->ToElement();
            std::stringstream(chunkElem->Attribute("from")) >> from_offset;
            std::stringstream(chunkElem->Attribute("to")) >> to_offset;
            chunk_from.push_back(from_offset);
            chunk_to.push_back(to_offset);
            chunk_hash.push_back(chunkElem->GetText());
            chunkNode = fileNode->IterateChildren(chunkNode);
        }

        std::cout   << "XML: Parsing finished" << std::endl << std::endl
                    << filename << std::endl
                    << "\tMD5:\t" << filehash << std::endl
                    << "\tChunks:\t" << chunks << std::endl
                    << "\tSize:\t" << filesize << " bytes" << std::endl << std::endl;
    }

    // No local XML file and parsing failed.
    if (bParsingFailed && !bLocalXMLExists)
    {
        if (this->config.bDownload)
        {
            std::cout << "Downloading: " << filepath << std::endl;
            CURLcode result = this->downloadFile(url, filepath, xml_data, gamename);
            std::cout << std::endl;
            if  (
                    (!bFileExists && result == CURLE_OK) || /* File doesn't exist so only accept if everything was OK */
                    (bFileExists && (result == CURLE_OK || result == CURLE_RANGE_ERROR ))   /* File exists so accept also CURLE_RANGE_ERROR because curl will return CURLE_RANGE_ERROR */
                )                                                                           /* if the file is already fully downloaded and we want to resume it */
            {
                bLocalXMLExists = boost::filesystem::exists(xml_file); // Check to see if downloadFile saved XML data

                if (config.sXMLFile == "automatic" && !bLocalXMLExists)
                {
                    std::cout << "Starting automatic XML creation" << std::endl;
                    Util::createXML(filepath, config.iChunkSize, xml_directory);
                }
                res = 1;
            }
        }
        else
        {
            std::cout << "Can't repair file." << std::endl;
        }
        return res;
    }

    // Check if file exists
    if (bFileExists)
    {
        // File exists
        if ((outfile = fopen(filepath.c_str(), "r+"))!=NULL )
        {
            fseek(outfile, 0, SEEK_END);
            offset = ftell(outfile);
        }
        else
        {
            std::cout << "Failed to open " << filepath << std::endl;
            return res;
        }
    }
    else
    {
        std::cout << "File doesn't exist " << filepath << std::endl;
        if (this->config.bDownload)
        {
            std::cout << "Downloading: " << filepath << std::endl;
            CURLcode result = this->downloadFile(url, filepath, xml_data, gamename);
            std::cout << std::endl;
            if (result == CURLE_OK)
            {
                if (config.sXMLFile == "automatic" && bParsingFailed)
                {
                    std::cout << "Starting automatic XML creation" << std::endl;
                    Util::createXML(filepath, config.iChunkSize, xml_directory);
                }
                res = 1;
            }
        }
        return res;
    }

    if (offset != filesize)
    {
        std::cout   << "Filesizes don't match" << std::endl
                    << "Incomplete download or different version" << std::endl;
        fclose(outfile);
        if (this->config.bDownload)
        {
            std::cout << "Redownloading file" << std::endl;

            std::string date_old = "." + bptime::to_iso_string(bptime::second_clock::local_time()) + ".old";
            boost::filesystem::path new_name = filepath + date_old; // Rename old file by appending date and ".old" to filename
            std::cout << "Renaming old file to " << new_name.string() << std::endl;
            boost::system::error_code ec;
            boost::filesystem::rename(pathname, new_name, ec); // Rename the file
            if (ec)
            {
                std::cout << "Failed to rename " << filepath << " to " << new_name.string() << std::endl;
                std::cout << "Skipping file" << std::endl;
                res = 0;
            }
            else
            {
                CURLcode result = this->downloadFile(url, filepath, xml_data, gamename);
                std::cout << std::endl;
                if (result == CURLE_OK)
                    res = 1;
                else
                    res = 0;
            }
        }
        return res;
    }

    // Check all chunks
    int iChunksRepaired = 0;
    for (int i=0; i<chunks; i++)
    {
        size_t chunk_begin = chunk_from.at(i);
        size_t chunk_end = chunk_to.at(i);
        size_t size=0, chunk_size = chunk_end - chunk_begin + 1;
        std::string range = std::to_string(chunk_begin) + "-" + std::to_string(chunk_end); // Download range string for curl

        std::cout << "\033[0K\rChunk " << i << " (" << chunk_size << " bytes): ";
        fseek(outfile, chunk_begin, SEEK_SET);
        unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
        if (chunk == NULL)
        {
            std::cout << "Memory error" << std::endl;
            fclose(outfile);
            return res;
        }
        size = fread(chunk, 1, chunk_size, outfile);
        if (size != chunk_size)
        {
            std::cout << "Read error" << std::endl;
            free(chunk);
            fclose(outfile);
            return res;
        }
        std::string hash = Util::getChunkHash(chunk, chunk_size, RHASH_MD5);
        if (hash != chunk_hash.at(i))
        {
            std::cout << "Failed - downloading chunk" << std::endl;
            fseek(outfile, chunk_begin, SEEK_SET);
            curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
            curl_easy_setopt(curlhandle, CURLOPT_RANGE, range.c_str()); //download range
            this->beginDownload(); //begin chunk download
            std::cout << std::endl;
            if (config.bReport)
                iChunksRepaired++;
            i--; //verify downloaded chunk
        }
        else
        {
            std::cout << "OK\r" << std::flush;
        }
        free(chunk);
        res = 1;
    }
    std::cout << std::endl;
    fclose(outfile);

    if (config.bReport)
    {
        std::string report_line = "Repaired [" + std::to_string(iChunksRepaired) + "/" + std::to_string(chunks) + "] " + filepath;
        this->report_ofs << report_line << std::endl;
    }

    return res;
}

// Download cover images
int Downloader::downloadCovers(const std::string& gamename, const std::string& directory, const std::string& cover_xml_data)
{
    int res = 0;
    TiXmlDocument xml;

    // Check that directory exists and create subdirectories
    boost::filesystem::path path = directory;
    if (boost::filesystem::exists(path))
    {
        if (!boost::filesystem::is_directory(path))
        {
            std::cout << path << " is not directory" << std::endl;
            return res;
        }

    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    xml.Parse(cover_xml_data.c_str());
    TiXmlElement *rootNode = xml.RootElement();
    if (!rootNode)
    {
        std::cout << "Not valid XML" << std::endl;
        return res;
    }
    else
    {
        TiXmlNode *gameNode = rootNode->FirstChild();
        while (gameNode)
        {
            TiXmlElement *gameElem = gameNode->ToElement();
            std::string game_name = gameElem->Attribute("name");

            if (game_name == gamename)
            {
                boost::match_results<std::string::const_iterator> what;
                TiXmlNode *coverNode = gameNode->FirstChild();
                while (coverNode)
                {
                    TiXmlElement *coverElem = coverNode->ToElement();
                    std::string cover_url = coverElem->GetText();
                    // Get file extension for the image
                    boost::regex e1(".*(\\.\\w+)$", boost::regex::perl | boost::regex::icase);
                    boost::regex_search(cover_url, what, e1);
                    std::string file_extension = what[1];
                    std::string cover_name = std::string("cover_") + coverElem->Attribute("id") + file_extension;
                    std::string filepath = directory + "/" + cover_name;

                    std::cout << "Downloading cover " << filepath << std::endl;
                    CURLcode result = this->downloadFile(cover_url, filepath);
                    std::cout << std::endl;
                    if (result == CURLE_OK)
                        res = 1;
                    else
                        res = 0;

                    if (result == CURLE_HTTP_RETURNED_ERROR)
                    {
                        long int response_code = 0;
                        result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                        std::cout << "HTTP ERROR: ";
                        if (result == CURLE_OK)
                            std::cout << response_code << std::endl;
                        else
                            std::cout << "failed to get error code: " << curl_easy_strerror(result) << std::endl;
                    }

                    coverNode = gameNode->IterateChildren(coverNode);
                }
                break; // Found cover for game, no need to go through rest of the game nodes
            }
            gameNode = rootNode->IterateChildren(gameNode);
        }
    }

    return res;
}

CURLcode Downloader::beginDownload()
{
    this->timer.reset();
    CURLcode result = curl_easy_perform(curlhandle);
    this->resume_position = 0;
    return result;
}

std::string Downloader::getResponse(const std::string& url)
{
    std::ostringstream memory;
    std::string response;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);

    CURLcode result;
    do
    {
        if (config.iWait > 0)
            usleep(config.iWait); // Delay the request by specified time
        result = curl_easy_perform(curlhandle);
        response = memory.str();
        memory.str(std::string());
    }
    while ((result != CURLE_OK) && response.empty() && (this->retries++ < config.iRetries));
    this->retries = 0; // reset retries counter

    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
        if (result == CURLE_HTTP_RETURNED_ERROR)
        {
            long int response_code = 0;
            result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
            std::cout << "HTTP ERROR: ";
            if (result == CURLE_OK)
                std::cout << response_code << std::endl;
            else
                std::cout << "failed to get error code: " << curl_easy_strerror(result) << std::endl;
        }
    }

    return response;
}

int Downloader::progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    // on entry: dltotal - how much remains to download till the end of the file (bytes)
    //           dlnow   - how much was downloaded from the start of the program (bytes)
    int bar_length      = 26;
    int min_bar_length  = 5;
    Downloader* downloader = static_cast<Downloader*>(clientp);

    double rate; //  average download speed in B/s
    // trying to get rate and setting to NaN if it fails
    if (CURLE_OK != curl_easy_getinfo(downloader->curlhandle, CURLINFO_SPEED_DOWNLOAD, &rate))
       rate = std::numeric_limits<double>::quiet_NaN();

    // (Shmerl): this flag is needed to catch the case before anything was downloaded on resume,
    // and there is no way to calculate the fraction, so we set to 0 (otherwise it'd be 1).
    // This is to prevent the progress bar from jumping to 100% and then to lower value.
    // It's visually better to jump from 0% to higher one.
    bool starting = ((0.0 == dlnow) && (0.0 == dltotal));

    // (Shmerl): DEBUG: strange thing - when resuming a file which is already downloaded, dlnow is correctly 0.0
    // but dltotal is 389.0! This messes things up in the progress bar not showing the very last bar as full.
    // enable this debug line to test the problem:
    //
    //   printf("\r\033[0K dlnow: %0.2f, dltotal: %0.2f\r", dlnow, dltotal); fflush(stdout); return 0;
    //
    // For now making a quirky workaround and setting dltotal to 0.0 in that case.
    // It's probably better to find a real fix.
    if ((0.0 == dlnow) && (389.0 == dltotal)) dltotal = 0.0;

    // setting full dlwnow and dltotal
    double offset = static_cast<double>(downloader->getResumePosition());
    if (offset>0)
    {
        dlnow   += offset;
        dltotal += offset;
    }

    // Update progress bar every 100ms
    if (downloader->timer.getTimeBetweenUpdates()>=100 || dlnow == dltotal)
    {
        downloader->timer.reset();
        int iTermWidth = Util::getTerminalWidth();
        bptime::time_duration eta(bptime::seconds((long)((dltotal - dlnow) / rate)));
        std::stringstream eta_ss;
        if (eta.hours() > 23)
        {
           eta_ss << eta.hours() / 24 << "d " <<
                     std::setfill('0') << std::setw(2) << eta.hours() % 24 << "h " <<
                     std::setfill('0') << std::setw(2) << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else if (eta.hours() > 0)
        {
           eta_ss << eta.hours() << "h " <<
                     std::setfill('0') << std::setw(2) << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else if (eta.minutes() > 0)
        {
           eta_ss << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else
        {
           eta_ss << eta.seconds() << "s";
        }

        // Create progressbar
        double fraction = starting ? 0.0 : dlnow / dltotal;

        // assuming that config is provided.
        printf("\033[0K\r%3.0f%% ", fraction * 100);

        // Download rate unit conversion
        std::string rate_unit;
        if (rate > 1048576) // 1 MB
        {
            rate /= 1048576;
            rate_unit = "MB/s";
        }
        else
        {
            rate /= 1024;
            rate_unit = "kB/s";
        }
        char status_text[200]; // We're probably never going to go as high as 200 characters but it's better to use too big number here than too small
        sprintf(status_text, " %0.2f/%0.2fMB @ %0.2f%s ETA: %s\r", dlnow/1024/1024, dltotal/1024/1024, rate, rate_unit.c_str(), eta_ss.str().c_str());
        int status_text_length = strlen(status_text) + 6;

        if ((status_text_length + bar_length) > iTermWidth)
            bar_length -= (status_text_length + bar_length) - iTermWidth;

        // Don't draw progressbar if length is less than min_bar_length
        if (bar_length >= min_bar_length)
            downloader->progressbar->draw(bar_length, fraction);

        std::cout << status_text << std::flush;
    }

    return 0;
}

size_t Downloader::writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) {
    std::ostringstream *stream = (std::ostringstream*)userp;
    size_t count = size * nmemb;
    stream->write(ptr, count);
    return count;
}

size_t Downloader::writeData(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

size_t Downloader::readData(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return fread(ptr, size, nmemb, stream);
}

size_t Downloader::getResumePosition()
{
    return this->resume_position;
}

// Login to GOG website
int Downloader::HTTP_Login(const std::string& email, const std::string& password)
{
    int res = 0;
    std::string postdata;
    std::ostringstream memory;
    std::string token;
    std::string tagname_username;
    std::string tagname_password;
    std::string tagname_login;
    std::string tagname_token;

    // Get login token
    std::string html = this->getResponse("https://www.gog.com/");
    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    // Find auth_url
    bool bFoundAuthUrl = false;
    for (; it != end; ++it)
    {
        if (it->tagName()=="script")
        {
            std::string auth_url;
            for (unsigned int i = 0; i < dom.number_of_children(it); ++i)
            {
                tree<htmlcxx::HTML::Node>::iterator script_it = dom.child(it, i);
                if (!script_it->isTag() && !script_it->isComment())
                {
                    if (script_it->text().find("GalaxyAccounts") != std::string::npos)
                    {
                        boost::match_results<std::string::const_iterator> what;
                        boost::regex expression(".*'(https://auth.gog.com/.*?)'.*");
                        boost::regex_match(script_it->text(), what, expression);
                        auth_url = what[1];
                        break;
                    }
                }
            }

            if (!auth_url.empty())
            {   // Found auth_url, get the necessary info for login
                bFoundAuthUrl = true;
                std::string login_form_html = this->getResponse(auth_url);
                #ifdef DEBUG
                    std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl;
                    std::cerr << login_form_html << std::endl;
                #endif
                tree<htmlcxx::HTML::Node> login_dom = parser.parseTree(login_form_html);
                tree<htmlcxx::HTML::Node>::iterator login_it = login_dom.begin();
                tree<htmlcxx::HTML::Node>::iterator login_it_end = login_dom.end();
                for (; login_it != login_it_end; ++login_it)
                {
                    if (login_it->tagName()=="input")
                    {
                        login_it->parseAttributes();
                        std::string id_login = login_it->attribute("id").second;
                        if (id_login == "login_username")
                        {
                            tagname_username = login_it->attribute("name").second;
                        }
                        else if (id_login == "login_password")
                        {
                            tagname_password = login_it->attribute("name").second;
                        }
                        else if (id_login == "login__token")
                        {
                            token = login_it->attribute("value").second; // login token
                            tagname_token = login_it->attribute("name").second;
                        }
                    }
                    else if (login_it->tagName()=="button")
                    {
                        login_it->parseAttributes();
                        std::string id_login = login_it->attribute("id").second;
                        if (id_login == "login_login")
                        {
                            tagname_login = login_it->attribute("name").second;
                        }
                    }
                }
                break;
            }
        }
    }

    if (!bFoundAuthUrl)
    {
        std::cout << "Failed to find url for login form" << std::endl;
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
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

    // Don't follow to redirect location because it doesn't work properly. Must clean up the redirect url first.
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

    curl_easy_setopt(curlhandle, CURLOPT_URL, redirect_url);
    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    result = curl_easy_perform(curlhandle);

    if (result != CURLE_OK)
    {
        std::cout << curl_easy_strerror(result) << std::endl;
    }

    html = this->getResponse("https://www.gog.com/account/settings");

    std::string account_email, username;
    dom = parser.parseTree(html);
    it = dom.begin();
    end = dom.end();
    bool bEmailFound = false;
    bool bUsernameFound = false;
    for (; it != end; ++it)
    {
        if (it->tagName()=="input")
        {
            it->parseAttributes();
            if (it->attribute("id").second == "accountEditEmail")
            {
                account_email = it->attribute("value").second;
                bEmailFound = true;
            }
        }
        else if (it->tagName()=="span")
        {
            it->parseAttributes();
            if (it->attribute("class").second == "nickname")
            {
                for (unsigned int i = 0; i < dom.number_of_children(it); ++i)
                {
                    tree<htmlcxx::HTML::Node>::iterator nick_it = dom.child(it, i);
                    if (!nick_it->isTag() && !nick_it->isComment())
                    {
                        username = nick_it->text();
                        bUsernameFound = true;
                    }
                }
            }
        }

        if (bUsernameFound && bEmailFound)
            break;
    }

    // Convert to lowercase for comparison
    std::string email_lowercase = boost::algorithm::to_lower_copy(email); // boost::algorithm::to_lower does in-place modification but "email" is read-only so we need to make a copy of it
    boost::algorithm::to_lower(account_email);
    boost::algorithm::to_lower(username);

    if (email_lowercase == account_email || email_lowercase == username)
    {
        res = 1; // Login successful
    }
    else
    {
        #ifdef DEBUG
            std::cerr   << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl;
            if (!bEmailFound || !bUsernameFound)
            {
                if (!bEmailFound)
                    std::cerr << "Could not find \"accountEditEmail\" input field on account settings page." << std::endl;
                if (!bUsernameFound)
                    std::cerr << "Could not find username on account settings page." << std::endl;
            }
            else
            {
                if (email_lowercase != account_email)
                    std::cerr << "Email (" << email_lowercase << ") doesn't match account email (" << account_email << ")" << std::endl;
                if (email_lowercase != username)
                    std::cerr << "Username (" << email_lowercase << ") doesn't match account username (" << username << ")" << std::endl;
            }
        #endif
        res = 0; // Login failed
    }

    return res;
}

// Get list of games from account page
std::vector<gameItem> Downloader::getGames()
{
    std::vector<gameItem> games;
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    int i = 1;
    std::string html = "";
    std::string page_html = "";

    do
    {
        std::string response = this->getResponse("https://www.gog.com/account/ajax?a=gamesShelfMore&s=title&q=&t=0&p=" + std::to_string(i));

        // Parse JSON
        if (!jsonparser->parse(response, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << response << std::endl;
            #endif
            std::cout << jsonparser->getFormatedErrorMessages();
            delete jsonparser;
            exit(1);
        }
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << root << std::endl;
        #endif
        page_html = root["html"].asString();
        html += page_html;
        if (page_html.empty() && i == 1)
        {
            std::cout << "No games were found on your account. Try --login to refresh your authorization." << std::endl;
        }
        i++;
    } while (!page_html.empty());

    delete jsonparser;

    // Parse HTML to get game names
    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="div")
        {
            it->parseAttributes();
            std::string classname = it->attribute("class").second;
            if (classname=="shelf_game")
            {
                gameItem game;
                // Game name is contained in data-gameindex attribute
                game.name = it->attribute("data-gameindex").second;
                game.id = it->attribute("data-gameid").second;

                // Get platform info
                std::string tags = it->attribute("data-title").second;
                unsigned int platform = GlobalConstants::PLATFORM_WINDOWS; // The tags don't specify Windows support so assume that there's always a Windows version

                if (tags.find("linux") != std::string::npos)
                    platform |= GlobalConstants::PLATFORM_LINUX;
                if (tags.find("osx mac") != std::string::npos)
                    platform |= GlobalConstants::PLATFORM_MAC;

                // Skip if platform doesn't match
                if (config.bPlatformDetection && !(platform & config.iInstallerType))
                    continue;

                if (!game.name.empty() && !game.id.empty())
                {
                    // Check for DLC
                    if (config.bDLC)
                    {
                        tree<htmlcxx::HTML::Node>::iterator dlc_it = it;
                        tree<htmlcxx::HTML::Node>::iterator dlc_end = it.end();
                        for (; dlc_it != dlc_end; ++dlc_it)
                        {
                            if (dlc_it->tagName()=="div")
                            {
                                dlc_it->parseAttributes();
                                std::string classname_dlc = dlc_it->attribute("class").second;
                                if (classname_dlc == "shelf-game-dlc-counter")
                                {
                                    std::string content;
                                    for (unsigned int i = 0; i < dom.number_of_children(dlc_it); ++i)
                                    {
                                        tree<htmlcxx::HTML::Node>::iterator it = dom.child(dlc_it, i);
                                        if (!it->isTag() && !it->isComment())
                                            content += it->text();
                                    }
                                    // Get game names if game has DLC
                                    if (content.find("DLC")!=std::string::npos)
                                    {
                                        Json::Value root;
                                        Json::Reader *jsonparser = new Json::Reader;

                                        std::string gameDataUrl = "https://www.gog.com/account/ajax?a=gamesListDetails&g=" + game.id;
                                        std::string json = this->getResponse(gameDataUrl);
                                        // Parse JSON
                                        if (!jsonparser->parse(json, root))
                                        {
                                            #ifdef DEBUG
                                                std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << json << std::endl;
                                            #endif
                                            std::cout << jsonparser->getFormatedErrorMessages();
                                            delete jsonparser;
                                            exit(1);
                                        }
                                        #ifdef DEBUG
                                            std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << root << std::endl;
                                        #endif
                                        std::string html = root["details"]["html"].asString();
                                        delete jsonparser;

                                        // Parse HTML to get game names for DLC
                                        htmlcxx::HTML::ParserDom parser;
                                        tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
                                        tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
                                        tree<htmlcxx::HTML::Node>::iterator end = dom.end();
                                        for (; it != end; ++it)
                                        {
                                            if (it->tagName()=="div")
                                            {
                                                it->parseAttributes();
                                                std::string gamename = it->attribute("data-gameindex").second;
                                                if (!gamename.empty() && gamename!=game.name)
                                                {
                                                    bool bDuplicate = false;
                                                    for (unsigned int i = 0; i < game.dlcnames.size(); ++i)
                                                    {
                                                        if (gamename == game.dlcnames[i])
                                                        {
                                                            bDuplicate = true;
                                                            break;
                                                        }
                                                    }
                                                    if (!bDuplicate)
                                                        game.dlcnames.push_back(gamename);
                                                }
                                            }
                                        }

                                        // Try getting game names for DLCs from extra links. Catches game names for DLCs that don't have installers.
                                        it = dom.begin();
                                        end = dom.end();
                                        for (; it != end; ++it)
                                        {
                                            if (it->tagName()=="a")
                                            {
                                                it->parseAttributes();
                                                std::string href = it->attribute("href").second;
                                                std::string search_string = "/downlink/file/"; // Extra links: https://www.gog.com/downlink/file/gamename/id_number
                                                if (href.find(search_string)!=std::string::npos)
                                                {
                                                    std::string gamename;
                                                    gamename.assign(href.begin()+href.find(search_string)+search_string.length(), href.begin()+href.find_last_of("/"));
                                                    if (!gamename.empty() && gamename!=game.name)
                                                    {
                                                        bool bDuplicate = false;
                                                        for (unsigned int i = 0; i < game.dlcnames.size(); ++i)
                                                        {
                                                            if (gamename == game.dlcnames[i])
                                                            {
                                                                bDuplicate = true;
                                                                break;
                                                            }
                                                        }
                                                        if (!bDuplicate)
                                                            game.dlcnames.push_back(gamename);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    games.push_back(game);
                }
            }
        }
    }

    return games;
}

// Get list of free games
std::vector<gameItem> Downloader::getFreeGames()
{
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    std::vector<gameItem> games;
    std::string json = this->getResponse("https://www.gog.com/games/ajax/filtered?mediaType=game&page=1&price=free&sort=title");

    // Parse JSON
    if (!jsonparser->parse(json, root))
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::getFreeGames)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        delete jsonparser;
        exit(1);
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::getFreeGames)" << std::endl << root << std::endl;
    #endif

    Json::Value products = root["products"];
    for (unsigned int i = 0; i < products.size(); ++i)
    {
        gameItem game;
        game.name = products[i]["slug"].asString();
        game.id = products[i]["id"].isInt() ? std::to_string(products[i]["id"].asInt()) : products[i]["id"].asString();
        games.push_back(game);
    }
    delete jsonparser;

    return games;
}

std::string Downloader::getGameDetailsHTML(const std::string& gamename, const std::string& gameid)
{
    std::string gameDataUrl = "https://www.gog.com/account/ajax?a=gamesListDetails&g=" + gameid;
    std::string json = this->getResponse(gameDataUrl);

    // Parse JSON
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    if (!jsonparser->parse(json, root))
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::getGameDetailsHTML)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        delete jsonparser;
        exit(1);
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::getGameDetailsHTML)" << std::endl << root << std::endl;
    #endif
    std::string html = root["details"]["html"].asString();
    delete jsonparser;

    return html;
}

std::vector<gameFile> Downloader::getExtrasFromHTML(const std::string& html, const std::string& gamename, const std::string& gameid)
{
    std::vector<gameFile> extras;

    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="a")
        {
            it->parseAttributes();
            std::string href = it->attribute("href").second;
            // Extra links https://www.gog.com/downlink/file/gamename/id_number
            if (href.find("/downlink/file/" + gamename + "/")!=std::string::npos)
            {
                std::string id, name, path;
                id.assign(href.begin()+href.find_last_of("/")+1, href.end());

                // Get path from download link
                std::string url = gogAPI->getExtraLink(gamename, id);
                url = htmlcxx::Uri::decode(url);
                if (url.find("/extras/") != std::string::npos)
                {
                    path.assign(url.begin()+url.find("/extras/"), url.begin()+url.find_first_of("?"));
                    path = "/" + gamename + path;
                }
                else
                {
                    path.assign(url.begin()+url.find_last_of("/")+1, url.begin()+url.find_first_of("?"));
                    path = "/" + gamename + "/extras/" + path;
                }

                // Get name from path
                name.assign(path.begin()+path.find_last_of("/")+1,path.end());

                if (name.empty())
                {
                    #ifdef DEBUG
                        std::cerr << "DEBUG INFO (getExtrasFromHTML)" << std::endl;
                        std::cerr << "Skipped file without a name (game: " << gamename << ", gameid: " << gameid << ", fileid: " << id << ")" << std::endl;
                    #endif
                    continue;
                }

                extras.push_back(
                                    gameFile (  false,
                                                id,
                                                name,
                                                path,
                                                std::string()
                                            )
                                 );
            }
        }
    }

    return extras;
}


std::string Downloader::getSerialsFromHTML(const std::string& html)
{
    std::ostringstream serials;

    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName() == "div")
        {
            it->parseAttributes();
            std::string classname = it->attribute("class").second;
            if (classname == "list_serial_h")
            {
                for (unsigned int i = 0; i < dom.number_of_children(it); ++i)
                {
                    tree<htmlcxx::HTML::Node>::iterator serials_it = dom.child(it, i);
                    if (!serials_it->isComment())
                    {
                        std::string tag_text;
                        if (!serials_it->isTag())
                        {
                            if (!serials_it->text().empty())
                                tag_text = serials_it->text();
                        }
                        else if (serials_it->tagName() == "span")
                        {
                            for (unsigned int j = 0; j < dom.number_of_children(serials_it); ++j)
                            {
                                tree<htmlcxx::HTML::Node>::iterator serials_span_it = dom.child(serials_it, j);
                                if (!serials_span_it->isTag() && !serials_span_it->isComment())
                                    tag_text = serials_span_it->text();
                            }
                        }

                        if (!tag_text.empty())
                        {
                            boost::regex expression("^\\h+|\\h+$");
                            std::string text = boost::regex_replace(tag_text, expression, "");
                            if (!text.empty())
                                serials << text << std::endl;
                        }
                    }
                }
            }
        }
    }

    return serials.str();
}

void Downloader::checkOrphans()
{
    // Always check everything when checking for orphaned files
    config.bInstallers = true;
    config.bExtras = true;
    config.bPatches = true;
    config.bLanguagePacks = true;

    if (this->games.empty())
        this->getGameDetails();

    std::vector<std::string> orphans;
    for (unsigned int i = 0; i < games.size(); ++i)
    {
        std::cout << "Checking for orphaned files " << i+1 << " / " << games.size() << "\r" << std::flush;
        std::vector<boost::filesystem::path> filepath_vector;

        try
        {
            std::vector<boost::filesystem::path> paths;
            std::vector<unsigned int> platformIds;
            platformIds.push_back(0);
            for (unsigned int j = 0; j < GlobalConstants::PLATFORMS.size(); ++j)
            {
                platformIds.push_back(GlobalConstants::PLATFORMS[j].platformId);
            }
            for (unsigned int j = 0; j < platformIds.size(); ++j)
            {
                std::string directory = config.sDirectory + "/" + config.sGameSubdir + "/";
                Util::filepathReplaceReservedStrings(directory, games[i].gamename, platformIds[j]);
                boost::filesystem::path path (directory);
                if (boost::filesystem::exists(path))
                {
                    bool bDuplicate = false;
                    for (unsigned int k = 0; k < paths.size(); ++k)
                    {
                        if (path == paths[k])
                        {
                            bDuplicate = true;
                            break;
                        }
                    }
                    if (!bDuplicate)
                        paths.push_back(path);
                }
            }

            for (unsigned int j = 0; j < paths.size(); ++j)
            {
                std::size_t pathlen = config.sDirectory.length();
                if (boost::filesystem::exists(paths[j]))
                {
                    if (boost::filesystem::is_directory(paths[j]))
                    {
                        // Recursively iterate over files in directory
                        boost::filesystem::recursive_directory_iterator end_iter;
                        boost::filesystem::recursive_directory_iterator dir_iter(paths[j]);
                        while (dir_iter != end_iter)
                        {
                            if (boost::filesystem::is_regular_file(dir_iter->status()))
                            {
                                std::string filepath = dir_iter->path().string();
                                if (config.blacklist.isBlacklisted(filepath.substr(pathlen))) {
                                    if (config.bVerbose)
                                        std::cout << "skipped blacklisted file " << filepath << std::endl;
                                } else {
                                    boost::regex expression(config.sOrphanRegex); // Limit to files matching the regex
                                    boost::match_results<std::string::const_iterator> what;
                                    if (boost::regex_search(filepath, what, expression))
                                        filepath_vector.push_back(dir_iter->path());
                                }
                            }
                            dir_iter++;
                        }
                    }
                }
                else
                    std::cout << paths[j] << " does not exist" << std::endl;
            }
        }
        catch (const boost::filesystem::filesystem_error& ex)
        {
            std::cout << ex.what() << std::endl;
        }

        if (!filepath_vector.empty())
        {
            for (unsigned int j = 0; j < filepath_vector.size(); ++j)
            {
                bool bFoundFile = false; // Assume that the file is orphaned

                // Check installers
                for (unsigned int k = 0; k < games[i].installers.size(); ++k)
                {
                    if (games[i].installers[k].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                    {
                        bFoundFile = true;
                        break;
                    }
                }
                if (!bFoundFile)
                {   // Check extras
                    for (unsigned int k = 0; k < games[i].extras.size(); ++k)
                    {
                        if (games[i].extras[k].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                        {
                            bFoundFile = true;
                            break;
                        }
                    }
                }
                if (!bFoundFile)
                {   // Check patches
                    for (unsigned int k = 0; k < games[i].patches.size(); ++k)
                    {
                        if (games[i].patches[k].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                        {
                            bFoundFile = true;
                            break;
                        }
                    }
                }
                if (!bFoundFile)
                {   // Check language packs
                    for (unsigned int k = 0; k < games[i].languagepacks.size(); ++k)
                    {
                        if (games[i].languagepacks[k].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                        {
                            bFoundFile = true;
                            break;
                        }
                    }
                }
                if (!bFoundFile)
                {   // Check dlcs
                    for (unsigned int k = 0; k < games[i].dlcs.size(); ++k)
                    {
                        for (unsigned int index = 0; index < games[i].dlcs[k].installers.size(); ++index)
                        {
                            if (games[i].dlcs[k].installers[index].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                            {
                                bFoundFile = true;
                                break;
                            }
                        }
                        if (bFoundFile) break;
                        for (unsigned int index = 0; index < games[i].dlcs[k].patches.size(); ++index)
                        {
                            if (games[i].dlcs[k].patches[index].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                            {
                                bFoundFile = true;
                                break;
                            }
                        }
                        for (unsigned int index = 0; index < games[i].dlcs[k].extras.size(); ++index)
                        {
                            if (games[i].dlcs[k].extras[index].path.find(filepath_vector[j].filename().string()) != std::string::npos)
                            {
                                bFoundFile = true;
                                break;
                            }
                        }
                        if (bFoundFile) break;
                    }
                }
                if (!bFoundFile)
                    orphans.push_back(filepath_vector[j].string());
            }
        }
    }
    std::cout << std::endl;

    if (!orphans.empty())
    {
        for (unsigned int i = 0; i < orphans.size(); ++i)
        {
            std::cout << orphans[i] << std::endl;
        }
    }
    else
    {
        std::cout << "No orphaned files" << std::endl;
    }

    return;
}

// Check status of files
void Downloader::checkStatus()
{
    if (this->games.empty())
        this->getGameDetails();

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        if (config.bInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                boost::filesystem::path filepath = games[i].installers[j].getFilepath();

                std::string remoteHash;
                std::string localHash;
                bool bHashOK = true; // assume hash OK
                size_t filesize;

                localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                remoteHash = this->getRemoteFileHash(games[i].gamename, games[i].installers[j].id);

                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                {
                    filesize = boost::filesystem::file_size(filepath);

                    if (remoteHash != localHash)
                        bHashOK = false;

                    std::cout << (bHashOK ? "OK " : "MD5 ") << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                }
                else
                {
                    std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                }
            }
        }

        if (config.bExtras)
        {
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                boost::filesystem::path filepath = games[i].extras[j].getFilepath();

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                {
                    filesize = boost::filesystem::file_size(filepath);
                    std::cout << "OK " << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                }
                else
                {
                    std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                }
            }
        }

        if (config.bPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                boost::filesystem::path filepath = games[i].patches[j].getFilepath();

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                {
                    filesize = boost::filesystem::file_size(filepath);
                    std::cout << "OK " << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                }
                else
                {
                    std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                }
            }
        }

        if (config.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                boost::filesystem::path filepath = games[i].languagepacks[j].getFilepath();

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                {
                    filesize = boost::filesystem::file_size(filepath);
                    std::cout << "OK " << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                }
                else
                {
                    std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                }
            }
        }

        if (config.bDLC)
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (config.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        boost::filesystem::path filepath = games[i].dlcs[j].installers[k].getFilepath();

                        std::string remoteHash;
                        std::string localHash;
                        bool bHashOK = true; // assume hash OK
                        size_t filesize;

                        localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        remoteHash = this->getRemoteFileHash(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);

                        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                        {
                            filesize = boost::filesystem::file_size(filepath);

                            if (remoteHash != localHash)
                                bHashOK = false;

                            std::cout << (bHashOK ? "OK " : "MD5 ") << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                        }
                        else
                        {
                            std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                        }
                    }
                }

                if (config.bPatches)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        boost::filesystem::path filepath = games[i].dlcs[j].patches[k].getFilepath();

                        std::string localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        size_t filesize;

                        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                        {
                            filesize = boost::filesystem::file_size(filepath);
                            std::cout << "OK " << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                        }
                        else
                        {
                            std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                        }
                    }
                }

                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        boost::filesystem::path filepath = games[i].dlcs[j].extras[k].getFilepath();

                        std::string localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        size_t filesize;

                        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                        {
                            filesize = boost::filesystem::file_size(filepath);
                            std::cout << "OK " << games[i].gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                        }
                        else
                        {
                            std::cout << "ND " << games[i].gamename << " " << filepath.filename().string() << std::endl;
                        }
                    }
                }
            }
        }
    }

    return;
}

std::string Downloader::getLocalFileHash(const std::string& filepath, const std::string& gamename)
{
    std::string localHash;
    boost::filesystem::path path = filepath;
    boost::filesystem::path local_xml_file;
    if (!gamename.empty())
        local_xml_file = config.sXMLDirectory + "/" + gamename + "/" + path.filename().string() + ".xml";
    else
        local_xml_file = config.sXMLDirectory + "/" + path.filename().string() + ".xml";

    if (boost::filesystem::exists(local_xml_file))
    {
        TiXmlDocument local_xml;
        local_xml.LoadFile(local_xml_file.string());
        TiXmlNode *fileNodeLocal = local_xml.FirstChild("file");
        if (fileNodeLocal)
        {
            TiXmlElement *fileElemLocal = fileNodeLocal->ToElement();
            localHash = fileElemLocal->Attribute("md5");
        }
    }
    else
    {
        if (boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path))
        {
            localHash = Util::getFileHash(path.string(), RHASH_MD5);
        }
    }
    return localHash;
}

std::string Downloader::getRemoteFileHash(const std::string& gamename, const std::string& id)
{
    std::string remoteHash;
    std::string xml_data = gogAPI->getXML(gamename, id);
    if (gogAPI->getError())
    {
        std::cout << gogAPI->getErrorMessage() << std::endl;
        gogAPI->clearError();
    }
    if (!xml_data.empty())
    {
        TiXmlDocument remote_xml;
        remote_xml.Parse(xml_data.c_str());
        TiXmlNode *fileNodeRemote = remote_xml.FirstChild("file");
        if (fileNodeRemote)
        {
            TiXmlElement *fileElemRemote = fileNodeRemote->ToElement();
            remoteHash = fileElemRemote->Attribute("md5");
        }
    }
    return remoteHash;
}

/* Load game details from cache file
    returns 0 if successful
    returns 1 if cache file doesn't exist
    returns 2 if JSON parsing failed
    returns 3 if cache is too old
    returns 4 if JSON doesn't contain "games" node
*/
int Downloader::loadGameDetailsCache()
{
    int res = 0;
    std::string cachepath = config.sCacheDirectory + "/gamedetails.json";

    // Make sure file exists
    boost::filesystem::path path = cachepath;
    if (!boost::filesystem::exists(path)) {
        return res = 1;
    }

    bptime::ptime now = bptime::second_clock::local_time();
    bptime::ptime cachedate;

    std::ifstream json(cachepath, std::ifstream::binary);
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    if (jsonparser->parse(json, root))
    {
        if (root.isMember("date"))
        {
            cachedate = bptime::from_iso_string(root["date"].asString());
            if ((now - cachedate) > bptime::minutes(config.iCacheValid))
            {
                // cache is too old
                delete jsonparser;
                json.close();
                return res = 3;
            }
        }

        if (root.isMember("games"))
        {
            this->games = getGameDetailsFromJsonNode(root["games"]);
            res = 0;
        }
        else
        {
            res = 4;
        }
    }
    else
    {
        res = 2;
        std::cout << "Failed to parse cache" << std::endl;
        std::cout << jsonparser->getFormatedErrorMessages() << std::endl;
    }
    delete jsonparser;
    if (json)
        json.close();

    return res;
}
/* Save game details to cache file
    returns 0 if successful
    returns 1 if fails
*/
int Downloader::saveGameDetailsCache()
{
    int res = 0;

    // Don't try to save cache if we don't have any game details
    if (this->games.empty())
    {
        return 1;
    }

    std::string cachepath = config.sCacheDirectory + "/gamedetails.json";

    Json::Value json;

    json["date"] = bptime::to_iso_string(bptime::second_clock::local_time());

    for (unsigned int i = 0; i < this->games.size(); ++i)
        json["games"].append(this->games[i].getDetailsAsJson());

    std::ofstream ofs(cachepath);
    if (!ofs)
    {
        res = 1;
    }
    else
    {
        Json::StyledStreamWriter jsonwriter;
        jsonwriter.write(ofs, json);
        ofs.close();
    }
    return res;
}

std::vector<gameDetails> Downloader::getGameDetailsFromJsonNode(Json::Value root, const int& recursion_level)
{
    std::vector<gameDetails> details;

    // If root node is not array and we use root.size() it will return the number of nodes --> limit to 1 "array" node to make sure it is handled properly
    for (unsigned int i = 0; i < (root.isArray() ? root.size() : 1); ++i)
    {
        Json::Value gameDetailsNode = (root.isArray() ? root[i] : root); // This json node can be array or non-array so take that into account
        gameDetails game;
        game.gamename = gameDetailsNode["gamename"].asString();

        // DLCs are handled as part of the game so make sure that filtering is done with base game name
        if (recursion_level == 0) // recursion level is 0 when handling base game
        {
            boost::regex expression(config.sGameRegex);
            boost::match_results<std::string::const_iterator> what;
            if (!boost::regex_search(game.gamename, what, expression)) // Check if name matches the specified regex
                continue;
        }
        game.title = gameDetailsNode["title"].asString();
        game.icon = gameDetailsNode["icon"].asString();
        game.serials = gameDetailsNode["serials"].asString();

        // Make a vector of valid node names to make things easier
        std::vector<std::string> nodes;
        nodes.push_back("extras");
        nodes.push_back("installers");
        nodes.push_back("patches");
        nodes.push_back("languagepacks");
        nodes.push_back("dlcs");

        gameSpecificConfig conf;
        conf.bDLC = config.bDLC;
        conf.iInstallerLanguage = config.iInstallerLanguage;
        conf.iInstallerType = config.iInstallerType;
        if (Util::getGameSpecificConfig(game.gamename, &conf) > 0)
            std::cout << game.gamename << " - Language: " << conf.iInstallerLanguage << ", Platform: " << conf.iInstallerType << ", DLC: " << (conf.bDLC ? "true" : "false") << std::endl;

        for (unsigned int j = 0; j < nodes.size(); ++j)
        {
            std::string nodeName = nodes[j];
            if (gameDetailsNode.isMember(nodeName))
            {
                Json::Value fileDetailsNodeVector = gameDetailsNode[nodeName];
                for (unsigned int index = 0; index < fileDetailsNodeVector.size(); ++index)
                {
                    Json::Value fileDetailsNode = fileDetailsNodeVector[index];
                    gameFile fileDetails;

                    if (nodeName != "dlcs")
                    {
                        fileDetails.updated = fileDetailsNode["updated"].asInt();
                        fileDetails.id = fileDetailsNode["id"].asString();
                        fileDetails.name = fileDetailsNode["name"].asString();
                        fileDetails.path = fileDetailsNode["path"].asString();
                        fileDetails.size = fileDetailsNode["size"].asString();
                        fileDetails.platform = fileDetailsNode["platform"].asUInt();
                        fileDetails.language = fileDetailsNode["language"].asUInt();
                        fileDetails.silent = fileDetailsNode["silent"].asInt();

                        if (nodeName != "extras" && !(fileDetails.platform & conf.iInstallerType))
                            continue;
                        if (nodeName != "extras" && !(fileDetails.language & conf.iInstallerLanguage))
                            continue;
                    }

                    if (nodeName == "extras" && config.bExtras)
                        game.extras.push_back(fileDetails);
                    else if (nodeName == "installers" && config.bInstallers)
                        game.installers.push_back(fileDetails);
                    else if (nodeName == "patches" && config.bPatches)
                        game.patches.push_back(fileDetails);
                    else if (nodeName == "languagepacks" && config.bLanguagePacks)
                        game.languagepacks.push_back(fileDetails);
                    else if (nodeName == "dlcs" && conf.bDLC)
                        game.dlcs = this->getGameDetailsFromJsonNode(fileDetailsNode, recursion_level + 1);
                }
            }
        }
        if (!game.extras.empty() || !game.installers.empty() || !game.patches.empty() || !game.languagepacks.empty() || !game.dlcs.empty())
            {
                game.filterWithPriorities(config);
                details.push_back(game);
            }
    }
    return details;
}

void Downloader::updateCache()
{
    // Make sure that all details get cached
    unsigned int all_platforms = GlobalConstants::PLATFORM_WINDOWS;
    unsigned int all_languages = GlobalConstants::LANGUAGE_EN;
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
        all_platforms |= GlobalConstants::PLATFORMS[i].platformId;
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
        all_languages |= GlobalConstants::LANGUAGES[i].languageId;

    config.bExtras = true;
    config.bInstallers = true;
    config.bPatches = true;
    config.bLanguagePacks = true;
    config.bDLC = true;
    config.sGameRegex = ".*";
    config.iInstallerLanguage = all_languages;
    config.iInstallerType = all_platforms;
    config.vLanguagePriority.clear();
    config.vPlatformPriority.clear();

    this->getGameList();
    this->getGameDetails();
    if (this->saveGameDetailsCache())
        std::cout << "Failed to save cache" << std::endl;

    return;
}

// Save serials to file
void Downloader::saveSerials(const std::string& serials, const std::string& filepath)
{
    bool bFileExists = boost::filesystem::exists(filepath);

    if (bFileExists)
        return;

    // Get directory from filepath
    boost::filesystem::path pathname = filepath;
    std::string directory = pathname.parent_path().string();

    // Check that directory exists and create subdirectories
    boost::filesystem::path path = directory;
    if (boost::filesystem::exists(path))
    {
        if (!boost::filesystem::is_directory(path))
        {
            std::cout << path << " is not directory" << std::endl;
            return;
        }
    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return;
        }
    }

    std::ofstream ofs(filepath);
    if (ofs)
    {
        std::cout << "Saving serials: " << filepath << std::endl;
        ofs << serials;
        ofs.close();
    }
    else
    {
        std::cout << "Failed to create file: " << filepath << std::endl;
    }

    return;
}
