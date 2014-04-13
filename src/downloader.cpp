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
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <tinyxml.h>
#include <jsoncpp/json/json.h>
#include <htmlcxx/html/ParserDom.h>
#include <htmlcxx/html/Uri.h>

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
    if (config.bLogin || !bInitOK)
        return this->login();

    if (config.bCover && config.bDownload && !config.bUpdateCheck)
        coverXML = this->getResponse("https://sites.google.com/site/gogdownloader/covers.xml");

    if (!config.bUpdateCheck) // updateCheck() calls getGameList() if needed
        this->getGameList();

    if (config.bReport && (config.bDownload || config.bRepair))
    {
        this->report_ofs.open("lgogdownloader-report.log");
        if (!this->report_ofs)
        {
            std::cout << "Failed to create lgogdownloader-report.log" << std::endl;
            return 1;
        }
    }

    return 0;
}

/* Login
    returns 1 if login fails
    returns 0 if successful
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
        return 1;
    }
    else
    {
        // Login to website
        if (!HTTP_Login(email, password))
        {
            std::cout << "HTTP: Login failed" << std::endl;
            return 1;
        }
        else
        {
            std::cout << "HTTP: Login successful" << std::endl;
        }
        // Login to API
        if (!gogAPI->login(email, password))
        {
            std::cout << "API: Login failed" << std::endl;
            return 1;
        }
        else
        {
            std::cout << "API: Login successful" << std::endl;
            // Save token and secret to config file
            std::ofstream ofs(config.sConfigFilePath.c_str());
            if (ofs)
            {
                ofs << "token = " << gogAPI->getToken() << std::endl << "secret = " << gogAPI->getSecret() << std::endl;
                ofs.close();
                return 0;
            }
            else
            {
                std::cout << "Failed to create config: " << config.sConfigFilePath << std::endl;
                return 1;
            }
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
    gameDetails game;
    int updated = 0;
    for (unsigned int i = 0; i < gameItems.size(); ++i)
    {
        std::cout << "Getting game info " << i+1 << " / " << gameItems.size() << "\r" << std::flush;
        bool bHasDLC = !gameItems[i].dlcnames.empty();
        game = gogAPI->getGameDetails(gameItems[i].name, config.iInstallerType, config.iInstallerLanguage, config.bDuplicateHandler);
        if (!gogAPI->getError())
        {
            if (game.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
            {
                game.extras = this->getExtras(gameItems[i].name, gameItems[i].id);
            }
            if (game.dlcs.empty() && bHasDLC && config.bDLC)
            {
                for (unsigned int j = 0; j < gameItems[i].dlcnames.size(); ++j)
                {
                    gameDetails dlc;
                    dlc = gogAPI->getGameDetails(gameItems[i].dlcnames[j], config.iInstallerType, config.iInstallerLanguage, config.bDuplicateHandler);
                    if (dlc.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
                    {
                        dlc.extras = this->getExtras(gameItems[i].dlcnames[j], gameItems[i].id);
                    }
                    game.dlcs.push_back(dlc);
                }
            }
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
            return 1;
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
            // List installers
            if (config.bInstallers)
            {
                std::cout << "installers: " << std::endl;
                for (unsigned int j = 0; j < games[i].installers.size(); ++j)
                {
                    if (!config.bUpdateCheck || games[i].installers[j].updated) // Always list updated files
                    {
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
                    std::string languages;
                    for (unsigned int k = 0; k < GlobalConstants::LANGUAGES.size(); k++) // Check which languages the installer supports
                    {
                        if (games[i].installers[j].language & GlobalConstants::LANGUAGES[k].languageId)
                            languages += (languages.empty() ? "" : ", ")+GlobalConstants::LANGUAGES[k].languageString;
                    }

                    std::cout   << "\tid: " << games[i].patches[j].id << std::endl
                                << "\tname: " << games[i].patches[j].name << std::endl
                                << "\tpath: " << games[i].patches[j].path << std::endl
                                << "\tsize: " << games[i].patches[j].size << std::endl
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
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].installers[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].installers[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].installers[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].installers[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].patches[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].patches[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].patches[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].patches[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
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
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[j].path, games[i].gamename);

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
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].extras[j].path, games[i].gamename, config.bSubDirectories ? "extras" : "");

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
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].patches[j].path, games[i].gamename, config.bSubDirectories ? "patches" : "");

                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
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

        // Language packs (GOG doesn't provide XML data for language packs, use local file)
        if (config.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].languagepacks[j].path, games[i].gamename, config.bSubDirectories ? "languagepacks" : "");

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
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].installers[k].path, games[i].gamename, config.bSubDirectories ? "dlc" : "");

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
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].patches[k].path, games[i].gamename, config.bSubDirectories ? "dlc/patches" : "");

                        std::string url = gogAPI->getPatchLink(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
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
                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].extras[k].path, games[i].gamename, config.bSubDirectories ? "dlc/extras" : "");

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
        // Download covers
        if (config.bCover && !config.bUpdateCheck)
        {
            if (!games[i].installers.empty())
            {
                // Take path from installer path because for some games the base directory for installer/extra path is not "gamename"
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[0].path, games[i].gamename);

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

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[j].path, games[i].gamename);

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
                        XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                    if (!games[i].installers[j].name.empty())
                        std::cout << "Dowloading: " << games[i].installers[j].name << std::endl;
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

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].extras[j].path, games[i].gamename, config.bSubDirectories ? "extras" : "");

                // Download
                if (!url.empty())
                {
                    if (!games[i].extras[j].name.empty())
                        std::cout << "Dowloading: " << games[i].extras[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    CURLcode result = downloadFile(url, filepath);
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
                // Get link
                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].patches[j].path, games[i].gamename, config.bSubDirectories ? "patches" : "");

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (config.bRemoteXML)
                        XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                    if (!games[i].patches[j].name.empty())
                        std::cout << "Dowloading: " << games[i].patches[j].name << std::endl;
                    CURLcode result = this->downloadFile(url, filepath, XML, games[i].gamename);
                    std::cout << std::endl;
                    if (result==CURLE_OK && config.sXMLFile == "automatic" && XML.empty())
                    {
                        std::cout << "Starting automatic XML creation" << std::endl;
                        std::string xml_dir = config.sXMLDirectory + "/" + games[i].gamename;
                        Util::createXML(filepath, config.iChunkSize, xml_dir);
                        std::cout << std::endl;
                    }
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

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].languagepacks[j].path, games[i].gamename, config.bSubDirectories ? "languagepacks" : "");

                // Download
                if (!url.empty())
                {
                    if (!games[i].languagepacks[j].name.empty())
                        std::cout << "Dowloading: " << games[i].gamename << " " << games[i].languagepacks[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    CURLcode result = downloadFile(url, filepath);
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
        if (config.bDLC && !games[i].dlcs.empty())
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (config.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].installers[k].path, games[i].gamename, config.bSubDirectories ? "dlc" : "");

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
                                XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                            if (!games[i].dlcs[j].installers[k].name.empty())
                                std::cout << "Dowloading: " << games[i].dlcs[j].installers[k].name << std::endl;
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
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].patches[k].path, games[i].gamename, config.bSubDirectories ? "dlc/patches" : "");

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
                                XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                            if (!games[i].dlcs[j].patches[k].name.empty())
                                std::cout << "Dowloading: " << games[i].dlcs[j].patches[k].name << std::endl;
                            CURLcode result = this->downloadFile(url, filepath, XML, games[i].dlcs[j].gamename);
                            std::cout << std::endl;
                            if (result==CURLE_OK && config.sXMLFile == "automatic" && XML.empty())
                            {
                                std::cout << "Starting automatic XML creation" << std::endl;
                                std::string xml_dir = config.sXMLDirectory + "/" + games[i].dlcs[j].gamename;
                                Util::createXML(filepath, config.iChunkSize, xml_dir);
                                std::cout << std::endl;
                            }
                        }
                    }
                }
                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].extras[k].path, games[i].gamename, config.bSubDirectories ? "dlc/extras" : "");

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
    std::string localHash = this->getLocalFileHash(filepath);

    if (!xml_data.empty())
    {
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
            boost::filesystem::path new_name = filepath + ".old"; // Rename old file by appending ".old" to filename
            if (boost::filesystem::exists(new_name))
            {   // One even older file exists, delete the older file before renaming
                std::cout << "Old renamed file found, deleting old file" << std::endl;
                if (!boost::filesystem::remove(new_name))
                {
                    std::cout << "Failed to delete " << new_name.string() << std::endl;
                    std::cout << "Skipping file" << std::endl;
                    return res;
                }
            }
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
            boost::filesystem::path path = filepath;
            if (!boost::filesystem::remove(path))
            {
                std::cout << "Failed to delete " << path << std::endl;
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

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    CURLcode result = curl_easy_perform(curlhandle);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    std::string response = memory.str();
    memory.str(std::string());

    if (result != CURLE_OK)
        std::cout << curl_easy_strerror(result) << std::endl;

    return response;
}

int Downloader::progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    // on entry: dltotal - how much remains to download till the end of the file (bytes)
    //           dlnow   - how much was downloaded from the start of the program (bytes)
    unsigned int bar_length  = 26;
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
        downloader->progressbar->draw(bar_length, fraction);

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
        printf(" %0.2f/%0.2fMB @ %0.2f%s ETA: %s\r", dlnow/1024/1024, dltotal/1024/1024, rate, rate_unit.c_str(), eta_ss.str().c_str());
        fflush(stdout);
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
    std::string buk;

    // Get "buk" for login form
    std::string json = this->getResponse("http://www.gog.com/user/ajax/?a=get");

    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    bool parsingSuccessful = jsonparser->parse(json, root);
    if (!parsingSuccessful)
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        return res = 0;
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << root << std::endl;
    #endif
    buk = root["buk"].asString();

    //Create postdata - escape characters in email/password to support special characters
    postdata = "log_email=" + (std::string)curl_easy_escape(curlhandle, email.c_str(),email.size())
            + "&log_password=" + (std::string)curl_easy_escape(curlhandle, password.c_str(), password.size())
            + "&buk=" + (std::string)curl_easy_escape(curlhandle, buk.c_str(), buk.size());
    curl_easy_setopt(curlhandle, CURLOPT_URL, "https://secure.gog.com/login");
    curl_easy_setopt(curlhandle, CURLOPT_POST, 1);
    curl_easy_setopt(curlhandle, CURLOPT_POSTFIELDS, postdata.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
    CURLcode result = curl_easy_perform(curlhandle);
    memory.str(std::string());

    if (result != CURLE_OK)
    {
        // Expected to hit maximum amount of redirects so don't print error on it
        if (result != CURLE_TOO_MANY_REDIRECTS)
            std::cout << curl_easy_strerror(result) << std::endl;
    }

    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    json = this->getResponse("http://www.gog.com/user/ajax/?a=get");

    parsingSuccessful = jsonparser->parse(json, root);
    if (!parsingSuccessful)
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        return res = 0;
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << root << std::endl;
    #endif

    if (root["user"]["email"].asString() == email && (result == CURLE_OK || result == CURLE_TOO_MANY_REDIRECTS))
        res = 1; // Login successful
    else
        res = 0; // Login failed

    delete jsonparser;

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
        std::string response = this->getResponse("https://secure.gog.com/en/account/ajax?a=gamesShelfMore&s=title&q=&t=0&p=" + std::to_string(i));

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

                                        std::string gameDataUrl = "https://secure.gog.com/en/account/ajax?a=gamesListDetails&g=" + game.id;
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
                                                std::string search_string = "/downlink/file/"; // Extra links: https://secure.gog.com/downlink/file/gamename/id_number
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
    std::string json = this->getResponse("https://secure.gog.com/games/ajax?a=search&f={\"price\":[\"free\"],\"sort\":\"title\"}&p=1&t=all");

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
    std::string html = root["result"]["html"].asString();
    delete jsonparser;

    // Parse HTML to get game names
    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="span")
        {
            it->parseAttributes();
            std::string classname = it->attribute("class").second;
            if (classname=="gog-price game-owned")
            {
                gameItem game;
                // Game name is contained in data-gameindex attribute
                game.name = it->attribute("data-gameindex").second;
                game.id = it->attribute("data-gameid").second;
                if (!game.name.empty() && !game.id.empty())
                    games.push_back(game);
            }
        }
    }

    return games;
}

std::vector<gameFile> Downloader::getExtras(const std::string& gamename, const std::string& gameid)
{
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    std::vector<gameFile> extras;

    std::string gameDataUrl = "https://secure.gog.com/en/account/ajax?a=gamesListDetails&g=" + gameid;
    std::string json = this->getResponse(gameDataUrl);
    // Parse JSON
    if (!jsonparser->parse(json, root))
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::getExtras)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        delete jsonparser;
        exit(1);
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::getExtras)" << std::endl << root << std::endl;
    #endif
    std::string html = root["details"]["html"].asString();
    delete jsonparser;

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
            // Extra links https://secure.gog.com/downlink/file/gamename/id_number
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
        boost::filesystem::path path (config.sDirectory + games[i].gamename);
        std::vector<boost::filesystem::path> filepath_vector;

        try
        {
            if (boost::filesystem::exists(path))
            {
                if (boost::filesystem::is_directory(path))
                {
                    // Recursively iterate over files in directory
                    boost::filesystem::recursive_directory_iterator end_iter;
                    boost::filesystem::recursive_directory_iterator dir_iter(path);
                    while (dir_iter != end_iter)
                    {
                        if (boost::filesystem::is_regular_file(dir_iter->status()))
                        {
                            std::string filename = dir_iter->path().filename().string();
                            boost::regex expression(config.sOrphanRegex); // Limit to files matching the regex
                            boost::match_results<std::string::const_iterator> what;
                            if (boost::regex_search(filename, what, expression))
                                filepath_vector.push_back(dir_iter->path());
                        }
                        dir_iter++;
                    }
                }
            }
            else
                std::cout << path << " does not exist" << std::endl;
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
                boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].installers[j].path, games[i].gamename);

                std::string remoteHash;
                std::string localHash;
                bool bHashOK = true; // assume hash OK
                size_t filesize;

                localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                remoteHash = this->getRemoteFileHash(games[i].gamename, games[i].installers[j].id);

                if (boost::filesystem::exists(filepath))
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
                boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].extras[j].path, games[i].gamename, config.bSubDirectories ? "extras" : "");

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath))
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
                boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].patches[j].path, games[i].gamename, config.bSubDirectories ? "patches" : "");

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath))
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
                boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].languagepacks[j].path, games[i].gamename, config.bSubDirectories ? "languagepacks" : "");

                std::string localHash = this->getLocalFileHash(filepath.string(), games[i].gamename);
                size_t filesize;

                if (boost::filesystem::exists(filepath))
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
                        boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].installers[k].path, games[i].gamename, config.bSubDirectories ? "dlc" : "");

                        std::string remoteHash;
                        std::string localHash;
                        bool bHashOK = true; // assume hash OK
                        size_t filesize;

                        localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        remoteHash = this->getRemoteFileHash(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);

                        if (boost::filesystem::exists(filepath))
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
                        boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].patches[k].path, games[i].gamename, config.bSubDirectories ? "dlc/patches" : "");

                        std::string localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        size_t filesize;

                        if (boost::filesystem::exists(filepath))
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
                        boost::filesystem::path filepath = Util::makeFilepath(config.sDirectory, games[i].dlcs[j].extras[k].path, games[i].gamename, config.bSubDirectories ? "dlc/extras" : "");

                        std::string localHash = this->getLocalFileHash(filepath.string(), games[i].dlcs[j].gamename);
                        size_t filesize;

                        if (boost::filesystem::exists(filepath))
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
        if (boost::filesystem::exists(path))
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
