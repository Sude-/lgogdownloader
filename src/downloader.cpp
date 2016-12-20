/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "util.h"
#include "globalconstants.h"
#include "downloadinfo.h"
#include "message.h"

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
#include <tinyxml2.h>
#include <json/json.h>
#include <htmlcxx/html/ParserDom.h>
#include <htmlcxx/html/Uri.h>
#include <termios.h>
#include <algorithm>
#include <thread>
#include <mutex>

namespace bptime = boost::posix_time;

std::vector<DownloadInfo> vDownloadInfo;
ThreadSafeQueue<gameFile> dlQueue;
ThreadSafeQueue<Message> msgQueue;
ThreadSafeQueue<gameFile> createXMLQueue;
ThreadSafeQueue<gameItem> gameItemQueue;
ThreadSafeQueue<gameDetails> gameDetailsQueue;
std::mutex mtx_create_directories; // Mutex for creating directories in Downloader::processDownloadQueue

Downloader::Downloader(Config &conf)
{
    this->config = conf;
    if (config.bLoginHTTP && boost::filesystem::exists(config.sCookiePath))
        if (!boost::filesystem::remove(config.sCookiePath))
            std::cerr << "Failed to delete " << config.sCookiePath << std::endl;

    this->resume_position = 0;
    this->retries = 0;

    // Initialize curl and set curl options
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, config.sVersionString.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, config.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, config.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, config.iDownloadRate);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallback);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFODATA, this);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_TIME, 30);
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_LIMIT, 200);

    if (!config.sCACertPath.empty())
        curl_easy_setopt(curlhandle, CURLOPT_CAINFO, config.sCACertPath.c_str());

    // Create new GOG website handle
    gogWebsite = new Website(config);

    // Create new API handle and set curl options for the API
    gogAPI = new API(config.sToken, config.sSecret);
    gogAPI->curlSetOpt(CURLOPT_VERBOSE, config.bVerbose);
    gogAPI->curlSetOpt(CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    gogAPI->curlSetOpt(CURLOPT_CONNECTTIMEOUT, config.iTimeout);
    if (!config.sCACertPath.empty())
        gogAPI->curlSetOpt(CURLOPT_CAINFO, config.sCACertPath.c_str());

    gogAPI->init();

    progressbar = new ProgressBar(config.bUnicode, config.bColor);
}

Downloader::~Downloader()
{
    if (config.bReport)
        if (this->report_ofs)
            this->report_ofs.close();
    delete progressbar;
    delete gogAPI;
    delete gogWebsite;
    curl_easy_cleanup(curlhandle);
    // Make sure that cookie file is only readable/writable by owner
    if (!config.bRespectUmask)
        Util::setFilePermissions(config.sCookiePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
}

/* Login check
    returns false if not logged in
    returns true if logged in
*/
bool Downloader::isLoggedIn()
{
    bool bIsLoggedIn = false;
    config.bLoginAPI = false;
    config.bLoginHTTP = false;

    bool bWebsiteIsLoggedIn = gogWebsite->IsLoggedIn();
    if (!bWebsiteIsLoggedIn)
        config.bLoginHTTP = true;

    bool bIsLoggedInAPI = gogAPI->isLoggedIn();
    if (!bIsLoggedInAPI)
        config.bLoginAPI = true;

    if (bIsLoggedInAPI && bWebsiteIsLoggedIn)
        bIsLoggedIn = true;

    return bIsLoggedIn;
}

/* Initialize the downloader
    returns 0 if failed
    returns 1 if successful
*/
int Downloader::init()
{
    if (!config.sGameHasDLCList.empty())
    {
        if (config.gamehasdlc.empty())
        {
            std::string game_has_dlc_list = this->getResponse(config.sGameHasDLCList);
            if (!game_has_dlc_list.empty())
                config.gamehasdlc.initialize(Util::tokenize(game_has_dlc_list, "\n"));
        }
    }
    gogWebsite->setConfig(config); // Update config for website handle

    if (config.bReport && (config.bDownload || config.bRepair))
    {
        this->report_ofs.open(config.sReportFilePath);
        if (!this->report_ofs)
        {
            config.bReport = false;
            std::cerr << "Failed to create " << config.sReportFilePath << std::endl;
            return 0;
        }
    }

    return 1;
}

/* Login
    returns 0 if login fails
    returns 1 if successful
*/
int Downloader::login()
{
    std::string email;
    if (!isatty(STDIN_FILENO)) {
        std::cerr << "Unable to read email and password" << std::endl;
        return 0;
    }
    std::cerr << "Email: ";
    std::getline(std::cin,email);

    std::string password;
    std::cerr << "Password: ";
    struct termios termios_old, termios_new;
    tcgetattr(STDIN_FILENO, &termios_old); // Get current terminal attributes
    termios_new = termios_old;
    termios_new.c_lflag &= ~ECHO; // Set ECHO off
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_new); // Set terminal attributes
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSANOW, &termios_old); // Restore old terminal attributes
    std::cerr << std::endl;

    if (email.empty() || password.empty())
    {
        std::cerr << "Email and/or password empty" << std::endl;
        return 0;
    }
    else
    {
        // Login to website
        if (config.bLoginHTTP)
        {
            // Delete old cookies
            if (boost::filesystem::exists(config.sCookiePath))
                if (!boost::filesystem::remove(config.sCookiePath))
                    std::cerr << "Failed to delete " << config.sCookiePath << std::endl;

            if (!gogWebsite->Login(email, password))
            {
                std::cerr << "HTTP: Login failed" << std::endl;
                return 0;
            }
            else
            {
                std::cerr << "HTTP: Login successful" << std::endl;
                if (!config.bLoginAPI)
                    return 1;
            }
        }
        // Login to API
        if (config.bLoginAPI)
        {
            if (!gogAPI->login(email, password))
            {
                std::cerr << "API: Login failed" << std::endl;
                return 0;
            }
            else
            {
                std::cerr << "API: Login successful" << std::endl;
                config.sToken = gogAPI->getToken();
                config.sSecret = gogAPI->getSecret();
                return 1;
            }
        }
    }
    return 0;
}

void Downloader::updateCheck()
{
    std::cout << "New forum replies: " << gogAPI->user.notifications_forum << std::endl;
    std::cout << "New private messages: " << gogAPI->user.notifications_messages << std::endl;
    std::cout << "Updated games: " << gogAPI->user.notifications_games << std::endl;

    if (gogAPI->user.notifications_games)
    {
        config.sGameRegex = ".*"; // Always check all games
        gogWebsite->setConfig(config); // Make sure that website handle has updated config
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
    if (config.sGameRegex == "free")
    {
        gameItems = gogWebsite->getFreeGames();
    }
    else
    {
        gameItems = gogWebsite->getGames();
    }
}

/* Get detailed info about the games
    returns 0 if successful
    returns 1 if fails
*/
int Downloader::getGameDetails()
{
    // Set default game specific directory options to values from config
    gameSpecificDirectoryConfig dirConfDefault;
    dirConfDefault.sDirectory = config.sDirectory;
    dirConfDefault.bSubDirectories = config.bSubDirectories;
    dirConfDefault.sGameSubdir = config.sGameSubdir;
    dirConfDefault.sInstallersSubdir = config.sInstallersSubdir;
    dirConfDefault.sExtrasSubdir = config.sExtrasSubdir;
    dirConfDefault.sLanguagePackSubdir = config.sLanguagePackSubdir;
    dirConfDefault.sDLCSubdir = config.sDLCSubdir;
    dirConfDefault.sPatchesSubdir = config.sPatchesSubdir;

    if (config.bUseCache && !config.bUpdateCache)
    {
        // GameRegex filter alias for all games
        if (config.sGameRegex == "all")
            config.sGameRegex = ".*";
        else if (config.sGameRegex == "free")
            std::cerr << "Warning: regex alias \"free\" doesn't work with cached details" << std::endl;

        int result = this->loadGameDetailsCache();
        if (result == 0)
        {
            for (unsigned int i = 0; i < this->games.size(); ++i)
            {
                gameSpecificConfig conf;
                conf.dirConf = dirConfDefault;
                Util::getGameSpecificConfig(games[i].gamename, &conf);
                this->games[i].makeFilepaths(conf.dirConf);
            }
            return 0;
        }
        else
        {
            if (result == 1)
            {
                std::cerr << "Cache doesn't exist." << std::endl;
                std::cerr << "Create cache with --update-cache" << std::endl;
            }
            else if (result == 3)
            {
                std::cerr << "Cache is too old." << std::endl;
                std::cerr << "Update cache with --update-cache or use bigger --cache-valid" << std::endl;
            }
            else if (result == 5)
            {
                std::cerr << "Cache version doesn't match current version." << std::endl;
                std::cerr << "Update cache with --update-cache" << std::endl;
            }
            return 1;
        }
    }

    if (gameItems.empty())
        this->getGameList();

    if (!gameItems.empty())
    {
        for (unsigned int i = 0; i < gameItems.size(); ++i)
        {
            gameItemQueue.push(gameItems[i]);
        }

        // Create threads
        unsigned int threads = std::min(config.iThreads, static_cast<unsigned int>(gameItemQueue.size()));
        std::vector<std::thread> vThreads;
        for (unsigned int i = 0; i < threads; ++i)
        {
            DownloadInfo dlInfo;
            dlInfo.setStatus(DLSTATUS_NOTSTARTED);
            vDownloadInfo.push_back(dlInfo);
            vThreads.push_back(std::thread(Downloader::getGameDetailsThread, this->config, i));
        }

        unsigned int dl_status = DLSTATUS_NOTSTARTED;
        while (dl_status != DLSTATUS_FINISHED)
        {
            dl_status = DLSTATUS_NOTSTARTED;

            // Print progress information once per 100ms
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cerr << "\033[J\r" << std::flush; // Clear screen from the current line down to the bottom of the screen

            // Print messages from message queue first
            Message msg;
            while (msgQueue.try_pop(msg))
            {
                std::cerr << msg.getFormattedString(config.bColor, true) << std::endl;
                if (config.bReport)
                {
                    this->report_ofs << msg.getTimestampString() << ": " << msg.getMessage() << std::endl;
                }
            }

            for (unsigned int i = 0; i < vDownloadInfo.size(); ++i)
            {
                unsigned int status = vDownloadInfo[i].getStatus();
                dl_status |= status;
            }

            std::cerr << "Getting game info " << (gameItems.size() - gameItemQueue.size()) << " / " << gameItems.size() << std::endl;

            if (dl_status != DLSTATUS_FINISHED)
            {
                std::cerr << "\033[1A\r" << std::flush; // Move cursor up by 1 row
            }
        }

        // Join threads
        for (unsigned int i = 0; i < vThreads.size(); ++i)
            vThreads[i].join();

        vThreads.clear();
        vDownloadInfo.clear();

        gameDetails details;
        while (gameDetailsQueue.try_pop(details))
        {
            this->games.push_back(details);
        }
        std::sort(this->games.begin(), this->games.end(), [](const gameDetails& i, const gameDetails& j) -> bool { return i.gamename < j.gamename; });
    }

    return 0;
}

int Downloader::listGames()
{
    if (config.bListDetails) // Detailed list
    {
        if (this->games.empty()) {
            int res = this->getGameDetails();
            if (res > 0)
                return res;
        }

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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::string languages = Util::getOptionNameString(games[i].installers[j].language, GlobalConstants::LANGUAGES);

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
                            std::cerr << "skipped blacklisted file " << filepath << std::endl;
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
                            std::cerr << "skipped blacklisted file " << filepath << std::endl;
                        continue;
                    }

                    std::string languages = Util::getOptionNameString(games[i].patches[j].language, GlobalConstants::LANGUAGES);

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
                            std::cerr << "skipped blacklisted file " << filepath << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].extras[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].extras[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].extras[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].extras[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].languagepacks.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].languagepacks[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tid: " << games[i].dlcs[j].languagepacks[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].languagepacks[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].languagepacks[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].languagepacks[k].size << std::endl
                                    << std::endl;
                    }
                }
            }
        }
    }
    else
    {   // List game names
        if (gameItems.empty())
            this->getGameList();

        for (unsigned int i = 0; i < gameItems.size(); ++i)
        {
            std::cout << gameItems[i].name << std::endl;
            for (unsigned int j = 0; j < gameItems[i].dlcnames.size(); ++j)
                std::cout << "+> " << gameItems[i].dlcnames[j] << std::endl;
        }
    }

    return 0;
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
                        std::cerr << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Get XML data
                std::string XML = "";
                if (config.bRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                    if (gogAPI->getError())
                    {
                        std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                        std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                        std::cerr << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                std::string url = gogAPI->getExtraLink(games[i].gamename, games[i].extras[j].id);
                if (gogAPI->getError())
                {
                    std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                        std::cerr << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                // Get XML data
                std::string XML = "";
                if (config.bRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                    if (gogAPI->getError())
                    {
                        std::cerr << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                    }
                }

                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                if (gogAPI->getError())
                {
                    std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                        std::cerr << "skipped blacklisted file " << filepath << std::endl;
                    continue;
                }

                std::string url = gogAPI->getLanguagePackLink(games[i].gamename, games[i].languagepacks[j].id);
                if (gogAPI->getError())
                {
                    std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get XML data
                        std::string XML = "";
                        if (config.bRemoteXML)
                        {
                            XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].installers[k].id);
                            if (gogAPI->getError())
                            {
                                std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                                std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get XML data
                        std::string XML = "";
                        if (config.bRemoteXML)
                        {
                            XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                            if (gogAPI->getError())
                            {
                                std::cerr << gogAPI->getErrorMessage() << std::endl;
                                gogAPI->clearError();
                            }
                        }

                        std::string url = gogAPI->getPatchLink(games[i].dlcs[j].gamename, games[i].dlcs[j].patches[k].id);
                        if (gogAPI->getError())
                        {
                            std::cerr << gogAPI->getErrorMessage() << std::endl;
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
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::string url = gogAPI->getExtraLink(games[i].dlcs[j].gamename, games[i].dlcs[j].extras[k].id);
                        if (gogAPI->getError())
                        {
                            std::cerr << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }
                        std::cout << "Repairing file " << filepath << std::endl;
                        this->repairFile(url, filepath, std::string(), games[i].dlcs[j].gamename);
                        std::cout << std::endl;
                    }
                }
                if (config.bLanguagePacks)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].languagepacks.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].languagepacks[k].getFilepath();
                        if (config.blacklist.isBlacklisted(filepath)) {
                            if (config.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        // Get XML data
                        std::string XML = "";
                        if (config.bRemoteXML)
                        {
                            XML = gogAPI->getXML(games[i].dlcs[j].gamename, games[i].dlcs[j].languagepacks[k].id);
                            if (gogAPI->getError())
                            {
                                std::cerr << gogAPI->getErrorMessage() << std::endl;
                                gogAPI->clearError();
                            }
                        }

                        std::string url = gogAPI->getLanguagePackLink(games[i].dlcs[j].gamename, games[i].dlcs[j].languagepacks[k].id);
                        if (gogAPI->getError())
                        {
                            std::cerr << gogAPI->getErrorMessage() << std::endl;
                            gogAPI->clearError();
                            continue;
                        }
                        std::cout << "Repairing file " << filepath << std::endl;
                        this->repairFile(url, filepath, XML, games[i].dlcs[j].gamename);
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

    if (config.bCover && !config.bUpdateCheck)
        coverXML = this->getResponse(config.sCoverList);

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        if (config.bSaveSerials && !games[i].serials.empty())
        {
            std::string filepath = games[i].getSerialsFilepath();
            this->saveSerials(games[i].serials, filepath);
        }

        if (config.bSaveChangelogs && !games[i].changelog.empty())
        {
            std::string filepath = games[i].getChangelogFilepath();
            this->saveChangelog(games[i].changelog, filepath);
        }

        // Download covers
        if (config.bCover && !config.bUpdateCheck)
        {
            if (!games[i].installers.empty())
            {
                // Take path from installer path because for some games the base directory for installer/extra path is not "gamename"
                boost::filesystem::path filepath = boost::filesystem::absolute(games[i].installers[0].getFilepath(), boost::filesystem::current_path());

                // Get base directory from filepath
                std::string directory = filepath.parent_path().string();

                this->downloadCovers(games[i].gamename, directory, coverXML);
            }
        }

        if (config.bInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                dlQueue.push(games[i].installers[j]);
            }
        }
        if (config.bPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                dlQueue.push(games[i].patches[j]);
            }
        }
        if (config.bExtras)
        {
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                dlQueue.push(games[i].extras[j]);
            }
        }
        if (config.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                dlQueue.push(games[i].languagepacks[j]);
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
                if (config.bSaveChangelogs && !games[i].dlcs[j].changelog.empty())
                {
                    std::string filepath = games[i].dlcs[j].getChangelogFilepath();
                    this->saveChangelog(games[i].dlcs[j].changelog, filepath);
                }

                if (config.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].installers[k]);
                    }
                }
                if (config.bPatches)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].patches[k]);
                    }
                }
                if (config.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].extras[k]);
                    }
                }
                if (config.bLanguagePacks)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].languagepacks.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].languagepacks[k]);
                    }
                }
            }
        }
    }

    if (!dlQueue.empty())
    {
        // Limit thread count to number of items in download queue
        unsigned int iThreads = std::min(config.iThreads, static_cast<unsigned int>(dlQueue.size()));

        // Create download threads
        std::vector<std::thread> vThreads;
        for (unsigned int i = 0; i < iThreads; ++i)
        {
            DownloadInfo dlInfo;
            dlInfo.setStatus(DLSTATUS_NOTSTARTED);
            vDownloadInfo.push_back(dlInfo);
            vThreads.push_back(std::thread(Downloader::processDownloadQueue, this->config, i));
        }

        this->printProgress();

        // Join threads
        for (unsigned int i = 0; i < vThreads.size(); ++i)
            vThreads[i].join();

        vThreads.clear();
        vDownloadInfo.clear();
    }

    // Create xml data for all files in the queue
    if (!createXMLQueue.empty())
    {
        std::cout << "Starting XML creation" << std::endl;
        gameFile gf;
        while (createXMLQueue.try_pop(gf))
        {
            std::string xml_directory = config.sXMLDirectory + "/" + gf.gamename;
            Util::createXML(gf.getFilepath(), config.iChunkSize, xml_directory);
        }
    }
}

// Download a file, resume if possible
CURLcode Downloader::downloadFile(const std::string& url, const std::string& filepath, const std::string& xml_data, const std::string& gamename)
{
    CURLcode res = CURLE_RECV_ERROR; // assume network error
    bool bResume = false;
    FILE *outfile;
    off_t offset=0;

    // Get directory from filepath
    boost::filesystem::path pathname = filepath;
    pathname = boost::filesystem::absolute(pathname, boost::filesystem::current_path());
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
            tinyxml2::XMLDocument remote_xml;
            remote_xml.Parse(xml_data.c_str());
            tinyxml2::XMLElement *fileElemRemote = remote_xml.FirstChildElement("file");
            if (fileElemRemote)
            {
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
            std::cerr << path << " is not directory" << std::endl;
            return res;
        }
    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cerr << "Failed to create directory: " << path << std::endl;
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
                // use ftello to support large files on 32 bit platforms
                offset = ftello(outfile);
                curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM_LARGE, offset);
                this->resume_position = offset;
            }
            else
            {
                std::cerr << "Failed to reopen " << filepath << std::endl;
                return res;
            }
        }
        else
        {   // File exists but is not the same version
            fclose(outfile);
            std::cerr << "Remote file is different, renaming local file" << std::endl;
            std::string date_old = "." + bptime::to_iso_string(bptime::second_clock::local_time()) + ".old";
            boost::filesystem::path new_name = filepath + date_old; // Rename old file by appending date and ".old" to filename
            boost::system::error_code ec;
            boost::filesystem::rename(pathname, new_name, ec); // Rename the file
            if (ec)
            {
                std::cerr << "Failed to rename " << filepath << " to " << new_name.string() << std::endl;
                std::cerr << "Skipping file" << std::endl;
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
                    std::cerr << "Failed to create " << filepath << std::endl;
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
            std::cerr << "Failed to create " << filepath << std::endl;
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
                    std::cerr << path << " is not directory" << std::endl;
                }
            }
            else
            {
                if (!boost::filesystem::create_directories(path))
                {
                    std::cerr << "Failed to create directory: " << path << std::endl;
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
                std::cerr << "Can't create " << local_xml_file.string() << std::endl;
            }
        }
    }

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
    res = this->beginDownload();

    fclose(outfile);

    // Download failed and was not a resume attempt so delete the file
    if (res != CURLE_OK && res != CURLE_PARTIAL_FILE && !bResume && res != CURLE_OPERATION_TIMEDOUT)
    {
        boost::filesystem::path path = filepath;
        if (boost::filesystem::exists(path))
            if (!boost::filesystem::remove(path))
                std::cerr << "Failed to delete " << path << std::endl;
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
    // Retry if we aborted the transfer due to low speed limit
    if ((res == CURLE_PARTIAL_FILE || res == CURLE_OPERATION_TIMEDOUT) && (this->retries < config.iRetries) )
    {
        this->retries++;

        std::cerr << std::endl << "Retry " << this->retries << "/" << config.iRetries;
        if (res == CURLE_PARTIAL_FILE)
            std::cerr << " (partial download)";
        else if (res == CURLE_OPERATION_TIMEDOUT)
            std::cerr << " (timeout)";
        std::cerr << std::endl;

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
    off_t offset=0, from_offset, to_offset, filesize;
    std::string filehash;
    int chunks;
    std::vector<off_t> chunk_from, chunk_to;
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

    tinyxml2::XMLDocument xml;
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
        xml.LoadFile(xml_file.c_str());
    }

    // Check if file node exists in XML data
    tinyxml2::XMLElement *fileElem = xml.FirstChildElement("file");
    if (!fileElem)
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
        filename = fileElem->Attribute("name");
        filehash = fileElem->Attribute("md5");
        std::stringstream(fileElem->Attribute("chunks")) >> chunks;
        std::stringstream(fileElem->Attribute("total_size")) >> filesize;

        //Iterate through all chunk nodes
        tinyxml2::XMLElement *chunkElem = fileElem->FirstChildElement("chunk");
        while (chunkElem)
        {
            std::stringstream(chunkElem->Attribute("from")) >> from_offset;
            std::stringstream(chunkElem->Attribute("to")) >> to_offset;
            chunk_from.push_back(from_offset);
            chunk_to.push_back(to_offset);
            chunk_hash.push_back(chunkElem->GetText());
            chunkElem = chunkElem->NextSiblingElement("chunk");
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

                if (config.bAutomaticXMLCreation && !bLocalXMLExists)
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
            // use ftello to support large files on 32 bit platforms
            offset = ftello(outfile);
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
                if (config.bAutomaticXMLCreation && bParsingFailed)
                {
                    std::cout << "Starting automatic XML creation" << std::endl;
                    Util::createXML(filepath, config.iChunkSize, xml_directory);
                }
                res = 1;
            }
        }
        return res;
    }

    // check if file sizes match
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
                if (bLocalXMLExists)
                {
                    std::cout << "Deleting old XML data" << std::endl;
                    boost::filesystem::remove(xml_file, ec); // Delete old XML data
                    if (ec)
                    {
                        std::cout << "Failed to delete " << xml_file << std::endl;
                    }
                }

                CURLcode result = this->downloadFile(url, filepath, xml_data, gamename);
                std::cout << std::endl;
                if (result == CURLE_OK)
                {
                    bLocalXMLExists = boost::filesystem::exists(xml_file); // Check to see if downloadFile saved XML data
                    if (!bLocalXMLExists)
                    {
                        std::cout << "Starting automatic XML creation" << std::endl;
                        Util::createXML(filepath, config.iChunkSize, xml_directory);
                    }
                    res = 1;
                }
                else
                {
                    res = 0;
                }
            }
        }
        return res;
    }

    // Check all chunks
    int iChunksRepaired = 0;
    for (int i=0; i<chunks; i++)
    {
        off_t chunk_begin = chunk_from.at(i);
        off_t chunk_end = chunk_to.at(i);
        off_t size=0, chunk_size = chunk_end - chunk_begin + 1;
        std::string range = std::to_string(chunk_begin) + "-" + std::to_string(chunk_end); // Download range string for curl

        std::cout << "\033[0K\rChunk " << i << " (" << chunk_size << " bytes): ";
        // use fseeko to support large files on 32 bit platforms
        fseeko(outfile, chunk_begin, SEEK_SET);
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
            // use fseeko to support large files on 32 bit platforms
            fseeko(outfile, chunk_begin, SEEK_SET);
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
    tinyxml2::XMLDocument xml;

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
    tinyxml2::XMLElement *rootNode = xml.RootElement();
    if (!rootNode)
    {
        std::cout << "Not valid XML" << std::endl;
        return res;
    }
    else
    {
        tinyxml2::XMLNode *gameNode = rootNode->FirstChild();
        while (gameNode)
        {
            tinyxml2::XMLElement *gameElem = gameNode->ToElement();
            std::string game_name = gameElem->Attribute("name");

            if (game_name == gamename)
            {
                boost::match_results<std::string::const_iterator> what;
                tinyxml2::XMLNode *coverNode = gameNode->FirstChild();
                while (coverNode)
                {
                    tinyxml2::XMLElement *coverElem = coverNode->ToElement();
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
                            std::cout << response_code << " (" << cover_url << ")" << std::endl;
                        else
                            std::cout << "failed to get error code: " << curl_easy_strerror(result) << " (" << cover_url << ")" << std::endl;
                    }

                    coverNode = coverNode->NextSibling();
                }
                break; // Found cover for game, no need to go through rest of the game nodes
            }
            gameNode = gameNode->NextSibling();
        }
    }

    return res;
}

CURLcode Downloader::beginDownload()
{
    this->TimeAndSize.clear();
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
                std::cout << response_code << " (" << url << ")" << std::endl;
            else
                std::cout << "failed to get error code: " << curl_easy_strerror(result) << " (" << url << ")" << std::endl;
        }
    }

    return response;
}

int Downloader::progressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    // unused so lets prevent warnings and be more pedantic
    (void) ulnow;
    (void) ultotal;

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
    bool starting = ((0 == dlnow) && (0 == dltotal));

    // (Shmerl): DEBUG: strange thing - when resuming a file which is already downloaded, dlnow is correctly 0.0
    // but dltotal is 389.0! This messes things up in the progress bar not showing the very last bar as full.
    // enable this debug line to test the problem:
    //
    //   printf("\r\033[0K dlnow: %0.2f, dltotal: %0.2f\r", dlnow, dltotal); fflush(stdout); return 0;
    //
    // For now making a quirky workaround and setting dltotal to 0.0 in that case.
    // It's probably better to find a real fix.
    if ((0 == dlnow) && (389 == dltotal)) dltotal = 0;

    // setting full dlwnow and dltotal
    curl_off_t offset = static_cast<curl_off_t>(downloader->getResumePosition());
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

        // 10 second average download speed
        // Don't use static value of 10 seconds because update interval depends on when and how often progress callback is called
        downloader->TimeAndSize.push_back(std::make_pair(time(NULL), static_cast<uintmax_t>(dlnow)));
        if (downloader->TimeAndSize.size() > 100) // 100 * 100ms = 10s
        {
            downloader->TimeAndSize.pop_front();
            time_t time_first = downloader->TimeAndSize.front().first;
            uintmax_t size_first = downloader->TimeAndSize.front().second;
            time_t time_last = downloader->TimeAndSize.back().first;
            uintmax_t size_last = downloader->TimeAndSize.back().second;
            rate = (size_last - size_first) / static_cast<double>((time_last - time_first));
        }

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
        double fraction = starting ? 0.0 : static_cast<double>(dlnow) / static_cast<double>(dltotal);

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
        sprintf(status_text, " %0.2f/%0.2fMB @ %0.2f%s ETA: %s\r", static_cast<double>(dlnow)/1024/1024, static_cast<double>(dltotal)/1024/1024, rate, rate_unit.c_str(), eta_ss.str().c_str());
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

uintmax_t Downloader::getResumePosition()
{
    return this->resume_position;
}

std::vector<gameFile> Downloader::getExtrasFromJSON(const Json::Value& json, const std::string& gamename, const Config& config)
{
    std::vector<gameFile> extras;

    // Create new API handle and set curl options for the API
    API* api = new API(config.sToken, config.sSecret);
    api->curlSetOpt(CURLOPT_VERBOSE, config.bVerbose);
    api->curlSetOpt(CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    api->curlSetOpt(CURLOPT_CONNECTTIMEOUT, config.iTimeout);
    if (!config.sCACertPath.empty())
        api->curlSetOpt(CURLOPT_CAINFO, config.sCACertPath.c_str());

    if (!api->init())
    {
        delete api;
        return extras;
    }

    for (unsigned int i = 0; i < json["extras"].size(); ++i)
    {
        std::string id, name, path, downloaderUrl;
        name = json["extras"][i]["name"].asString();
        downloaderUrl = json["extras"][i]["downloaderUrl"].asString();
        id.assign(downloaderUrl.begin()+downloaderUrl.find_last_of("/")+1, downloaderUrl.end());

        // Get path from download link
        std::string url = api->getExtraLink(gamename, id);
        if (api->getError())
        {
            api->clearError();
            continue;
        }
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

        // Get filename
        std::string filename;
        filename.assign(path.begin()+path.find_last_of("/")+1,path.end());

        // Use filename if name was not specified
        if (name.empty())
            name = filename;

        if (name.empty())
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (getExtrasFromJSON)" << std::endl;
                std::cerr << "Skipped file without a name (game: " << gamename << ", fileid: " << id << ")" << std::endl;
            #endif
            continue;
        }

        if (filename.empty())
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (getExtrasFromJSON)" << std::endl;
                std::cerr << "Skipped file without a filename (game: " << gamename << ", fileid: " << id << ", name: " << name << ")" << std::endl;
            #endif
            continue;
        }

        gameFile gf;
        gf.type = GFTYPE_EXTRA;
        gf.gamename = gamename;
        gf.updated = false;
        gf.id = id;
        gf.name = name;
        gf.path = path;

        extras.push_back(gf);
    }
    delete api;

    return extras;
}

std::string Downloader::getSerialsFromJSON(const Json::Value& json)
{
    std::ostringstream serials;

    if (!json.isMember("cdKey"))
        return std::string();

    std::string cdkey = json["cdKey"].asString();

    if (cdkey.empty())
        return std::string();

    if (cdkey.find("<span>") == std::string::npos)
    {
        serials << cdkey << std::endl;
    }
    else
    {
        htmlcxx::HTML::ParserDom parser;
        tree<htmlcxx::HTML::Node> dom = parser.parseTree(cdkey);
        tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
        tree<htmlcxx::HTML::Node>::iterator end = dom.end();
        for (; it != end; ++it)
        {
            std::string tag_text;
            if (it->tagName() == "span")
            {
                for (unsigned int j = 0; j < dom.number_of_children(it); ++j)
                {
                    tree<htmlcxx::HTML::Node>::iterator span_it = dom.child(it, j);
                    if (!span_it->isTag() && !span_it->isComment())
                        tag_text = span_it->text();
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

    return serials.str();
}

std::string Downloader::getChangelogFromJSON(const Json::Value& json)
{
    std::string changelog;
    std::string title = "Changelog";

    if (!json.isMember("changelog"))
        return std::string();

    changelog = json["changelog"].asString();

    if (changelog.empty())
        return std::string();

    if (json.isMember("title"))
        title = title + ": " + json["title"].asString();

    changelog = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n<title>" + title + "</title>\n</head>\n<body>" + changelog + "</body>\n</html>";

    return changelog;
}

// Linear search.  Good thing computers are fast and lists are small.
static int isPresent(std::vector<gameFile>& list, const boost::filesystem::path& path, Blacklist& blacklist)
{
    if(blacklist.isBlacklisted(path.native()))
	return false;
    for (unsigned int k = 0; k < list.size(); ++k)
	if (list[k].getFilepath() == path.native())
	    return true;
    return false;
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
        std::cerr << "Checking for orphaned files " << i+1 << " / " << games.size() << "\r" << std::flush;
        std::vector<boost::filesystem::path> filepath_vector;

        try
        {
            std::vector<boost::filesystem::path> paths;
            std::vector<unsigned int> platformIds;
            platformIds.push_back(0);
            for (unsigned int j = 0; j < GlobalConstants::PLATFORMS.size(); ++j)
            {
                platformIds.push_back(GlobalConstants::PLATFORMS[j].id);
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
                                if (config.ignorelist.isBlacklisted(filepath.substr(pathlen))) {
                                    if (config.bVerbose)
                                        std::cerr << "skipped ignorelisted file " << filepath << std::endl;
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
                    std::cerr << paths[j] << " does not exist" << std::endl;
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
                bool bFoundFile = isPresent(games[i].installers, filepath_vector[j], config.blacklist)
		               || isPresent(games[i].extras, filepath_vector[j], config.blacklist)
		               || isPresent(games[i].patches, filepath_vector[j], config.blacklist)
		               || isPresent(games[i].languagepacks, filepath_vector[j], config.blacklist);

                if (!bFoundFile)
                {   // Check dlcs
                    for (unsigned int k = 0; k < games[i].dlcs.size(); ++k)
                    {
                        bFoundFile = isPresent(games[i].dlcs[k].installers, filepath_vector[j], config.blacklist)
                            || isPresent(games[i].dlcs[k].extras, filepath_vector[j], config.blacklist)
                            || isPresent(games[i].dlcs[k].patches, filepath_vector[j], config.blacklist)
                            || isPresent(games[i].dlcs[k].languagepacks, filepath_vector[j], config.blacklist);
                        if(bFoundFile)
                            break;
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

    // Create a vector containing all game files
    std::vector<gameFile> vGameFiles;
    for (unsigned int i = 0; i < games.size(); ++i)
    {
        std::vector<gameFile> vec = games[i].getGameFileVector();
        vGameFiles.insert(std::end(vGameFiles), std::begin(vec), std::end(vec));
    }

    for (unsigned int i = 0; i < vGameFiles.size(); ++i)
    {
        unsigned int type = vGameFiles[i].type;
        if (!config.bDLC && (type & GFTYPE_DLC))
            continue;
        if (!config.bInstallers && (type & GFTYPE_INSTALLER))
            continue;
        if (!config.bExtras && (type & GFTYPE_EXTRA))
            continue;
        if (!config.bPatches && (type & GFTYPE_PATCH))
            continue;
        if (!config.bLanguagePacks && (type & GFTYPE_LANGPACK))
            continue;

        boost::filesystem::path filepath = vGameFiles[i].getFilepath();

        if (config.blacklist.isBlacklisted(filepath.native()))
            continue;

        std::string gamename = vGameFiles[i].gamename;
        std::string id = vGameFiles[i].id;

        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
        {
            std::string remoteHash;
            bool bHashOK = true; // assume hash OK
            uintmax_t filesize = boost::filesystem::file_size(filepath);

            // GOG only provides xml data for installers, patches and language packs
            if (type & (GFTYPE_INSTALLER | GFTYPE_PATCH | GFTYPE_LANGPACK))
                remoteHash = this->getRemoteFileHash(gamename, id);
            std::string localHash = this->getLocalFileHash(filepath.string(), gamename);

            if (!remoteHash.empty())
            {
                if (remoteHash != localHash)
                    bHashOK = false;
                else
                {
                    // Check for incomplete file by comparing the filesizes
                    // Remote hash was saved but download was incomplete and therefore getLocalFileHash returned the same as getRemoteFileHash
                    uintmax_t filesize_xml = 0;
                    boost::filesystem::path path = filepath;
                    boost::filesystem::path local_xml_file;
                    if (!gamename.empty())
                        local_xml_file = config.sXMLDirectory + "/" + gamename + "/" + path.filename().string() + ".xml";
                    else
                        local_xml_file = config.sXMLDirectory + "/" + path.filename().string() + ".xml";

                    if (boost::filesystem::exists(local_xml_file))
                    {
                        tinyxml2::XMLDocument local_xml;
                        local_xml.LoadFile(local_xml_file.string().c_str());
                        tinyxml2::XMLElement *fileElemLocal = local_xml.FirstChildElement("file");
                        if (fileElemLocal)
                        {
                            std::string filesize_xml_str = fileElemLocal->Attribute("total_size");
                            filesize_xml = std::stoull(filesize_xml_str);
                        }
                    }

                    if (filesize_xml > 0 && filesize_xml != filesize)
                    {
                        localHash = Util::getFileHash(path.string(), RHASH_MD5);
                        std::cout << "FS " << gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
                        continue;
                    }
                }
            }
            std::cout << (bHashOK ? "OK " : "MD5 ") << gamename << " " << filepath.filename().string() << " " << filesize << " " << localHash << std::endl;
        }
        else
        {
            std::cout << "ND " << gamename << " " << filepath.filename().string() << std::endl;
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

    if (config.bAutomaticXMLCreation && !boost::filesystem::exists(local_xml_file) && boost::filesystem::exists(path))
    {
        std::string xml_directory = config.sXMLDirectory + "/" + gamename;
        Util::createXML(filepath, config.iChunkSize, xml_directory);
    }

    localHash = Util::getLocalFileHash(config.sXMLDirectory, filepath, gamename);

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
        tinyxml2::XMLDocument remote_xml;
        remote_xml.Parse(xml_data.c_str());
        tinyxml2::XMLElement *fileElemRemote = remote_xml.FirstChildElement("file");
        if (fileElemRemote)
        {
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
    returns 5 if cache version doesn't match
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

        int iCacheVersion = 0;
        if (root.isMember("gamedetails-cache-version"))
            iCacheVersion = root["gamedetails-cache-version"].asInt();

        if (iCacheVersion != GlobalConstants::GAMEDETAILS_CACHE_VERSION)
        {
                res = 5;
        }
        else
        {
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
    }
    else
    {
        res = 2;
        std::cout << "Failed to parse cache" << std::endl;
        std::cout << jsonparser->getFormattedErrorMessages() << std::endl;
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

    json["gamedetails-cache-version"] = GlobalConstants::GAMEDETAILS_CACHE_VERSION;
    json["version-string"] = config.sVersionString;
    json["version-number"] = config.sVersionNumber;
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
        game.changelog = gameDetailsNode["changelog"].asString();

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
        conf.iInstallerPlatform = config.iInstallerPlatform;
        conf.vLanguagePriority = config.vLanguagePriority;
        conf.vPlatformPriority = config.vPlatformPriority;
        if (Util::getGameSpecificConfig(game.gamename, &conf) > 0)
            std::cerr << game.gamename << " - Language: " << conf.iInstallerLanguage << ", Platform: " << conf.iInstallerPlatform << ", DLC: " << (conf.bDLC ? "true" : "false") << std::endl;

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
                        fileDetails.gamename = fileDetailsNode["gamename"].asString();
                        fileDetails.type = fileDetailsNode["type"].asUInt();

                        if (nodeName != "extras" && !(fileDetails.platform & conf.iInstallerPlatform))
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
                    {
                        std::vector<gameDetails> dlcs = this->getGameDetailsFromJsonNode(fileDetailsNode, recursion_level + 1);
                        game.dlcs.insert(game.dlcs.end(), dlcs.begin(), dlcs.end());
                    }
                }
            }
        }
        if (!game.extras.empty() || !game.installers.empty() || !game.patches.empty() || !game.languagepacks.empty() || !game.dlcs.empty())
            {
                game.filterWithPriorities(conf);
                details.push_back(game);
            }
    }
    return details;
}

void Downloader::updateCache()
{
    // Make sure that all details get cached
    config.bExtras = true;
    config.bInstallers = true;
    config.bPatches = true;
    config.bLanguagePacks = true;
    config.bDLC = true;
    config.sGameRegex = ".*";
    config.iInstallerLanguage = Util::getOptionValue("all", GlobalConstants::LANGUAGES);
    config.iInstallerPlatform = Util::getOptionValue("all", GlobalConstants::PLATFORMS);
    config.vLanguagePriority.clear();
    config.vPlatformPriority.clear();
    config.sIgnoreDLCCountRegex = ".*"; // Ignore DLC count for all games because GOG doesn't report DLC count correctly
    gogWebsite->setConfig(config); // Make sure that website handle has updated config

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

// Save changelog to file
void Downloader::saveChangelog(const std::string& changelog, const std::string& filepath)
{
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
        std::cout << "Saving changelog: " << filepath << std::endl;
        ofs << changelog;
        ofs.close();
    }
    else
    {
        std::cout << "Failed to create file: " << filepath << std::endl;
    }

    return;
}

int Downloader::downloadFileWithId(const std::string& fileid_string, const std::string& output_filepath)
{
    int res = 1;
    size_t pos = fileid_string.find("/");
    if (pos == std::string::npos)
    {
        std::cout << "Invalid file id " << fileid_string << ": could not find separator \"/\"" << std::endl;
    }
    else if (!output_filepath.empty() && boost::filesystem::is_directory(output_filepath))
    {
        std::cout << "Failed to create the file " << output_filepath << ": Is a directory" << std::endl;
    }
    else
    {
        std::string gamename, fileid, url;
        gamename.assign(fileid_string.begin(), fileid_string.begin()+pos);
        fileid.assign(fileid_string.begin()+pos+1, fileid_string.end());

        if (fileid.find("installer") != std::string::npos)
            url = gogAPI->getInstallerLink(gamename, fileid);
        else if (fileid.find("patch") != std::string::npos)
            url = gogAPI->getPatchLink(gamename, fileid);
        else if (fileid.find("langpack") != std::string::npos)
            url = gogAPI->getLanguagePackLink(gamename, fileid);
        else
            url = gogAPI->getExtraLink(gamename, fileid);

        if (!gogAPI->getError())
        {
            std::string filename, filepath;
            filename.assign(url.begin()+url.find_last_of("/")+1, url.begin()+url.find_first_of("?"));
            if (output_filepath.empty())
                filepath = Util::makeFilepath(config.sDirectory, filename, gamename);
            else
                filepath = output_filepath;
            std::cout << "Downloading: " << filepath << std::endl;
            res = this->downloadFile(url, filepath, std::string(), gamename);
            std::cout << std::endl;
        }
        else
        {
            std::cout << gogAPI->getErrorMessage() << std::endl;
            gogAPI->clearError();
        }
    }

    return res;
}

void Downloader::showWishlist()
{
    std::vector<wishlistItem> wishlistItems = gogWebsite->getWishlistItems();
    for (unsigned int i = 0; i < wishlistItems.size(); ++i)
    {
        wishlistItem item = wishlistItems[i];
        std::string platforms_text = Util::getOptionNameString(item.platform, GlobalConstants::PLATFORMS);
        std::string tags_text;
        for (unsigned int j = 0; j < item.tags.size(); ++j)
        {
            tags_text += (tags_text.empty() ? "" : ", ")+item.tags[j];
        }
        if (!tags_text.empty())
            tags_text = "[" + tags_text + "]";

        std::string price_text = item.price;
        if (item.bIsDiscounted)
            price_text += " (-" + item.discount_percent + " | -" + item.discount + ")";

        std::cout << item.title;
        if (!tags_text.empty())
            std::cout << " " << tags_text;
        std::cout << std::endl;
        std::cout << "\t" << item.url << std::endl;
        if (item.platform != 0)
            std::cout << "\tPlatforms: " << platforms_text << std::endl;
        if (item.release_date_time != 0)
            std::cout << "\tRelease date: " << bptime::to_simple_string(bptime::from_time_t(item.release_date_time)) << std::endl;
        std::cout << "\tPrice: " << price_text << std::endl;
        if (item.bIsBonusStoreCreditIncluded)
            std::cout << "\tStore credit: " << item.store_credit << std::endl;
        std::cout << std::endl;
    }

    return;
}

void Downloader::processDownloadQueue(Config conf, const unsigned int& tid)
{
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    API* api = new API(conf.sToken, conf.sSecret);
    api->curlSetOpt(CURLOPT_SSL_VERIFYPEER, conf.bVerifyPeer);
    api->curlSetOpt(CURLOPT_CONNECTTIMEOUT, conf.iTimeout);
    if (!conf.sCACertPath.empty())
        api->curlSetOpt(CURLOPT_CAINFO, conf.sCACertPath.c_str());

    if (!api->init())
    {
        delete api;
        msgQueue.push(Message("API init failed", MSGTYPE_ERROR, msg_prefix));
        vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
        return;
    }

    CURL* dlhandle = curl_easy_init();
    curl_easy_setopt(dlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(dlhandle, CURLOPT_USERAGENT, conf.sVersionString.c_str());
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(dlhandle, CURLOPT_CONNECTTIMEOUT, conf.iTimeout);
    curl_easy_setopt(dlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(dlhandle, CURLOPT_SSL_VERIFYPEER, conf.bVerifyPeer);
    curl_easy_setopt(dlhandle, CURLOPT_VERBOSE, conf.bVerbose);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, conf.iDownloadRate);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_TIME, 30);
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_LIMIT, 200);

    if (!conf.sCACertPath.empty())
        curl_easy_setopt(dlhandle, CURLOPT_CAINFO, conf.sCACertPath.c_str());

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    gameFile gf;
    while (dlQueue.try_pop(gf))
    {
        CURLcode result = CURLE_RECV_ERROR; // assume network error
        int iRetryCount = 0;
        off_t iResumePosition = 0;

        vDownloadInfo[tid].setStatus(DLSTATUS_STARTING);

        // Get directory from filepath
        boost::filesystem::path filepath = gf.getFilepath();
        filepath = boost::filesystem::absolute(filepath, boost::filesystem::current_path());
        boost::filesystem::path directory = filepath.parent_path();

        // Skip blacklisted files
        if (conf.blacklist.isBlacklisted(filepath.string()))
        {
            msgQueue.push(Message("Blacklisted file: " + filepath.string(), MSGTYPE_INFO, msg_prefix));
            continue;
        }

        std::string filenameXML = filepath.filename().string() + ".xml";
        std::string xml_directory = conf.sXMLDirectory + "/" + gf.gamename;
        boost::filesystem::path local_xml_file = xml_directory + "/" + filenameXML;

        vDownloadInfo[tid].setFilename(filepath.filename().string());
        msgQueue.push(Message("Begin download: " + filepath.filename().string(), MSGTYPE_INFO, msg_prefix));

        // Check that directory exists and create subdirectories
        mtx_create_directories.lock(); // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory))
            {
                mtx_create_directories.unlock();
                msgQueue.push(Message(directory.string() + " is not directory, skipping file (" + filepath.filename().string() + ")", MSGTYPE_WARNING, msg_prefix));
                continue;
            }
            else
            {
                mtx_create_directories.unlock();
            }
        }
        else
        {
            if (!boost::filesystem::create_directories(directory))
            {
                mtx_create_directories.unlock();
                msgQueue.push(Message("Failed to create directory (" + directory.string() + "), skipping file (" + filepath.filename().string() + ")", MSGTYPE_ERROR, msg_prefix));
                continue;
            }
            else
            {
                mtx_create_directories.unlock();
            }
        }

        bool bSameVersion = true; // assume same version
        bool bLocalXMLExists = boost::filesystem::exists(local_xml_file); // This is additional check to see if remote xml should be saved to speed up future version checks

        std::string xml;
        if (gf.type & (GFTYPE_INSTALLER | GFTYPE_PATCH) && conf.bRemoteXML)
        {
            xml = api->getXML(gf.gamename, gf.id);
            if (api->getError())
            {
                msgQueue.push(Message(api->getErrorMessage(), MSGTYPE_ERROR, msg_prefix));
                api->clearError();
            }
            else
            {
                if (!xml.empty())
                {
                    std::string localHash = Util::getLocalFileHash(conf.sXMLDirectory, filepath.string(), gf.gamename);
                    // Do version check if local hash exists
                    if (!localHash.empty())
                    {
                        tinyxml2::XMLDocument remote_xml;
                        remote_xml.Parse(xml.c_str());
                        tinyxml2::XMLElement *fileElem = remote_xml.FirstChildElement("file");
                        if (fileElem)
                        {
                            std::string remoteHash = fileElem->Attribute("md5");
                            if (remoteHash != localHash)
                                bSameVersion = false;
                        }
                    }
                }
            }
        }

        bool bResume = false;
        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
        {
            if (bSameVersion)
            {
                bResume = true;
            }
            else
            {
                msgQueue.push(Message("Remote file is different, renaming local file", MSGTYPE_INFO, msg_prefix));
                std::string date_old = "." + bptime::to_iso_string(bptime::second_clock::local_time()) + ".old";
                boost::filesystem::path new_name = filepath.string() + date_old; // Rename old file by appending date and ".old" to filename
                boost::system::error_code ec;
                boost::filesystem::rename(filepath, new_name, ec); // Rename the file
                if (ec)
                {
                    msgQueue.push(Message("Failed to rename " + filepath.string() + " to " + new_name.string() + " - Skipping file", MSGTYPE_WARNING, msg_prefix));
                    continue;
                }
            }
        }

        // Save remote XML
        if (!xml.empty())
        {
            if ((bLocalXMLExists && !bSameVersion) || !bLocalXMLExists)
            {
                // Check that directory exists and create subdirectories
                boost::filesystem::path path = xml_directory;
                mtx_create_directories.lock(); // Use mutex to avoid race conditions
                if (boost::filesystem::exists(path))
                {
                    if (!boost::filesystem::is_directory(path))
                    {
                        msgQueue.push(Message(path.string() + " is not directory", MSGTYPE_WARNING, msg_prefix));
                    }
                }
                else
                {
                    if (!boost::filesystem::create_directories(path))
                    {
                        msgQueue.push(Message("Failed to create directory: " + path.string(), MSGTYPE_ERROR, msg_prefix));
                    }
                }
                mtx_create_directories.unlock();
                std::ofstream ofs(local_xml_file.string().c_str());
                if (ofs)
                {
                    ofs << xml;
                    ofs.close();
                }
                else
                {
                    msgQueue.push(Message("Can't create " + local_xml_file.string(), MSGTYPE_ERROR, msg_prefix));
                }
            }
        }

        // Get download url
        std::string url;
        if (gf.type & GFTYPE_INSTALLER)
            url = api->getInstallerLink(gf.gamename, gf.id);
        else if (gf.type & GFTYPE_PATCH)
            url = api->getPatchLink(gf.gamename, gf.id);
        else if (gf.type & GFTYPE_LANGPACK)
            url = api->getLanguagePackLink(gf.gamename, gf.id);
        else if (gf.type & GFTYPE_EXTRA)
            url = api->getExtraLink(gf.gamename, gf.id);
        else
            url = api->getExtraLink(gf.gamename, gf.id); // assume extra if type didn't match any of the others

        if (api->getError())
        {
            msgQueue.push(Message(api->getErrorMessage(), MSGTYPE_ERROR, msg_prefix));
            api->clearError();
            continue;
        }

        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
        do
        {
            if (iRetryCount != 0)
                msgQueue.push(Message("Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + filepath.filename().string(), MSGTYPE_INFO, msg_prefix));

            FILE* outfile;
            // File exists, resume
            if (bResume)
            {
                iResumePosition = boost::filesystem::file_size(filepath);
                if ((outfile=fopen(filepath.string().c_str(), "r+"))!=NULL)
                {
                    fseek(outfile, 0, SEEK_END);
                    curl_easy_setopt(dlhandle, CURLOPT_RESUME_FROM_LARGE, iResumePosition);
                    curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, outfile);
                }
                else
                {
                    msgQueue.push(Message("Failed to open " + filepath.string(), MSGTYPE_ERROR, msg_prefix));
                    break;
                }
            }
            else // File doesn't exist, create new file
            {
                if ((outfile=fopen(filepath.string().c_str(), "w"))!=NULL)
                {
                    curl_easy_setopt(dlhandle, CURLOPT_RESUME_FROM_LARGE, 0); // start downloading from the beginning of file
                    curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, outfile);
                }
                else
                {
                    msgQueue.push(Message("Failed to create " + filepath.string(), MSGTYPE_ERROR, msg_prefix));
                    break;
                }
            }

            xferinfo.offset = iResumePosition;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();
            result = curl_easy_perform(dlhandle);
            fclose(outfile);

            if (result == CURLE_PARTIAL_FILE || result == CURLE_OPERATION_TIMEDOUT)
            {
                iRetryCount++;
                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                    bResume = true;
            }

        } while ((result == CURLE_PARTIAL_FILE || result == CURLE_OPERATION_TIMEDOUT) && (iRetryCount <= conf.iRetries));

        long int response_code = 0;
        if (result == CURLE_HTTP_RETURNED_ERROR)
        {
            curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
        }
        if (result == CURLE_OK || result == CURLE_RANGE_ERROR || (result == CURLE_HTTP_RETURNED_ERROR && response_code == 416))
        {
            // Average download speed
            std::ostringstream dlrate_avg;
            std::string rate_unit;
            progressInfo progress_info = vDownloadInfo[tid].getProgressInfo();
            if (progress_info.rate_avg > 1048576) // 1 MB
            {
                progress_info.rate_avg /= 1048576;
                rate_unit = "MB/s";
            }
            else
            {
                progress_info.rate_avg /= 1024;
                rate_unit = "kB/s";
            }
            dlrate_avg << std::setprecision(2) << std::fixed << progress_info.rate_avg << rate_unit;

            msgQueue.push(Message("Download complete: " + filepath.filename().string() + " (@ " + dlrate_avg.str() + ")", MSGTYPE_SUCCESS, msg_prefix));
        }
        else
        {
            msgQueue.push(Message("Download complete (" + static_cast<std::string>(curl_easy_strerror(result)) + "): " + filepath.filename().string(), MSGTYPE_WARNING, msg_prefix));

            // Delete the file if download failed and was not a resume attempt or the result is zero length file
            if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
            {
                if ((result != CURLE_PARTIAL_FILE && !bResume && result != CURLE_OPERATION_TIMEDOUT) || boost::filesystem::file_size(filepath) == 0)
                {
                    if (!boost::filesystem::remove(filepath))
                        msgQueue.push(Message("Failed to delete " + filepath.filename().string(), MSGTYPE_ERROR, msg_prefix));
                }
            }
        }

        // Automatic xml creation
        if (conf.bAutomaticXMLCreation)
        {
            if (result == CURLE_OK)
            {
                if ((gf.type & GFTYPE_EXTRA) || (conf.bRemoteXML && !bLocalXMLExists && xml.empty()))
                    createXMLQueue.push(gf);
            }
        }
    }

    curl_easy_cleanup(dlhandle);
    delete api;

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    msgQueue.push(Message("Finished all tasks", MSGTYPE_INFO, msg_prefix));

    return;
}

int Downloader::progressCallbackForThread(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    // unused so lets prevent warnings and be more pedantic
    (void) ulnow;
    (void) ultotal;

    xferInfo* xferinfo = static_cast<xferInfo*>(clientp);

    // Update progress info every 100ms
    if (xferinfo->timer.getTimeBetweenUpdates()>=100 || dlnow == dltotal)
    {
        xferinfo->timer.reset();
        progressInfo info;
        info.dlnow = dlnow;
        info.dltotal = dltotal;

        // trying to get rate and setting to NaN if it fails
        if (CURLE_OK != curl_easy_getinfo(xferinfo->curlhandle, CURLINFO_SPEED_DOWNLOAD, &info.rate_avg))
            info.rate_avg = std::numeric_limits<double>::quiet_NaN();

        // setting full dlwnow and dltotal
        if (xferinfo->offset > 0)
        {
            info.dlnow   += xferinfo->offset;
            info.dltotal += xferinfo->offset;
        }

        // 10 second average download speed
        // Don't use static value of 10 seconds because update interval depends on when and how often progress callback is called
        xferinfo->TimeAndSize.push_back(std::make_pair(time(NULL), static_cast<uintmax_t>(info.dlnow)));
        if (xferinfo->TimeAndSize.size() > 100) // 100 * 100ms = 10s
        {
            xferinfo->TimeAndSize.pop_front();
            time_t time_first = xferinfo->TimeAndSize.front().first;
            uintmax_t size_first = xferinfo->TimeAndSize.front().second;
            time_t time_last = xferinfo->TimeAndSize.back().first;
            uintmax_t size_last = xferinfo->TimeAndSize.back().second;
            info.rate = (size_last - size_first) / static_cast<double>((time_last - time_first));
        }
        else
        {
            info.rate = info.rate_avg;
        }

        vDownloadInfo[xferinfo->tid].setProgressInfo(info);
        vDownloadInfo[xferinfo->tid].setStatus(DLSTATUS_RUNNING);
    }

    return 0;
}

void Downloader::printProgress()
{
    // Print progress information until all threads have finished their tasks
    ProgressBar bar(config.bUnicode, config.bColor);
    unsigned int dl_status = DLSTATUS_NOTSTARTED;
    while (dl_status != DLSTATUS_FINISHED)
    {
        dl_status = DLSTATUS_NOTSTARTED;

        // Print progress information once per 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "\033[J\r" << std::flush; // Clear screen from the current line down to the bottom of the screen

        // Print messages from message queue first
        Message msg;
        while (msgQueue.try_pop(msg))
        {
            std::cout << msg.getFormattedString(config.bColor, true) << std::endl;
            if (config.bReport)
            {
                this->report_ofs << msg.getTimestampString() << ": " << msg.getMessage() << std::endl;
            }
        }

        int iTermWidth = Util::getTerminalWidth();
        double total_rate = 0;

        // Create progress info text for all download threads
        std::vector<std::string> vProgressText;
        for (unsigned int i = 0; i < vDownloadInfo.size(); ++i)
        {
            std::string progress_text;
            int bar_length     = 26;
            int min_bar_length = 5;

            unsigned int status = vDownloadInfo[i].getStatus();
            dl_status |= status;

            if (status == DLSTATUS_FINISHED)
            {
                vProgressText.push_back("#" + std::to_string(i) + ": Finished");
                continue;
            }

            std::string filename = vDownloadInfo[i].getFilename();
            progressInfo progress_info = vDownloadInfo[i].getProgressInfo();
            total_rate += progress_info.rate;

            bool starting = ((0 == progress_info.dlnow) && (0 == progress_info.dltotal));
            double fraction = starting ? 0.0 : static_cast<double>(progress_info.dlnow) / static_cast<double>(progress_info.dltotal);

            char progress_percentage_text[200];
            sprintf(progress_percentage_text, "%3.0f%% ", fraction * 100);
            int progress_percentage_text_length = strlen(progress_percentage_text) + 1;

            bptime::time_duration eta(bptime::seconds((long)((progress_info.dltotal - progress_info.dlnow) / progress_info.rate)));
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

            std::string rate_unit;
            if (progress_info.rate > 1048576) // 1 MB
            {
                progress_info.rate /= 1048576;
                rate_unit = "MB/s";
            }
            else
            {
                progress_info.rate /= 1024;
                rate_unit = "kB/s";
            }

            char progress_status_text[200]; // We're probably never going to go as high as 200 characters but it's better to use too big number here than too small
            sprintf(progress_status_text, " %0.2f/%0.2fMB @ %0.2f%s ETA: %s", static_cast<double>(progress_info.dlnow)/1024/1024, static_cast<double>(progress_info.dltotal)/1024/1024, progress_info.rate, rate_unit.c_str(), eta_ss.str().c_str());
            int status_text_length = strlen(progress_status_text) + 1;

            if ((status_text_length + progress_percentage_text_length + bar_length) > iTermWidth)
                bar_length -= (status_text_length + progress_percentage_text_length + bar_length) - iTermWidth;

            // Don't draw progressbar if length is less than min_bar_length
            std::string progress_bar_text;
            if (bar_length >= min_bar_length)
                progress_bar_text = bar.createBarString(bar_length, fraction);

            progress_text = std::string(progress_percentage_text) + progress_bar_text + std::string(progress_status_text);
            std::string filename_text = "#" + std::to_string(i) + " " + filename;
            Util::shortenStringToTerminalWidth(filename_text);

            vProgressText.push_back(filename_text);
            vProgressText.push_back(progress_text);
        }

        // Total download speed and number of remaining tasks in download queue
        if (dl_status != DLSTATUS_FINISHED)
        {
            std::ostringstream ss;
            if (config.iThreads > 1)
            {
                std::string rate_unit;
                if (total_rate > 1048576) // 1 MB
                {
                    total_rate /= 1048576;
                    rate_unit = "MB/s";
                }
                else
                {
                    total_rate /= 1024;
                    rate_unit = "kB/s";
                }
                ss << "Total: " << std::setprecision(2) << std::fixed << total_rate << rate_unit << " | ";
            }
            ss << "Remaining: " << dlQueue.size();
            vProgressText.push_back(ss.str());
        }

        // Print progress info
        for (unsigned int i = 0; i < vProgressText.size(); ++i)
        {
            std::cout << vProgressText[i] << std::endl;
        }

        // Move cursor up by vProgressText.size() rows
        if (dl_status != DLSTATUS_FINISHED)
        {
            std::cout << "\033[" << vProgressText.size() << "A\r" << std::flush;
        }
    }
}

void Downloader::getGameDetailsThread(Config config, const unsigned int& tid)
{
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    API* api = new API(config.sToken, config.sSecret);
    api->curlSetOpt(CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    api->curlSetOpt(CURLOPT_CONNECTTIMEOUT, config.iTimeout);
    if (!config.sCACertPath.empty())
        api->curlSetOpt(CURLOPT_CAINFO, config.sCACertPath.c_str());

    if (!api->init())
    {
        delete api;
        msgQueue.push(Message("API init failed", MSGTYPE_ERROR, msg_prefix));
        vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
        return;
    }

    // Create new GOG website handle
    Website* website = new Website(config);
    if (!website->IsLoggedIn())
    {
        delete api;
        delete website;
        msgQueue.push(Message("Website not logged in", MSGTYPE_ERROR, msg_prefix));
        vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
        return;
    }

    // Set default game specific directory options to values from config
    gameSpecificDirectoryConfig dirConfDefault;
    dirConfDefault.sDirectory = config.sDirectory;
    dirConfDefault.bSubDirectories = config.bSubDirectories;
    dirConfDefault.sGameSubdir = config.sGameSubdir;
    dirConfDefault.sInstallersSubdir = config.sInstallersSubdir;
    dirConfDefault.sExtrasSubdir = config.sExtrasSubdir;
    dirConfDefault.sLanguagePackSubdir = config.sLanguagePackSubdir;
    dirConfDefault.sDLCSubdir = config.sDLCSubdir;
    dirConfDefault.sPatchesSubdir = config.sPatchesSubdir;

    gameItem game_item;
    while (gameItemQueue.try_pop(game_item))
    {
        gameDetails game;
        bool bHasDLC = !game_item.dlcnames.empty();

        gameSpecificConfig conf;
        conf.bDLC = config.bDLC;
        conf.bIgnoreDLCCount = false;
        conf.iInstallerLanguage = config.iInstallerLanguage;
        conf.iInstallerPlatform = config.iInstallerPlatform;
        conf.dirConf = dirConfDefault;
        conf.vLanguagePriority = config.vLanguagePriority;
        conf.vPlatformPriority = config.vPlatformPriority;
        if (!config.bUpdateCache) // Disable game specific config files for cache update
        {
            int iOptionsOverridden = Util::getGameSpecificConfig(game_item.name, &conf);
            if (iOptionsOverridden > 0)
            {
                std::ostringstream ss;
                ss << game_item.name << " - " << iOptionsOverridden << " options overridden with game specific options" << std::endl;
                if (config.bVerbose)
                {
                    if (conf.bIgnoreDLCCount)
                        ss << "\tIgnore DLC count" << std::endl;
                    if (conf.bDLC != config.bDLC)
                        ss << "\tDLC: " << (conf.bDLC ? "true" : "false") << std::endl;
                    if (conf.iInstallerLanguage != config.iInstallerLanguage)
                        ss << "\tLanguage: " << Util::getOptionNameString(conf.iInstallerLanguage, GlobalConstants::LANGUAGES) << std::endl;
                    if (conf.vLanguagePriority != config.vLanguagePriority)
                    {
                        ss << "\tLanguage priority:" << std::endl;
                        for (unsigned int j = 0; j < conf.vLanguagePriority.size(); ++j)
                        {
                            ss << "\t  " << j << ": " << Util::getOptionNameString(conf.vLanguagePriority[j], GlobalConstants::LANGUAGES) << std::endl;
                        }
                    }
                    if (conf.iInstallerPlatform != config.iInstallerPlatform)
                        ss << "\tPlatform: " << Util::getOptionNameString(conf.iInstallerPlatform, GlobalConstants::PLATFORMS) << std::endl;
                    if (conf.vPlatformPriority != config.vPlatformPriority)
                    {
                        ss << "\tPlatform priority:" << std::endl;
                        for (unsigned int j = 0; j < conf.vPlatformPriority.size(); ++j)
                        {
                            ss << "\t  " << j << ": " << Util::getOptionNameString(conf.vPlatformPriority[j], GlobalConstants::PLATFORMS) << std::endl;
                        }
                    }
                }
                msgQueue.push(Message(ss.str(), MSGTYPE_INFO, msg_prefix));
            }
        }

        game = api->getGameDetails(game_item.name, conf.iInstallerPlatform, conf.iInstallerLanguage, config.bDuplicateHandler);
        if (!api->getError())
        {
            game.filterWithPriorities(conf);
            Json::Value gameDetailsJSON;

            if (!game_item.gamedetailsjson.empty())
                gameDetailsJSON = game_item.gamedetailsjson;

            if (game.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
            {
                if (gameDetailsJSON.empty())
                    gameDetailsJSON = website->getGameDetailsJSON(game_item.id);
                game.extras = Downloader::getExtrasFromJSON(gameDetailsJSON, game_item.name, config);
            }
            if (config.bSaveSerials)
            {
                if (gameDetailsJSON.empty())
                    gameDetailsJSON = website->getGameDetailsJSON(game_item.id);
                game.serials = Downloader::getSerialsFromJSON(gameDetailsJSON);
            }
            if (config.bSaveChangelogs)
            {
                if (gameDetailsJSON.empty())
                    gameDetailsJSON = website->getGameDetailsJSON(game_item.id);
                game.changelog = Downloader::getChangelogFromJSON(gameDetailsJSON);
            }

            // Ignore DLC count and try to get DLCs from JSON
            if (game.dlcs.empty() && !bHasDLC && conf.bDLC && conf.bIgnoreDLCCount)
            {
                if (gameDetailsJSON.empty())
                    gameDetailsJSON = website->getGameDetailsJSON(game_item.id);

                game_item.dlcnames = Util::getDLCNamesFromJSON(gameDetailsJSON["dlcs"]);
                bHasDLC = !game_item.dlcnames.empty();
            }

            if (game.dlcs.empty() && bHasDLC && conf.bDLC)
            {
                for (unsigned int j = 0; j < game_item.dlcnames.size(); ++j)
                {
                    gameDetails dlc;
                    dlc = api->getGameDetails(game_item.dlcnames[j], conf.iInstallerPlatform, conf.iInstallerLanguage, config.bDuplicateHandler);
                    dlc.filterWithPriorities(conf);
                    if (dlc.extras.empty() && config.bExtras) // Try to get extras from account page if API didn't return any extras
                    {
                        if (gameDetailsJSON.empty())
                            gameDetailsJSON = website->getGameDetailsJSON(game_item.id);

                        // Make sure we get extras for the right DLC
                        for (unsigned int k = 0; k < gameDetailsJSON["dlcs"].size(); ++k)
                        {
                            std::vector<std::string> urls;
                            if (gameDetailsJSON["dlcs"][k].isMember("extras"))
                                Util::getDownloaderUrlsFromJSON(gameDetailsJSON["dlcs"][k]["extras"], urls);

                            if (!urls.empty())
                            {
                                if (urls[0].find("/" + game_item.dlcnames[j] + "/") != std::string::npos)
                                {
                                    dlc.extras = Downloader::getExtrasFromJSON(gameDetailsJSON["dlcs"][k], game_item.dlcnames[j], config);
                                }
                            }
                        }
                    }

                    if (config.bSaveSerials)
                    {
                        if (gameDetailsJSON.empty())
                            gameDetailsJSON = website->getGameDetailsJSON(game_item.id);

                        // Make sure we save serial for the right DLC
                        for (unsigned int k = 0; k < gameDetailsJSON["dlcs"].size(); ++k)
                        {
                            std::vector<std::string> urls;
                            if (gameDetailsJSON["dlcs"][k].isMember("cdKey") && gameDetailsJSON["dlcs"][k].isMember("downloads"))
                            {
                                // Assuming that only DLC with installers can have serial
                                Util::getDownloaderUrlsFromJSON(gameDetailsJSON["dlcs"][k]["downloads"], urls);
                            }

                            if (!urls.empty())
                            {
                                if (urls[0].find("/" + game_item.dlcnames[j] + "/") != std::string::npos)
                                {
                                    dlc.serials = Downloader::getSerialsFromJSON(gameDetailsJSON["dlcs"][k]);
                                }
                            }
                        }
                    }

                    if (config.bSaveChangelogs)
                    {
                        if (gameDetailsJSON.empty())
                            gameDetailsJSON = website->getGameDetailsJSON(game_item.id);

                        // Make sure we save changelog for the right DLC
                        for (unsigned int k = 0; k < gameDetailsJSON["dlcs"].size(); ++k)
                        {
                            std::vector<std::string> urls;
                            if (gameDetailsJSON["dlcs"][k].isMember("changelog") && gameDetailsJSON["dlcs"][k].isMember("downloads"))
                            {
                                // Assuming that only DLC with installers can have changelog
                                Util::getDownloaderUrlsFromJSON(gameDetailsJSON["dlcs"][k]["downloads"], urls);
                            }

                            if (!urls.empty())
                            {
                                if (urls[0].find("/" + game_item.dlcnames[j] + "/") != std::string::npos)
                                {
                                    dlc.changelog = Downloader::getChangelogFromJSON(gameDetailsJSON["dlcs"][k]);
                                }
                            }
                        }
                    }

                    // Add DLC type to all DLC files
                    for (unsigned int a = 0; a < dlc.installers.size(); ++a)
                        dlc.installers[a].type |= GFTYPE_DLC;
                    for (unsigned int a = 0; a < dlc.extras.size(); ++a)
                        dlc.extras[a].type |= GFTYPE_DLC;
                    for (unsigned int a = 0; a < dlc.patches.size(); ++a)
                        dlc.patches[a].type |= GFTYPE_DLC;
                    for (unsigned int a = 0; a < dlc.languagepacks.size(); ++a)
                        dlc.languagepacks[a].type |= GFTYPE_DLC;

                    game.dlcs.push_back(dlc);
                }
            }

            game.makeFilepaths(conf.dirConf);

            if (!config.bUpdateCheck)
                gameDetailsQueue.push(game);
            else
            { // Update check, only add games that have updated files
                for (unsigned int j = 0; j < game.installers.size(); ++j)
                {
                    if (game.installers[j].updated)
                    {
                        gameDetailsQueue.push(game);
                        break; // add the game only once
                    }
                }
            }
        }
        else
        {
            msgQueue.push(Message(api->getErrorMessage(), MSGTYPE_ERROR, msg_prefix));
            api->clearError();
            continue;
        }
    }
    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    delete api;
    delete website;
    return;
}
