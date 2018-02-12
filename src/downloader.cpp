/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "util.h"
#include "globals.h"
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

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

namespace bptime = boost::posix_time;

std::vector<DownloadInfo> vDownloadInfo;
ThreadSafeQueue<gameFile> dlQueue;
ThreadSafeQueue<Message> msgQueue;
ThreadSafeQueue<gameFile> createXMLQueue;
ThreadSafeQueue<gameItem> gameItemQueue;
ThreadSafeQueue<gameDetails> gameDetailsQueue;
ThreadSafeQueue<galaxyDepotItem> dlQueueGalaxy;
std::mutex mtx_create_directories; // Mutex for creating directories in Downloader::processDownloadQueue

static curl_off_t WriteChunkMemoryCallback(void *contents, curl_off_t size, curl_off_t nmemb, void *userp)
{
    curl_off_t realsize = size * nmemb;
    struct ChunkMemoryStruct *mem = (struct ChunkMemoryStruct *)userp;

    mem->memory = (char *) realloc(mem->memory, mem->size + realsize + 1);
    if(mem->memory == NULL)
    {
        std::cout << "Not enough memory (realloc returned NULL)" << std::endl;
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

Downloader::Downloader()
{
    if (Globals::globalConfig.bLoginHTTP)
    {
        if (boost::filesystem::exists(Globals::globalConfig.curlConf.sCookiePath))
            if (!boost::filesystem::remove(Globals::globalConfig.curlConf.sCookiePath))
                std::cerr << "Failed to delete " << Globals::globalConfig.curlConf.sCookiePath << std::endl;
        if (boost::filesystem::exists(Globals::galaxyConf.getFilepath()))
            if (!boost::filesystem::remove(Globals::galaxyConf.getFilepath()))
                std::cerr << "Failed to delete " << Globals::galaxyConf.getFilepath() << std::endl;
    }

    this->resume_position = 0;
    this->retries = 0;

    // Initialize curl and set curl options
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, Globals::globalConfig.curlConf.sUserAgent.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, Globals::globalConfig.curlConf.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, Globals::globalConfig.curlConf.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, Globals::globalConfig.curlConf.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, Globals::globalConfig.curlConf.iDownloadRate);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallback);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFODATA, this);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_TIME, Globals::globalConfig.curlConf.iLowSpeedTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_LIMIT, Globals::globalConfig.curlConf.iLowSpeedTimeoutRate);

    if (!Globals::globalConfig.curlConf.sCACertPath.empty())
        curl_easy_setopt(curlhandle, CURLOPT_CAINFO, Globals::globalConfig.curlConf.sCACertPath.c_str());

    // Create new GOG website handle
    gogWebsite = new Website();

    // Create new API handle and set curl options for the API
    gogAPI = new API(Globals::globalConfig.apiConf.sToken, Globals::globalConfig.apiConf.sSecret);
    gogAPI->curlSetOpt(CURLOPT_VERBOSE, Globals::globalConfig.curlConf.bVerbose);
    gogAPI->curlSetOpt(CURLOPT_SSL_VERIFYPEER, Globals::globalConfig.curlConf.bVerifyPeer);
    gogAPI->curlSetOpt(CURLOPT_CONNECTTIMEOUT, Globals::globalConfig.curlConf.iTimeout);
    if (!Globals::globalConfig.curlConf.sCACertPath.empty())
        gogAPI->curlSetOpt(CURLOPT_CAINFO, Globals::globalConfig.curlConf.sCACertPath.c_str());

    gogAPI->init();

    progressbar = new ProgressBar(Globals::globalConfig.bUnicode, Globals::globalConfig.bColor);

    if (boost::filesystem::exists(Globals::galaxyConf.getFilepath()))
    {
        std::ifstream ifs(Globals::galaxyConf.getFilepath(), std::ifstream::binary);
        Json::Value json;
		try {
			ifs >> json;
            if (!json.isMember("expires_at"))
            {
                std::time_t last_modified = boost::filesystem::last_write_time(Globals::galaxyConf.getFilepath());
                Json::Value::LargestInt expires_in = json["expires_in"].asLargestInt();
                json["expires_at"] = expires_in + last_modified;
            }

            Globals::galaxyConf.setJSON(json);
		} catch (const Json::Exception& exc) {
            std::cerr << "Failed to parse " << Globals::galaxyConf.getFilepath() << std::endl;
            std::cerr << exc.what() << std::endl;
		}

        if (ifs)
            ifs.close();
    }

    gogGalaxy = new galaxyAPI(Globals::globalConfig.curlConf);
}

Downloader::~Downloader()
{
    if (Globals::globalConfig.bReport)
        if (this->report_ofs)
            this->report_ofs.close();

    if (!gogGalaxy->isTokenExpired())
        this->saveGalaxyJSON();

    delete progressbar;
    delete gogGalaxy;
    delete gogAPI;
    delete gogWebsite;
    curl_easy_cleanup(curlhandle);
    // Make sure that cookie file is only readable/writable by owner
    if (!Globals::globalConfig.bRespectUmask)
    {
        Util::setFilePermissions(Globals::globalConfig.curlConf.sCookiePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
    }
}

/* Login check
    returns false if not logged in
    returns true if logged in
*/
bool Downloader::isLoggedIn()
{
    bool bIsLoggedIn = false;
    Globals::globalConfig.bLoginAPI = false;
    Globals::globalConfig.bLoginHTTP = false;

    bool bWebsiteIsLoggedIn = gogWebsite->IsLoggedIn();
    if (!bWebsiteIsLoggedIn)
        Globals::globalConfig.bLoginHTTP = true;

    bool bGalaxyIsLoggedIn = !gogGalaxy->isTokenExpired();
    if (!bGalaxyIsLoggedIn)
    {
        if (gogGalaxy->refreshLogin())
            bGalaxyIsLoggedIn = true;
        else
            Globals::globalConfig.bLoginHTTP = true;
    }

    bool bIsLoggedInAPI = gogAPI->isLoggedIn();
    if (!bIsLoggedInAPI)
        Globals::globalConfig.bLoginAPI = true;

    /* Check that website and Galaxy API are logged in.
        Allows users to use most of the functionality without having valid API login credentials.
        Globals::globalConfig.bLoginAPI can still be set to true at this point which means that if website is not logged in we still try to login to API.
    */
    if (bWebsiteIsLoggedIn && bGalaxyIsLoggedIn)
        bIsLoggedIn = true;

    return bIsLoggedIn;
}

/* Initialize the downloader
    returns 0 if failed
    returns 1 if successful
*/
int Downloader::init()
{
    if (!Globals::globalConfig.sGameHasDLCList.empty())
    {
        if (Globals::globalConfig.gamehasdlc.empty())
        {
            std::string game_has_dlc_list = this->getResponse(Globals::globalConfig.sGameHasDLCList);
            if (!game_has_dlc_list.empty())
                Globals::globalConfig.gamehasdlc.initialize(Util::tokenize(game_has_dlc_list, "\n"));
        }
    }

    if (!gogGalaxy->init())
    {
        if (gogGalaxy->refreshLogin())
        {
            this->saveGalaxyJSON();
        }
        else
            return 0;
    }

    if (!Globals::galaxyConf.getJSON().empty())
    {
        if (Globals::galaxyConf.isExpired())
        {
            // Access token has expired, refresh
            if (gogGalaxy->refreshLogin())
            {
                this->saveGalaxyJSON();
            }
        }
    }

    if (Globals::globalConfig.bReport && (Globals::globalConfig.bDownload || Globals::globalConfig.bRepair))
    {
        this->report_ofs.open(Globals::globalConfig.sReportFilePath);
        if (!this->report_ofs)
        {
            Globals::globalConfig.bReport = false;
            std::cerr << "Failed to create " << Globals::globalConfig.sReportFilePath << std::endl;
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
    std::string password;

    if (!Globals::globalConfig.sEmail.empty() && !Globals::globalConfig.sPassword.empty())
    {
        email = Globals::globalConfig.sEmail;
        password = Globals::globalConfig.sPassword;
    }
    else
    {
        if (!isatty(STDIN_FILENO)) {
            std::cerr << "Unable to read email and password" << std::endl;
            return 0;
        }
        std::cerr << "Email: ";
        std::getline(std::cin,email);

        std::cerr << "Password: ";
        struct termios termios_old, termios_new;
        tcgetattr(STDIN_FILENO, &termios_old); // Get current terminal attributes
        termios_new = termios_old;
        termios_new.c_lflag &= ~ECHO; // Set ECHO off
        tcsetattr(STDIN_FILENO, TCSANOW, &termios_new); // Set terminal attributes
        std::getline(std::cin, password);
        tcsetattr(STDIN_FILENO, TCSANOW, &termios_old); // Restore old terminal attributes
        std::cerr << std::endl;
    }

    if (email.empty() || password.empty())
    {
        std::cerr << "Email and/or password empty" << std::endl;
        return 0;
    }
    else
    {
        // Login to website
        if (Globals::globalConfig.bLoginHTTP)
        {
            // Delete old cookies
            if (boost::filesystem::exists(Globals::globalConfig.curlConf.sCookiePath))
                if (!boost::filesystem::remove(Globals::globalConfig.curlConf.sCookiePath))
                    std::cerr << "Failed to delete " << Globals::globalConfig.curlConf.sCookiePath << std::endl;

            int iWebsiteLoginResult = gogWebsite->Login(email, password);
            if (iWebsiteLoginResult < 1)
            {
                std::cerr << "HTTP: Login failed" << std::endl;
                return 0;
            }
            else
            {
                std::cerr << "HTTP: Login successful" << std::endl;
            }

            if (iWebsiteLoginResult < 2)
            {
                std::cerr << "Galaxy: Login failed" << std::endl;
                return 0;
            }
            else
            {
                std::cerr << "Galaxy: Login successful" << std::endl;

                if (!Globals::galaxyConf.getJSON().empty())
                {
                    this->saveGalaxyJSON();
                }

                if (!Globals::globalConfig.bLoginAPI)
                    return 1;
            }
        }
        // Login to API
        if (Globals::globalConfig.bLoginAPI)
        {
            if (!gogAPI->login(email, password))
            {
                std::cerr << "API: Login failed (some features may not work)" << std::endl;
                return 0;
            }
            else
            {
                std::cerr << "API: Login successful" << std::endl;
                Globals::globalConfig.apiConf.sToken = gogAPI->getToken();
                Globals::globalConfig.apiConf.sSecret = gogAPI->getSecret();
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
        Globals::globalConfig.sGameRegex = ".*"; // Always check all games
        if (Globals::globalConfig.bList || Globals::globalConfig.bListDetails || Globals::globalConfig.bDownload)
        {
            if (Globals::globalConfig.bList)
                Globals::globalConfig.bListDetails = true; // Always list details
            this->getGameList();
            if (Globals::globalConfig.bDownload)
                this->download();
            else
                this->listGames();
        }
    }
}

void Downloader::getGameList()
{
    if (Globals::globalConfig.sGameRegex == "free")
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
    DirectoryConfig dirConfDefault = Globals::globalConfig.dirConf;

    if (Globals::globalConfig.bUseCache && !Globals::globalConfig.bUpdateCache)
    {
        // GameRegex filter alias for all games
        if (Globals::globalConfig.sGameRegex == "all")
            Globals::globalConfig.sGameRegex = ".*";
        else if (Globals::globalConfig.sGameRegex == "free")
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
        unsigned int threads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(gameItemQueue.size()));
        std::vector<std::thread> vThreads;
        for (unsigned int i = 0; i < threads; ++i)
        {
            DownloadInfo dlInfo;
            dlInfo.setStatus(DLSTATUS_NOTSTARTED);
            vDownloadInfo.push_back(dlInfo);
            vThreads.push_back(std::thread(Downloader::getGameDetailsThread, Globals::globalConfig, i));
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
                std::cerr << msg.getFormattedString(Globals::globalConfig.bColor, true) << std::endl;
                if (Globals::globalConfig.bReport)
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
    if (Globals::globalConfig.bListDetails) // Detailed list
    {
        if (this->games.empty()) {
            int res = this->getGameDetails();
            if (res > 0)
                return res;
        }

        for (unsigned int i = 0; i < games.size(); ++i)
        {
            std::cout   << "gamename: " << games[i].gamename << std::endl
                        << "product id: " << games[i].product_id << std::endl
                        << "title: " << games[i].title << std::endl
                        << "icon: " << games[i].icon << std::endl;
            if (!games[i].serials.empty())
                std::cout << "serials:" << std::endl << games[i].serials << std::endl;

            // List installers
            if (Globals::globalConfig.dlConf.bInstallers)
            {
                std::cout << "installers: " << std::endl;
                for (unsigned int j = 0; j < games[i].installers.size(); ++j)
                {
                    if (!Globals::globalConfig.bUpdateCheck || games[i].installers[j].updated) // Always list updated files
                    {
                        std::string filepath = games[i].installers[j].getFilepath();
                        if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
                        {
                            if (Globals::globalConfig.bVerbose)
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
            if (Globals::globalConfig.dlConf.bExtras && !Globals::globalConfig.bUpdateCheck && !games[i].extras.empty())
            {
                std::cout << "extras: " << std::endl;
                for (unsigned int j = 0; j < games[i].extras.size(); ++j)
                {
                    std::string filepath = games[i].extras[j].getFilepath();
                    if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
                    {
                        if (Globals::globalConfig.bVerbose)
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
            if (Globals::globalConfig.dlConf.bPatches && !Globals::globalConfig.bUpdateCheck && !games[i].patches.empty())
            {
                std::cout << "patches: " << std::endl;
                for (unsigned int j = 0; j < games[i].patches.size(); ++j)
                {
                    std::string filepath = games[i].patches[j].getFilepath();
                    if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
                    {
                        if (Globals::globalConfig.bVerbose)
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
            if (Globals::globalConfig.dlConf.bLanguagePacks && !Globals::globalConfig.bUpdateCheck && !games[i].languagepacks.empty())
            {
                std::cout << "language packs: " << std::endl;
                for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
                {
                    std::string filepath = games[i].languagepacks[j].getFilepath();
                    if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
                    {
                        if (Globals::globalConfig.bVerbose)
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
            if (Globals::globalConfig.dlConf.bDLC && !games[i].dlcs.empty())
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
                        if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
                        {
                            if (Globals::globalConfig.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tproduct id: " << games[i].dlcs[j].product_id << std::endl
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
                        if (Globals::globalConfig.blacklist.isBlacklisted(filepath)) {
                            if (Globals::globalConfig.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tproduct id: " << games[i].dlcs[j].product_id << std::endl
                                    << "\tid: " << games[i].dlcs[j].patches[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].patches[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].patches[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].patches[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].extras[k].getFilepath();
                        if (Globals::globalConfig.blacklist.isBlacklisted(filepath)) {
                            if (Globals::globalConfig.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tproduct id: " << games[i].dlcs[j].product_id << std::endl
                                    << "\tid: " << games[i].dlcs[j].extras[k].id << std::endl
                                    << "\tname: " << games[i].dlcs[j].extras[k].name << std::endl
                                    << "\tpath: " << games[i].dlcs[j].extras[k].path << std::endl
                                    << "\tsize: " << games[i].dlcs[j].extras[k].size << std::endl
                                    << std::endl;
                    }
                    for (unsigned int k = 0; k < games[i].dlcs[j].languagepacks.size(); ++k)
                    {
                        std::string filepath = games[i].dlcs[j].languagepacks[k].getFilepath();
                        if (Globals::globalConfig.blacklist.isBlacklisted(filepath)) {
                            if (Globals::globalConfig.bVerbose)
                                std::cerr << "skipped blacklisted file " << filepath << std::endl;
                            continue;
                        }

                        std::cout   << "\tgamename: " << games[i].dlcs[j].gamename << std::endl
                                    << "\tproduct id: " << games[i].dlcs[j].product_id << std::endl
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
            std::string gamename = gameItems[i].name;
            if (gameItems[i].updates > 0)
            {
                gamename += " [" + std::to_string(gameItems[i].updates) + "]";
                if (Globals::globalConfig.bColor)
                    gamename = "\033[32m" + gamename + "\033[0m";
            }
            std::cout << gamename << std::endl;
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

    // Create a vector containing all game files
    std::vector<gameFile> vGameFiles;
    for (unsigned int i = 0; i < games.size(); ++i)
    {
        std::vector<gameFile> vec = games[i].getGameFileVector();
        vGameFiles.insert(std::end(vGameFiles), std::begin(vec), std::end(vec));
    }

    for (unsigned int i = 0; i < vGameFiles.size(); ++i)
    {
        gameSpecificConfig conf;
        conf.dlConf = Globals::globalConfig.dlConf;
        conf.dirConf = Globals::globalConfig.dirConf;

        unsigned int type = vGameFiles[i].type;
        if (!conf.dlConf.bDLC && (type & GFTYPE_DLC))
            continue;
        if (!conf.dlConf.bInstallers && (type & GFTYPE_INSTALLER))
            continue;
        if (!conf.dlConf.bExtras && (type & GFTYPE_EXTRA))
            continue;
        if (!conf.dlConf.bPatches && (type & GFTYPE_PATCH))
            continue;
        if (!conf.dlConf.bLanguagePacks && (type & GFTYPE_LANGPACK))
            continue;

        std::string filepath = vGameFiles[i].getFilepath();
        if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
        {
            if (Globals::globalConfig.bVerbose)
                std::cerr << "skipped blacklisted file " << filepath << std::endl;
            continue;
        }

        // Refresh Galaxy login if token is expired
        if (gogGalaxy->isTokenExpired())
        {
            if (!gogGalaxy->refreshLogin())
            {
                std::cerr << "Galaxy API failed to refresh login" << std::endl;
                break;
            }
        }

        Json::Value downlinkJson;
        std::string response = gogGalaxy->getResponse(vGameFiles[i].galaxy_downlink_json_url);

        if (response.empty())
        {
            std::cerr << "Found nothing in " << vGameFiles[i].galaxy_downlink_json_url << ", skipping file" << std::endl;
            continue;
        }
        try {
            std::istringstream iss(response);
            iss >> downlinkJson;
        } catch (const Json::Exception& exc) {
            std::cerr << "Could not parse JSON response, skipping file" << std::endl;
            continue;
        }

        if (!downlinkJson.isMember("downlink"))
        {
            std::cerr << "Invalid JSON response, skipping file" << std::endl;
            continue;
        }

        std::string xml_url;
        if (downlinkJson.isMember("checksum"))
            if (!downlinkJson["checksum"].empty())
                xml_url = downlinkJson["checksum"].asString();

        // Get XML data
        std::string XML = "";
        if (conf.dlConf.bRemoteXML && !xml_url.empty())
            XML = gogGalaxy->getResponse(xml_url);

        // Repair
        bool bUseLocalXML = !conf.dlConf.bRemoteXML;

        // Use local XML data for extras
        if (XML.empty() && (type & GFTYPE_EXTRA))
            bUseLocalXML = true;

        if (!XML.empty() || bUseLocalXML)
        {
            std::string url = downlinkJson["downlink"].asString();

            std::cout << "Repairing file " << filepath << std::endl;
            this->repairFile(url, filepath, XML, vGameFiles[i].gamename);
            std::cout << std::endl;
        }
    }
}

void Downloader::download()
{
    if (this->games.empty())
        this->getGameDetails();

    if (Globals::globalConfig.dlConf.bCover && !Globals::globalConfig.bUpdateCheck)
        coverXML = this->getResponse(Globals::globalConfig.sCoverList);

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        gameSpecificConfig conf;
        conf.dlConf = Globals::globalConfig.dlConf;
        conf.dirConf = Globals::globalConfig.dirConf;

        if (conf.dlConf.bSaveSerials && !games[i].serials.empty())
        {
            std::string filepath = games[i].getSerialsFilepath();
            this->saveSerials(games[i].serials, filepath);
        }

        if (conf.dlConf.bSaveChangelogs && !games[i].changelog.empty())
        {
            std::string filepath = games[i].getChangelogFilepath();
            this->saveChangelog(games[i].changelog, filepath);
        }

        // Download covers
        if (conf.dlConf.bCover && !Globals::globalConfig.bUpdateCheck)
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

        if (conf.dlConf.bInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                dlQueue.push(games[i].installers[j]);
            }
        }
        if (conf.dlConf.bPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                dlQueue.push(games[i].patches[j]);
            }
        }
        if (conf.dlConf.bExtras)
        {
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                dlQueue.push(games[i].extras[j]);
            }
        }
        if (conf.dlConf.bLanguagePacks)
        {
            for (unsigned int j = 0; j < games[i].languagepacks.size(); ++j)
            {
                dlQueue.push(games[i].languagepacks[j]);
            }
        }
        if (conf.dlConf.bDLC && !games[i].dlcs.empty())
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (conf.dlConf.bSaveSerials && !games[i].dlcs[j].serials.empty())
                {
                    std::string filepath = games[i].dlcs[j].getSerialsFilepath();
                    this->saveSerials(games[i].dlcs[j].serials, filepath);
                }
                if (conf.dlConf.bSaveChangelogs && !games[i].dlcs[j].changelog.empty())
                {
                    std::string filepath = games[i].dlcs[j].getChangelogFilepath();
                    this->saveChangelog(games[i].dlcs[j].changelog, filepath);
                }

                if (conf.dlConf.bInstallers)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].installers.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].installers[k]);
                    }
                }
                if (conf.dlConf.bPatches)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].patches.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].patches[k]);
                    }
                }
                if (conf.dlConf.bExtras)
                {
                    for (unsigned int k = 0; k < games[i].dlcs[j].extras.size(); ++k)
                    {
                        dlQueue.push(games[i].dlcs[j].extras[k]);
                    }
                }
                if (conf.dlConf.bLanguagePacks)
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
        unsigned int iThreads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(dlQueue.size()));

        // Create download threads
        std::vector<std::thread> vThreads;
        for (unsigned int i = 0; i < iThreads; ++i)
        {
            DownloadInfo dlInfo;
            dlInfo.setStatus(DLSTATUS_NOTSTARTED);
            vDownloadInfo.push_back(dlInfo);
            vThreads.push_back(std::thread(Downloader::processDownloadQueue, Globals::globalConfig, i));
        }

        this->printProgress(dlQueue);

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
            std::string xml_directory = Globals::globalConfig.sXMLDirectory + "/" + gf.gamename;
            Util::createXML(gf.getFilepath(), Globals::globalConfig.iChunkSize, xml_directory);
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
        xml_directory = Globals::globalConfig.sXMLDirectory + "/" + gamename;
    else
        xml_directory = Globals::globalConfig.sXMLDirectory;

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
        if ((bLocalXMLExists && (!bSameVersion || Globals::globalConfig.bRepair)) || !bLocalXMLExists)
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
    curl_easy_setopt(curlhandle, CURLOPT_FILETIME, 1L);
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

    if (Globals::globalConfig.bReport)
    {
        std::string status = static_cast<std::string>(curl_easy_strerror(res));
        if (bResume && res == CURLE_RANGE_ERROR) // CURLE_RANGE_ERROR on resume attempts is not an error that user needs to know about
            status = "No error";
        std::string report_line = "Downloaded [" + status + "] " + filepath;
        this->report_ofs << report_line << std::endl;
    }

    // Retry partially downloaded file
    // Retry if we aborted the transfer due to low speed limit
    if ((res == CURLE_PARTIAL_FILE || res == CURLE_OPERATION_TIMEDOUT) && (this->retries < Globals::globalConfig.iRetries) )
    {
        this->retries++;

        std::cerr << std::endl << "Retry " << this->retries << "/" << Globals::globalConfig.iRetries;
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
        // Set timestamp for downloaded file to same value as file on server
        long filetime = -1;
        CURLcode result = curl_easy_getinfo(curlhandle, CURLINFO_FILETIME, &filetime);
        if (result == CURLE_OK && filetime >= 0)
        {
            std::time_t timestamp = (std::time_t)filetime;
            boost::filesystem::last_write_time(filepath, timestamp);
        }
    }
    curl_easy_setopt(curlhandle, CURLOPT_FILETIME, 0L);

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
        xml_directory = Globals::globalConfig.sXMLDirectory + "/" + gamename;
    else
        xml_directory = Globals::globalConfig.sXMLDirectory;
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
        if (Globals::globalConfig.bDownload)
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
        if (Globals::globalConfig.bDownload)
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

                if (Globals::globalConfig.dlConf.bAutomaticXMLCreation && !bLocalXMLExists)
                {
                    std::cout << "Starting automatic XML creation" << std::endl;
                    Util::createXML(filepath, Globals::globalConfig.iChunkSize, xml_directory);
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
        if (Globals::globalConfig.bDownload)
        {
            std::cout << "Downloading: " << filepath << std::endl;
            CURLcode result = this->downloadFile(url, filepath, xml_data, gamename);
            std::cout << std::endl;
            if (result == CURLE_OK)
            {
                if (Globals::globalConfig.dlConf.bAutomaticXMLCreation && bParsingFailed)
                {
                    std::cout << "Starting automatic XML creation" << std::endl;
                    Util::createXML(filepath, Globals::globalConfig.iChunkSize, xml_directory);
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
        if (Globals::globalConfig.bDownload)
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
                        Util::createXML(filepath, Globals::globalConfig.iChunkSize, xml_directory);
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
    int iChunkRetryCount = 0;
    int iChunkRetryLimit = 3;
    bool bChunkRetryLimitReached = false;
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
            if (bChunkRetryLimitReached)
            {
                std::cout << "Failed - chunk retry limit reached\r" << std::flush;
                free(chunk);
                res = 0;
                break;
            }

            if (iChunkRetryCount < 1)
                std::cout << "Failed - downloading chunk" << std::endl;
            else
                std::cout << "Failed - retrying chunk download" << std::endl;
            // use fseeko to support large files on 32 bit platforms
            fseeko(outfile, chunk_begin, SEEK_SET);
            curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
            curl_easy_setopt(curlhandle, CURLOPT_RANGE, range.c_str()); //download range
            curl_easy_setopt(curlhandle, CURLOPT_FILETIME, 1L);
            this->beginDownload(); //begin chunk download
            std::cout << std::endl;
            if (Globals::globalConfig.bReport)
                iChunksRepaired++;
            i--; //verify downloaded chunk

            iChunkRetryCount++;
            if (iChunkRetryCount >= iChunkRetryLimit)
            {
                bChunkRetryLimitReached = true;
            }
        }
        else
        {
            std::cout << "OK\r" << std::flush;
            iChunkRetryCount = 0; // reset retry count
            bChunkRetryLimitReached = false;
        }
        free(chunk);
        res = 1;
    }
    std::cout << std::endl;
    fclose(outfile);

    if (Globals::globalConfig.bReport)
    {
        std::string report_line;
        if (bChunkRetryLimitReached)
            report_line = "Repair failed: " + filepath;
        else
            report_line = "Repaired [" + std::to_string(iChunksRepaired) + "/" + std::to_string(chunks) + "] " + filepath;
        this->report_ofs << report_line << std::endl;
    }

    if (bChunkRetryLimitReached)
        return res;

    // Set timestamp for downloaded file to same value as file on server
    long filetime = -1;
    CURLcode result = curl_easy_getinfo(curlhandle, CURLINFO_FILETIME, &filetime);
    if (result == CURLE_OK && filetime >= 0)
    {
        std::time_t timestamp = (std::time_t)filetime;
        boost::filesystem::last_write_time(filepath, timestamp);
    }
    curl_easy_setopt(curlhandle, CURLOPT_FILETIME, 0L);

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
        if (Globals::globalConfig.iWait > 0)
            usleep(Globals::globalConfig.iWait); // Delay the request by specified time
        result = curl_easy_perform(curlhandle);
        response = memory.str();
        memory.str(std::string());
    }
    while ((result != CURLE_OK) && response.empty() && (this->retries++ < Globals::globalConfig.iRetries));
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

        std::cout << Util::formattedString("\033[0K\r%3.0f%% ", fraction * 100);

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
        std::string status_text = Util::formattedString(" %0.2f/%0.2fMB @ %0.2f%s ETA: %s\r", static_cast<double>(dlnow)/1024/1024, static_cast<double>(dltotal)/1024/1024, rate, rate_unit.c_str(), eta_ss.str().c_str());
        int status_text_length = status_text.length() + 6;

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
    API* api = new API(config.apiConf.sToken, config.apiConf.sSecret);
    api->curlSetOpt(CURLOPT_VERBOSE, config.curlConf.bVerbose);
    api->curlSetOpt(CURLOPT_SSL_VERIFYPEER, config.curlConf.bVerifyPeer);
    api->curlSetOpt(CURLOPT_CONNECTTIMEOUT, config.curlConf.iTimeout);
    if (!config.curlConf.sCACertPath.empty())
        api->curlSetOpt(CURLOPT_CAINFO, config.curlConf.sCACertPath.c_str());

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
        boost::regex expression("<br\\h*/?>");
        std::string text = boost::regex_replace(cdkey, expression, "\n");
        serials << text << std::endl;
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
    Config config = Globals::globalConfig;
    config.dlConf.bInstallers = true;
    config.dlConf.bExtras = true;
    config.dlConf.bPatches = true;
    config.dlConf.bLanguagePacks = true;

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
                std::string directory = config.dirConf.sDirectory + "/" + config.dirConf.sGameSubdir + "/";
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
                std::size_t pathlen = config.dirConf.sDirectory.length();
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
        if (!Globals::globalConfig.dlConf.bDLC && (type & GFTYPE_DLC))
            continue;
        if (!Globals::globalConfig.dlConf.bInstallers && (type & GFTYPE_INSTALLER))
            continue;
        if (!Globals::globalConfig.dlConf.bExtras && (type & GFTYPE_EXTRA))
            continue;
        if (!Globals::globalConfig.dlConf.bPatches && (type & GFTYPE_PATCH))
            continue;
        if (!Globals::globalConfig.dlConf.bLanguagePacks && (type & GFTYPE_LANGPACK))
            continue;

        boost::filesystem::path filepath = vGameFiles[i].getFilepath();

        if (Globals::globalConfig.blacklist.isBlacklisted(filepath.native()))
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
                        local_xml_file = Globals::globalConfig.sXMLDirectory + "/" + gamename + "/" + path.filename().string() + ".xml";
                    else
                        local_xml_file = Globals::globalConfig.sXMLDirectory + "/" + path.filename().string() + ".xml";

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
        local_xml_file = Globals::globalConfig.sXMLDirectory + "/" + gamename + "/" + path.filename().string() + ".xml";
    else
        local_xml_file = Globals::globalConfig.sXMLDirectory + "/" + path.filename().string() + ".xml";

    if (Globals::globalConfig.dlConf.bAutomaticXMLCreation && !boost::filesystem::exists(local_xml_file) && boost::filesystem::exists(path))
    {
        std::string xml_directory = Globals::globalConfig.sXMLDirectory + "/" + gamename;
        Util::createXML(filepath, Globals::globalConfig.iChunkSize, xml_directory);
    }

    localHash = Util::getLocalFileHash(Globals::globalConfig.sXMLDirectory, filepath, gamename);

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
    std::string cachepath = Globals::globalConfig.sCacheDirectory + "/gamedetails.json";

    // Make sure file exists
    boost::filesystem::path path = cachepath;
    if (!boost::filesystem::exists(path)) {
        return res = 1;
    }

    bptime::ptime now = bptime::second_clock::local_time();
    bptime::ptime cachedate;

    std::ifstream json(cachepath, std::ifstream::binary);
    Json::Value root;
    try {
        json >> root;
    } catch (const Json::Exception& exc) {
        std::cout << "Failed to parse cache" << std::endl;
        std::cout << exc.what() << std::endl;
        return 2;
    }

    if (root.isMember("date"))
    {
        cachedate = bptime::from_iso_string(root["date"].asString());
        if ((now - cachedate) > bptime::minutes(Globals::globalConfig.iCacheValid))
        {
            // cache is too old
            return 3;
        }
    }

    int iCacheVersion = 0;
    if (root.isMember("gamedetails-cache-version"))
        iCacheVersion = root["gamedetails-cache-version"].asInt();

    if (iCacheVersion != GlobalConstants::GAMEDETAILS_CACHE_VERSION)
    {
        return 5;
    }

    if (root.isMember("games"))
    {
        this->games = getGameDetailsFromJsonNode(root["games"]);
        return 0;
    }

    return 4;
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

    std::string cachepath = Globals::globalConfig.sCacheDirectory + "/gamedetails.json";

    Json::Value json;

    json["gamedetails-cache-version"] = GlobalConstants::GAMEDETAILS_CACHE_VERSION;
    json["version-string"] = Globals::globalConfig.sVersionString;
    json["version-number"] = Globals::globalConfig.sVersionNumber;
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
        ofs << json << std::endl;
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
            boost::regex expression(Globals::globalConfig.sGameRegex);
            boost::match_results<std::string::const_iterator> what;
            if (!boost::regex_search(game.gamename, what, expression)) // Check if name matches the specified regex
                continue;
        }
        game.title = gameDetailsNode["title"].asString();
        game.icon = gameDetailsNode["icon"].asString();
        game.serials = gameDetailsNode["serials"].asString();
        game.changelog = gameDetailsNode["changelog"].asString();
        game.product_id = gameDetailsNode["product_id"].asString();

        // Make a vector of valid node names to make things easier
        std::vector<std::string> nodes;
        nodes.push_back("extras");
        nodes.push_back("installers");
        nodes.push_back("patches");
        nodes.push_back("languagepacks");
        nodes.push_back("dlcs");

        gameSpecificConfig conf;
        conf.dlConf = Globals::globalConfig.dlConf;
        if (Util::getGameSpecificConfig(game.gamename, &conf) > 0)
            std::cerr << game.gamename << " - Language: " << conf.dlConf.iInstallerLanguage << ", Platform: " << conf.dlConf.iInstallerPlatform << ", DLC: " << (conf.dlConf.bDLC ? "true" : "false") << std::endl;

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
                        fileDetails.galaxy_downlink_json_url = fileDetailsNode["galaxy_downlink_json_url"].asString();

                        if (nodeName != "extras" && !(fileDetails.platform & conf.dlConf.iInstallerPlatform))
                            continue;
                        if (nodeName != "extras" && !(fileDetails.language & conf.dlConf.iInstallerLanguage))
                            continue;
                    }

                    if (nodeName == "extras" && conf.dlConf.bExtras)
                        game.extras.push_back(fileDetails);
                    else if (nodeName == "installers" && conf.dlConf.bInstallers)
                        game.installers.push_back(fileDetails);
                    else if (nodeName == "patches" && conf.dlConf.bPatches)
                        game.patches.push_back(fileDetails);
                    else if (nodeName == "languagepacks" && conf.dlConf.bLanguagePacks)
                        game.languagepacks.push_back(fileDetails);
                    else if (nodeName == "dlcs" && conf.dlConf.bDLC)
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
    Globals::globalConfig.dlConf.bExtras = true;
    Globals::globalConfig.dlConf.bInstallers = true;
    Globals::globalConfig.dlConf.bPatches = true;
    Globals::globalConfig.dlConf.bLanguagePacks = true;
    Globals::globalConfig.dlConf.bDLC = true;
    Globals::globalConfig.sGameRegex = ".*";
    Globals::globalConfig.dlConf.iInstallerLanguage = Util::getOptionValue("all", GlobalConstants::LANGUAGES);
    Globals::globalConfig.dlConf.iInstallerPlatform = Util::getOptionValue("all", GlobalConstants::PLATFORMS);
    Globals::globalConfig.dlConf.vLanguagePriority.clear();
    Globals::globalConfig.dlConf.vPlatformPriority.clear();
    Globals::globalConfig.sIgnoreDLCCountRegex = ".*"; // Ignore DLC count for all games because GOG doesn't report DLC count correctly

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
    if (!gogAPI->isLoggedIn())
    {
        std::cout << "API not logged in. This feature doesn't work without valid API login." << std::endl;
        std::cout << "Try to login with --login-api" << std::endl;
        exit(1);
    }

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
                filepath = Util::makeFilepath(Globals::globalConfig.dirConf.sDirectory, filename, gamename);
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

    galaxyAPI* galaxy = new galaxyAPI(Globals::globalConfig.curlConf);
    if (!galaxy->init())
    {
        if (!galaxy->refreshLogin())
        {
            delete galaxy;
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* dlhandle = curl_easy_init();
    curl_easy_setopt(dlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(dlhandle, CURLOPT_USERAGENT, conf.curlConf.sUserAgent.c_str());
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(dlhandle, CURLOPT_CONNECTTIMEOUT, conf.curlConf.iTimeout);
    curl_easy_setopt(dlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(dlhandle, CURLOPT_SSL_VERIFYPEER, conf.curlConf.bVerifyPeer);
    curl_easy_setopt(dlhandle, CURLOPT_VERBOSE, conf.curlConf.bVerbose);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, conf.curlConf.iDownloadRate);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_TIME, conf.curlConf.iLowSpeedTimeout);
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_LIMIT, conf.curlConf.iLowSpeedTimeoutRate);

    if (!conf.curlConf.sCACertPath.empty())
        curl_easy_setopt(dlhandle, CURLOPT_CAINFO, conf.curlConf.sCACertPath.c_str());

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

        // Refresh Galaxy login if token is expired
        if (galaxy->isTokenExpired())
        {
            if (!galaxy->refreshLogin())
            {
                msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                delete galaxy;
                return;
            }
        }

        // Get downlink JSON from Galaxy API
        Json::Value downlinkJson;
        std::string response = galaxy->getResponse(gf.galaxy_downlink_json_url);

        if (response.empty())
        {
            msgQueue.push(Message("Found nothing in " + gf.galaxy_downlink_json_url + ", skipping file", MSGTYPE_WARNING, msg_prefix));
            continue;
        }

        try {
            std::istringstream iss(response);
            iss >> downlinkJson;
        } catch (const Json::Exception& exc) {
            msgQueue.push(Message("Could not parse JSON response, skipping file", MSGTYPE_WARNING, msg_prefix));
            continue;
        }

        if (!downlinkJson.isMember("downlink"))
        {
            msgQueue.push(Message("Invalid JSON response, skipping file", MSGTYPE_WARNING, msg_prefix));
            continue;
        }

        std::string xml;
        if (gf.type & (GFTYPE_INSTALLER | GFTYPE_PATCH) && conf.dlConf.bRemoteXML)
        {
            std::string xml_url;
            if (downlinkJson.isMember("checksum"))
                if (!downlinkJson["checksum"].empty())
                    xml_url = downlinkJson["checksum"].asString();

            // Get XML data
            if (conf.dlConf.bRemoteXML && !xml_url.empty())
                xml = galaxy->getResponse(xml_url);

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

        std::string url = downlinkJson["downlink"].asString();
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
            // Set timestamp for downloaded file to same value as file on server
            long filetime = -1;
            CURLcode res = curl_easy_getinfo(dlhandle, CURLINFO_FILETIME, &filetime);
            if (res == CURLE_OK && filetime >= 0)
            {
                std::time_t timestamp = (std::time_t)filetime;
                boost::filesystem::last_write_time(filepath, timestamp);
            }

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
        if (conf.dlConf.bAutomaticXMLCreation)
        {
            if (result == CURLE_OK)
            {
                if ((gf.type & GFTYPE_EXTRA) || (conf.dlConf.bRemoteXML && !bLocalXMLExists && xml.empty()))
                    createXMLQueue.push(gf);
            }
        }
    }

    curl_easy_cleanup(dlhandle);
    delete galaxy;

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

template <typename T> void Downloader::printProgress(const ThreadSafeQueue<T>& download_queue)
{
    // Print progress information until all threads have finished their tasks
    ProgressBar bar(Globals::globalConfig.bUnicode, Globals::globalConfig.bColor);
    unsigned int dl_status = DLSTATUS_NOTSTARTED;
    while (dl_status != DLSTATUS_FINISHED)
    {
        dl_status = DLSTATUS_NOTSTARTED;

        // Print progress information once per 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(Globals::globalConfig.iProgressInterval));
        std::cout << "\033[J\r" << std::flush; // Clear screen from the current line down to the bottom of the screen

        // Print messages from message queue first
        Message msg;
        while (msgQueue.try_pop(msg))
        {
            std::cout << msg.getFormattedString(Globals::globalConfig.bColor, true) << std::endl;
            if (Globals::globalConfig.bReport)
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

            std::string progress_percentage_text = Util::formattedString("%3.0f%% ", fraction * 100);
            int progress_percentage_text_length = progress_percentage_text.length() + 1;

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

            std::string progress_status_text = Util::formattedString(" %0.2f/%0.2fMB @ %0.2f%s ETA: %s", static_cast<double>(progress_info.dlnow)/1024/1024, static_cast<double>(progress_info.dltotal)/1024/1024, progress_info.rate, rate_unit.c_str(), eta_ss.str().c_str());
            int status_text_length = progress_status_text.length() + 1;

            if ((status_text_length + progress_percentage_text_length + bar_length) > iTermWidth)
                bar_length -= (status_text_length + progress_percentage_text_length + bar_length) - iTermWidth;

            // Don't draw progressbar if length is less than min_bar_length
            std::string progress_bar_text;
            if (bar_length >= min_bar_length)
                progress_bar_text = bar.createBarString(bar_length, fraction);

            progress_text = progress_percentage_text + progress_bar_text + progress_status_text;
            std::string filename_text = "#" + std::to_string(i) + " " + filename;
            Util::shortenStringToTerminalWidth(filename_text);

            vProgressText.push_back(filename_text);
            vProgressText.push_back(progress_text);
        }

        // Total download speed and number of remaining tasks in download queue
        if (dl_status != DLSTATUS_FINISHED)
        {
            std::ostringstream ss;
            if (Globals::globalConfig.iThreads > 1)
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
            ss << "Remaining: " << download_queue.size();
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

    galaxyAPI* galaxy = new galaxyAPI(Globals::globalConfig.curlConf);
    if (!galaxy->init())
    {
        if (!galaxy->refreshLogin())
        {
            delete galaxy;
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    // Create new GOG website handle
    Website* website = new Website();
    if (!website->IsLoggedIn())
    {
        delete galaxy;
        delete website;
        msgQueue.push(Message("Website not logged in", MSGTYPE_ERROR, msg_prefix));
        vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
        return;
    }

    // Set default game specific directory options to values from config
    DirectoryConfig dirConfDefault;
    dirConfDefault = config.dirConf;

    gameItem game_item;
    while (gameItemQueue.try_pop(game_item))
    {
        gameDetails game;

        gameSpecificConfig conf;
        conf.dlConf = config.dlConf;
        conf.dirConf = dirConfDefault;
        conf.dlConf.bIgnoreDLCCount = false;

        if (!config.bUpdateCache) // Disable game specific config files for cache update
        {
            int iOptionsOverridden = Util::getGameSpecificConfig(game_item.name, &conf);
            if (iOptionsOverridden > 0)
            {
                std::ostringstream ss;
                ss << game_item.name << " - " << iOptionsOverridden << " options overridden with game specific options" << std::endl;
                if (config.bVerbose)
                {
                    if (conf.dlConf.bIgnoreDLCCount)
                        ss << "\tIgnore DLC count" << std::endl;
                    if (conf.dlConf.bDLC != config.dlConf.bDLC)
                        ss << "\tDLC: " << (conf.dlConf.bDLC ? "true" : "false") << std::endl;
                    if (conf.dlConf.iInstallerLanguage != config.dlConf.iInstallerLanguage)
                        ss << "\tLanguage: " << Util::getOptionNameString(conf.dlConf.iInstallerLanguage, GlobalConstants::LANGUAGES) << std::endl;
                    if (conf.dlConf.vLanguagePriority != config.dlConf.vLanguagePriority)
                    {
                        ss << "\tLanguage priority:" << std::endl;
                        for (unsigned int j = 0; j < conf.dlConf.vLanguagePriority.size(); ++j)
                        {
                            ss << "\t  " << j << ": " << Util::getOptionNameString(conf.dlConf.vLanguagePriority[j], GlobalConstants::LANGUAGES) << std::endl;
                        }
                    }
                    if (conf.dlConf.iInstallerPlatform != config.dlConf.iInstallerPlatform)
                        ss << "\tPlatform: " << Util::getOptionNameString(conf.dlConf.iInstallerPlatform, GlobalConstants::PLATFORMS) << std::endl;
                    if (conf.dlConf.vPlatformPriority != config.dlConf.vPlatformPriority)
                    {
                        ss << "\tPlatform priority:" << std::endl;
                        for (unsigned int j = 0; j < conf.dlConf.vPlatformPriority.size(); ++j)
                        {
                            ss << "\t  " << j << ": " << Util::getOptionNameString(conf.dlConf.vPlatformPriority[j], GlobalConstants::PLATFORMS) << std::endl;
                        }
                    }
                }
                msgQueue.push(Message(ss.str(), MSGTYPE_INFO, msg_prefix));
            }
        }

        // Refresh Galaxy login if token is expired
        if (galaxy->isTokenExpired())
        {
            if (!galaxy->refreshLogin())
            {
                msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
                break;
            }
        }

        Json::Value product_info = galaxy->getProductInfo(game_item.id);
        game = galaxy->productInfoJsonToGameDetails(product_info, conf.dlConf);
        game.filterWithPriorities(conf);

        Json::Value gameDetailsJSON;

        if (!game_item.gamedetailsjson.empty())
            gameDetailsJSON = game_item.gamedetailsjson;

        if (conf.dlConf.bSaveSerials && game.serials.empty())
        {
            if (gameDetailsJSON.empty())
                gameDetailsJSON = website->getGameDetailsJSON(game_item.id);
            game.serials = Downloader::getSerialsFromJSON(gameDetailsJSON);
        }

        if (conf.dlConf.bSaveChangelogs && game.changelog.empty())
        {
            if (gameDetailsJSON.empty())
                gameDetailsJSON = website->getGameDetailsJSON(game_item.id);
            game.changelog = Downloader::getChangelogFromJSON(gameDetailsJSON);
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

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    delete galaxy;
    delete website;

    return;
}

void Downloader::saveGalaxyJSON()
{
    if (!Globals::galaxyConf.getJSON().empty())
    {
        std::ofstream ofs(Globals::galaxyConf.getFilepath());
        if (!ofs)
        {
            std::cerr << "Failed to write " << Globals::galaxyConf.getFilepath() << std::endl;
        }
        else
        {
            ofs << Globals::galaxyConf.getJSON() << std::endl;
            ofs.close();
        }
        if (!Globals::globalConfig.bRespectUmask)
            Util::setFilePermissions(Globals::galaxyConf.getFilepath(), boost::filesystem::owner_read | boost::filesystem::owner_write);
    }
}

void Downloader::galaxyInstallGame(const std::string& product_id, int build_index, const unsigned int& iGalaxyArch)
{
    if (build_index < 0)
        build_index = 0;

    std::string sPlatform;
    unsigned int iPlatform = Globals::globalConfig.dlConf.iGalaxyPlatform;
    if (iPlatform == GlobalConstants::PLATFORM_LINUX)
        sPlatform = "linux";
    else if (iPlatform == GlobalConstants::PLATFORM_MAC)
        sPlatform = "osx";
    else
        sPlatform = "windows";

    std::string sLanguage = "en";
    unsigned int iLanguage = Globals::globalConfig.dlConf.iGalaxyLanguage;
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        if (GlobalConstants::LANGUAGES[i].id == iLanguage)
        {
            sLanguage = GlobalConstants::LANGUAGES[i].code;
            break;
        }
    }

    std::string sGalaxyArch = "64";
    for (unsigned int i = 0; i < GlobalConstants::GALAXY_ARCHS.size(); ++i)
    {
        if (GlobalConstants::GALAXY_ARCHS[i].id == iGalaxyArch)
        {
            sGalaxyArch = GlobalConstants::GALAXY_ARCHS[i].code;
            break;
        }
    }

    Json::Value json = gogGalaxy->getProductBuilds(product_id, sPlatform);

    // JSON is empty and platform is Linux. Most likely cause is that Galaxy API doesn't have Linux support
    if (json.empty() && iPlatform == GlobalConstants::PLATFORM_LINUX)
    {
        std::cout << "Galaxy API doesn't have Linux support" << std::endl;
        return;
    }

    if (json["items"][build_index]["generation"].asInt() != 2)
    {
        std::cout << "Only generation 2 builds are supported currently" << std::endl;
        return;
    }

    std::string link = json["items"][build_index]["link"].asString();
    std::string buildHash;
    buildHash.assign(link.begin()+link.find_last_of("/")+1, link.end());

    json = gogGalaxy->getManifestV2(buildHash);
    std::string game_title = json["products"][0]["name"].asString();
    std::string install_directory = json["installDirectory"].asString();
    if (install_directory.empty())
        install_directory = product_id;

    std::string install_path = Globals::globalConfig.dirConf.sDirectory + install_directory;

    std::vector<galaxyDepotItem> items;
    for (unsigned int i = 0; i < json["depots"].size(); ++i)
    {
        bool bSelectedLanguage = false;
        bool bSelectedArch = false;
        for (unsigned int j = 0; j < json["depots"][i]["languages"].size(); ++j)
        {
            std::string language = json["depots"][i]["languages"][j].asString();
            if (language == "*" || language == sLanguage)
                bSelectedLanguage = true;
        }

        if (json["depots"][i].isMember("osBitness"))
        {
            for (unsigned int j = 0; j < json["depots"][i]["osBitness"].size(); ++j)
            {
                std::string osBitness = json["depots"][i]["osBitness"][j].asString();
                if (osBitness == "*" || osBitness == sGalaxyArch)
                    bSelectedArch = true;
            }
        }
        else
        {
            // No osBitness found, assume that we want to download this depot
            bSelectedArch = true;
        }

        if (!bSelectedLanguage || !bSelectedArch)
            continue;

        std::string depotHash = json["depots"][i]["manifest"].asString();
        std::string depot_product_id = json["depots"][i]["productId"].asString();

        if (depot_product_id.empty())
            depot_product_id = product_id;

        std::vector<galaxyDepotItem> vec = gogGalaxy->getDepotItemsVector(depotHash);

        // Set product id for items
        for (auto it = vec.begin(); it != vec.end(); ++it)
            it->product_id = depot_product_id;

        items.insert(std::end(items), std::begin(vec), std::end(vec));
    }

    uintmax_t totalSize = 0;
    for (unsigned int i = 0; i < items.size(); ++i)
    {
        if (Globals::globalConfig.bVerbose)
        {
            std::cout << items[i].path << std::endl;
            std::cout << "\tChunks: " << items[i].chunks.size() << std::endl;
            std::cout << "\tmd5: " << items[i].md5 << std::endl;
        }
        totalSize += items[i].totalSizeUncompressed;
        dlQueueGalaxy.push(items[i]);
    }

    double totalSizeMB = static_cast<double>(totalSize)/1024/1024;
    std::cout << game_title << std::endl;
    std::cout << "Files: " << items.size() << std::endl;
    std::cout << "Total size installed: " << totalSizeMB << " MB" << std::endl;

    // Limit thread count to number of items in download queue
    unsigned int iThreads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(dlQueueGalaxy.size()));

    // Create download threads
    std::vector<std::thread> vThreads;
    for (unsigned int i = 0; i < iThreads; ++i)
    {
        DownloadInfo dlInfo;
        dlInfo.setStatus(DLSTATUS_NOTSTARTED);
        vDownloadInfo.push_back(dlInfo);
        vThreads.push_back(std::thread(Downloader::processGalaxyDownloadQueue, install_path, Globals::globalConfig, i));
    }

    this->printProgress(dlQueueGalaxy);

    // Join threads
    for (unsigned int i = 0; i < vThreads.size(); ++i)
        vThreads[i].join();

    vThreads.clear();
    vDownloadInfo.clear();

    std::cout << "Checking for orphaned files" << std::endl;
    std::vector<std::string> orphans = this->galaxyGetOrphanedFiles(items, install_path);
    std::cout << "\t" << orphans.size() << " orphaned files" << std::endl;
    for (unsigned int i = 0; i < orphans.size(); ++i)
        std::cout << "\t" << orphans[i] << std::endl;
}

void Downloader::processGalaxyDownloadQueue(const std::string& install_path, Config conf, const unsigned int& tid)
{
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    galaxyAPI* galaxy = new galaxyAPI(Globals::globalConfig.curlConf);
    if (!galaxy->init())
    {
        if (!galaxy->refreshLogin())
        {
            delete galaxy;
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* dlhandle = curl_easy_init();
    curl_easy_setopt(dlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(dlhandle, CURLOPT_USERAGENT, conf.curlConf.sUserAgent.c_str());
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(dlhandle, CURLOPT_CONNECTTIMEOUT, conf.curlConf.iTimeout);
    curl_easy_setopt(dlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(dlhandle, CURLOPT_SSL_VERIFYPEER, conf.curlConf.bVerifyPeer);
    curl_easy_setopt(dlhandle, CURLOPT_VERBOSE, conf.curlConf.bVerbose);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, conf.curlConf.iDownloadRate);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_TIME, conf.curlConf.iLowSpeedTimeout);
    curl_easy_setopt(dlhandle, CURLOPT_LOW_SPEED_LIMIT, conf.curlConf.iLowSpeedTimeoutRate);

    if (!conf.curlConf.sCACertPath.empty())
        curl_easy_setopt(dlhandle, CURLOPT_CAINFO, conf.curlConf.sCACertPath.c_str());

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    galaxyDepotItem item;
    while (dlQueueGalaxy.try_pop(item))
    {
        vDownloadInfo[tid].setStatus(DLSTATUS_STARTING);

        boost::filesystem::path path = install_path + "/" + item.path;

        // Check that directory exists and create it
        boost::filesystem::path directory = path.parent_path();
        mtx_create_directories.lock(); // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory))
            {
                msgQueue.push(Message(directory.string() + " is not directory", MSGTYPE_ERROR, msg_prefix));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                delete galaxy;
                mtx_create_directories.unlock();
                return;
            }
        }
        else
        {
            if (!boost::filesystem::create_directories(directory))
            {
                msgQueue.push(Message("Failed to create directory: " + directory.string(), MSGTYPE_ERROR, msg_prefix));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                delete galaxy;
                mtx_create_directories.unlock();
                return;
            }
        }
        mtx_create_directories.unlock();

        vDownloadInfo[tid].setFilename(path.string());

        unsigned int start_chunk = 0;
        if (boost::filesystem::exists(path))
        {
            if (conf.bVerbose)
                msgQueue.push(Message("File already exists: " + path.string(), MSGTYPE_INFO, msg_prefix));

            unsigned int resume_chunk = 0;
            uintmax_t filesize = boost::filesystem::file_size(path);
            if (filesize == item.totalSizeUncompressed)
            {
                // File is same size
                if (Util::getFileHash(path.string(), RHASH_MD5) == item.md5)
                {
                    msgQueue.push(Message(path.string() + ": OK", MSGTYPE_SUCCESS, msg_prefix));
                    continue;
                }
                else
                {
                    msgQueue.push(Message(path.string() + ": MD5 mismatch", MSGTYPE_WARNING, msg_prefix));
                    if (!boost::filesystem::remove(path))
                    {
                        msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix));
                        continue;
                    }
                }
            }
            else if (filesize > item.totalSizeUncompressed)
            {
                // File is bigger than on server, delete old file and start from beginning
                msgQueue.push(Message(path.string() + ": File is bigger than expected. Deleting old file and starting from beginning", MSGTYPE_INFO, msg_prefix));
                if (!boost::filesystem::remove(path))
                {
                    msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix));
                    continue;
                }
            }
            else
            {
                // File is smaller than on server, resume
                for (unsigned int j = 0; j < item.chunks.size(); ++j)
                {
                    if (item.chunks[j].offset_uncompressed == filesize)
                    {
                        resume_chunk = j;
                        break;
                    }
                }

                if (resume_chunk > 0)
                {
                    msgQueue.push(Message(path.string() + ": Resume from chunk " + std::to_string(resume_chunk), MSGTYPE_INFO, msg_prefix));
                    // Get chunk hash for previous chunk
                    FILE* f = fopen(path.string().c_str(), "r");
                    if (!f)
                    {
                        msgQueue.push(Message(path.string() + ": Failed to open", MSGTYPE_ERROR, msg_prefix));
                        continue;
                    }

                    unsigned int previous_chunk = resume_chunk - 1;
                    uintmax_t chunk_size = item.chunks[previous_chunk].size_uncompressed;
                    // use fseeko to support large files on 32 bit platforms
                    fseeko(f, item.chunks[previous_chunk].offset_uncompressed, SEEK_SET);
                    unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
                    if (chunk == NULL)
                    {
                        msgQueue.push(Message(path.string() + ": Memory error - Chunk " + std::to_string(resume_chunk), MSGTYPE_ERROR, msg_prefix));
                        fclose(f);
                        continue;
                    }

                    uintmax_t fread_size = fread(chunk, 1, chunk_size, f);
                    fclose(f);

                    if (fread_size != chunk_size)
                    {
                        msgQueue.push(Message(path.string() + ": Read error - Chunk " + std::to_string(resume_chunk), MSGTYPE_ERROR, msg_prefix));
                        free(chunk);
                        continue;
                    }
                    std::string chunk_hash = Util::getChunkHash(chunk, chunk_size, RHASH_MD5);
                    free(chunk);

                    if (chunk_hash == item.chunks[previous_chunk].md5_uncompressed)
                    {
                        // Hash for previous chunk matches, resume at this position
                        start_chunk = resume_chunk;
                    }
                    else
                    {
                        // Hash for previous chunk is different, delete old file and start from beginning
                        msgQueue.push(Message(path.string() + ": Chunk hash is different. Deleting old file and starting from beginning.", MSGTYPE_WARNING, msg_prefix));
                        if (!boost::filesystem::remove(path))
                        {
                            msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix));
                            continue;
                        }
                    }
                }
                else
                {
                    msgQueue.push(Message(path.string() + ": Failed to find valid resume position. Deleting old file and starting from beginning.", MSGTYPE_WARNING, msg_prefix));
                    if (!boost::filesystem::remove(path))
                    {
                        msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix));
                        continue;
                    }
                }
            }
        }

        std::time_t timestamp = -1;
        for (unsigned int j = start_chunk; j < item.chunks.size(); ++j)
        {
            ChunkMemoryStruct chunk;
            chunk.memory = (char *) malloc(1);
            chunk.size = 0;

            // Refresh Galaxy login if token is expired
            if (galaxy->isTokenExpired())
            {
                if (!galaxy->refreshLogin())
                {
                    msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix));
                    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                    free(chunk.memory);
                    delete galaxy;
                    return;
                }
            }

            Json::Value json = galaxy->getSecureLink(item.product_id, galaxy->hashToGalaxyPath(item.chunks[j].md5_compressed));

            // Prefer edgecast urls
            bool bPreferEdgecast = true;
            unsigned int idx = 0;
            for (unsigned int k = 0; k < json["urls"].size(); ++k)
            {
                std::string endpoint_name = json["urls"][k]["endpoint_name"].asString();
                if (bPreferEdgecast)
                {
                    if (endpoint_name == "edgecast")
                    {
                        idx = k;
                        break;
                    }
                }
            }

            // Build url according to url_format
            std::string link_base_url = json["urls"][idx]["parameters"]["base_url"].asString();
            std::string link_path = json["urls"][idx]["parameters"]["path"].asString();
            std::string link_token = json["urls"][idx]["parameters"]["token"].asString();
            std::string url = json["urls"][idx]["url_format"].asString();

            while(Util::replaceString(url, "{base_url}", link_base_url));
            while(Util::replaceString(url, "{path}", link_path));
            while(Util::replaceString(url, "{token}", link_token));

            curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
            curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, WriteChunkMemoryCallback);
            curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, &chunk);
            curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
            curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);
            curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

            std::string filepath_and_chunk = path.string() + " (chunk " + std::to_string(j + 1) + "/" + std::to_string(item.chunks.size()) + ")";
            vDownloadInfo[tid].setFilename(filepath_and_chunk);

            if (Globals::globalConfig.iWait > 0)
                usleep(Globals::globalConfig.iWait); // Delay the request by specified time

            xferinfo.offset = 0;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();

            CURLcode result = curl_easy_perform(dlhandle);

            curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
            curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
            curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 0L);

            if (result != CURLE_OK)
            {
                msgQueue.push(Message(std::string(curl_easy_strerror(result)), MSGTYPE_ERROR, msg_prefix));
                if (result == CURLE_HTTP_RETURNED_ERROR)
                {
                    long int response_code = 0;
                    result = curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                    if (result == CURLE_OK)
                        msgQueue.push(Message("HTTP ERROR: " + std::to_string(response_code) + " (" + url + ")", MSGTYPE_ERROR, msg_prefix));
                    else
                        msgQueue.push(Message("HTTP ERROR: failed to get error code: " + std::string(curl_easy_strerror(result)) + " (" + url + ")", MSGTYPE_ERROR, msg_prefix));
                }
            }
            else
            {
                // Get timestamp for downloaded file
                long filetime = -1;
                result = curl_easy_getinfo(dlhandle, CURLINFO_FILETIME, &filetime);
                if (result == CURLE_OK && filetime >= 0)
                    timestamp = (std::time_t)filetime;
            }

            std::ofstream ofs(path.string(), std::ofstream::out | std::ofstream::binary | std::ofstream::app);
            if (ofs)
            {
                boost::iostreams::filtering_streambuf<boost::iostreams::output> output;
                output.push(boost::iostreams::zlib_decompressor(GlobalConstants::ZLIB_WINDOW_SIZE));
                output.push(ofs);
                boost::iostreams::write(output, chunk.memory, chunk.size);
            }
            if (ofs)
                ofs.close();

            free(chunk.memory);
        }

        // Set timestamp for downloaded file to same value as file on server
        if (boost::filesystem::exists(path) && timestamp >= 0)
            boost::filesystem::last_write_time(path, timestamp);

        msgQueue.push(Message("Download complete: " + path.string(), MSGTYPE_SUCCESS, msg_prefix));
    }

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    delete galaxy;
    curl_easy_cleanup(dlhandle);

    return;
}

void Downloader::galaxyShowBuilds(const std::string& product_id, int build_index)
{
    std::string sPlatform;
    unsigned int iPlatform = Globals::globalConfig.dlConf.iGalaxyPlatform;
    if (iPlatform == GlobalConstants::PLATFORM_LINUX)
        sPlatform = "linux";
    else if (iPlatform == GlobalConstants::PLATFORM_MAC)
        sPlatform = "osx";
    else
        sPlatform = "windows";

    Json::Value json = gogGalaxy->getProductBuilds(product_id, sPlatform);

    // JSON is empty and platform is Linux. Most likely cause is that Galaxy API doesn't have Linux support
    if (json.empty() && iPlatform == GlobalConstants::PLATFORM_LINUX)
    {
        std::cout << "Galaxy API doesn't have Linux support" << std::endl;
        return;
    }

    if (build_index < 0)
    {
        for (unsigned int i = 0; i < json["items"].size(); ++i)
        {
            std::cout << i << ": " << "Version " << json["items"][i]["version_name"].asString() << " - " << json["items"][i]["date_published"].asString() << " (Gen " << json["items"][i]["generation"].asInt() << ")" << std::endl;
        }
        return;
    }

    std::string link = json["items"][build_index]["link"].asString();

    if (json["items"][build_index]["generation"].asInt() == 1)
    {
        json = gogGalaxy->getManifestV1(link);
    }
    else if (json["items"][build_index]["generation"].asInt() == 2)
    {
        std::string buildHash;
        buildHash.assign(link.begin()+link.find_last_of("/")+1, link.end());
        json = gogGalaxy->getManifestV2(buildHash);
    }
    else
    {
        std::cout << "Only generation 1 and 2 builds are supported currently" << std::endl;
        return;
    }

    std::cout << json << std::endl;

    return;
}

std::vector<std::string> Downloader::galaxyGetOrphanedFiles(const std::vector<galaxyDepotItem>& items, const std::string& install_path)
{
    std::vector<std::string> orphans;
    std::vector<std::string> item_paths;
    for (unsigned int i = 0; i < items.size(); ++i)
        item_paths.push_back(install_path + "/" + items[i].path);

    std::vector<boost::filesystem::path> filepath_vector;
    try
    {
        std::size_t pathlen = Globals::globalConfig.dirConf.sDirectory.length();
        if (boost::filesystem::exists(install_path))
        {
            if (boost::filesystem::is_directory(install_path))
            {
                // Recursively iterate over files in directory
                boost::filesystem::recursive_directory_iterator end_iter;
                boost::filesystem::recursive_directory_iterator dir_iter(install_path);
                while (dir_iter != end_iter)
                {
                    if (boost::filesystem::is_regular_file(dir_iter->status()))
                    {
                        std::string filepath = dir_iter->path().string();
                        if (Globals::globalConfig.ignorelist.isBlacklisted(filepath.substr(pathlen)))
                        {
                            if (Globals::globalConfig.bVerbose)
                                std::cerr << "skipped ignorelisted file " << filepath << std::endl;
                        }
                        else
                        {
                            filepath_vector.push_back(dir_iter->path());
                        }
                    }
                    dir_iter++;
                }
            }
        }
        else
            std::cerr << install_path << " does not exist" << std::endl;
    }
    catch (const boost::filesystem::filesystem_error& ex)
    {
        std::cout << ex.what() << std::endl;
    }

    std::sort(item_paths.begin(), item_paths.end());
    std::sort(filepath_vector.begin(), filepath_vector.end());

    if (!filepath_vector.empty())
    {
        for (unsigned int i = 0; i < filepath_vector.size(); ++i)
        {
            bool bFileIsOrphaned = true;
            for (std::vector<std::string>::iterator it = item_paths.begin(); it != item_paths.end(); it++)
            {
                boost::filesystem::path item_path = *it;
                boost::filesystem::path file_path = filepath_vector[i].native();

                if (item_path == file_path)
                {
                    bFileIsOrphaned = false;
                    item_paths.erase(it);
                    break;
                }
            }

            if (bFileIsOrphaned)
                orphans.push_back(filepath_vector[i].string());
        }
    }

    return orphans;
}
