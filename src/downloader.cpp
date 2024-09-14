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
#include "ziputil.h"

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
#include <termios.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/device/back_inserter.hpp>

namespace bptime = boost::posix_time;

struct cloudSaveFile {
    boost::posix_time::ptime lastModified;
    unsigned long long fileSize;
    std::string path;
    std::string location;
};

std::vector<std::string> Globals::vOwnedGamesIds;
std::vector<DownloadInfo> vDownloadInfo;
ThreadSafeQueue<gameFile> dlQueue;
ThreadSafeQueue<cloudSaveFile> dlCloudSaveQueue;
ThreadSafeQueue<Message> msgQueue;
ThreadSafeQueue<gameFile> createXMLQueue;
ThreadSafeQueue<gameItem> gameItemQueue;
ThreadSafeQueue<gameDetails> gameDetailsQueue;
ThreadSafeQueue<galaxyDepotItem> dlQueueGalaxy;
ThreadSafeQueue<zipFileEntry> dlQueueGalaxy_MojoSetupHack;
std::mutex mtx_create_directories; // Mutex for creating directories in Downloader::processDownloadQueue
std::atomic<unsigned long long> iTotalRemainingBytes(0);

std::string username() {
    auto user = std::getenv("USER");
    return user ? user : std::string();
}

void dirForEachHelper(const boost::filesystem::path &location, std::function<void(boost::filesystem::directory_iterator)> &f) {
    boost::filesystem::directory_iterator begin { location };
    boost::filesystem::directory_iterator end;

    for(boost::filesystem::directory_iterator curr_dir { begin }; curr_dir != end; ++curr_dir) {
        if(boost::filesystem::is_directory(*curr_dir)) {

            dirForEachHelper(*curr_dir, f);
        }
        else {
            f(curr_dir);
        }
    }
}

void dirForEach(const std::string &location, std::function<void(boost::filesystem::directory_iterator)> &&f) {
    dirForEachHelper(location, f);
}

bool whitelisted(const std::string &path) {
    auto &whitelist = Globals::globalConfig.cloudWhiteList;
    auto &blacklist = Globals::globalConfig.cloudBlackList;

    // Check if path is whitelisted
    if(!whitelist.empty()) {
        return std::any_of(std::begin(whitelist), std::end(whitelist), [&path](const std::string &whitelisted) {
            return
                path.rfind(whitelisted, 0) == 0 &&
                (path.size() == whitelisted.size() || path[whitelisted.size()] == '/');
        });
    }

    // Check if blacklisted
    if(!blacklist.empty()) {
        return !std::any_of(std::begin(blacklist), std::end(blacklist), [&path](const std::string &blacklisted) {
            return
                path.rfind(blacklisted, 0) == 0 &&
                (path.size() == blacklisted.size() || path[blacklisted.size()] == '/');
        });
    }

    return true;
}

Downloader::Downloader()
{
    if (Globals::globalConfig.bLogin)
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
    Util::CurlHandleSetDefaultOptions(curlhandle, Globals::globalConfig.curlConf);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallback);
    curl_easy_setopt(curlhandle, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, Downloader::readData);



    // Create new GOG website handle
    gogWebsite = new Website();

    progressbar = new ProgressBar(Globals::globalConfig.bUnicode, Globals::globalConfig.bColor);

    if (boost::filesystem::exists(Globals::galaxyConf.getFilepath()))
    {
        Json::Value json = Util::readJsonFile(Globals::galaxyConf.getFilepath());
        if (!json.isMember("expires_at"))
        {
            std::time_t last_modified = boost::filesystem::last_write_time(Globals::galaxyConf.getFilepath());
            Json::Value::LargestInt expires_in = json["expires_in"].asLargestInt();
            json["expires_at"] = expires_in + last_modified;
        }
        Globals::galaxyConf.setJSON(json);
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
    bool bWebsiteIsLoggedIn = gogWebsite->IsLoggedIn();
    bool bGalaxyIsLoggedIn = !gogGalaxy->isTokenExpired();

    if (!bGalaxyIsLoggedIn)
    {
        if (gogGalaxy->refreshLogin())
            bGalaxyIsLoggedIn = true;
    }

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
    bool headless = false;
    bool bForceGUI = false;
    #ifdef USE_QT_GUI_LOGIN
        bForceGUI = Globals::globalConfig.bForceGUILogin;
    #endif

    if (!Globals::globalConfig.sEmail.empty() && !Globals::globalConfig.sPassword.empty())
    {
        email = Globals::globalConfig.sEmail;
        password = Globals::globalConfig.sPassword;
    }
    else if (!(bForceGUI || Globals::globalConfig.bForceBrowserLogin))
    {
        if (!isatty(STDIN_FILENO)) {
            /* Attempt to read this stuff from elsewhere */
            bool cookie_gone = !(boost::filesystem::exists(Globals::globalConfig.curlConf.sCookiePath));
            bool tokens_gone = !(boost::filesystem::exists(Globals::globalConfig.sConfigDirectory + "/galaxy_tokens.json"));
            std::cout << Globals::globalConfig.curlConf.sCookiePath << std::endl;
            std::cout << (Globals::globalConfig.sConfigDirectory + "/galaxy_tokens.json") << std::endl;
            if(cookie_gone || tokens_gone) {
                std::cerr << "Unable to read email and password" << std::endl;
                return 0;
            } else headless = true;
        } else {
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
    }

    if ((email.empty() || password.empty())
        && !(Globals::globalConfig.bForceBrowserLogin || headless || bForceGUI)
    )
    {
        std::cerr << "Email and/or password empty" << std::endl;
        return 0;
    }

    // Login to website and Galaxy API
    if (Globals::globalConfig.bLogin)
    {
        // Delete old cookies
        if (boost::filesystem::exists(Globals::globalConfig.curlConf.sCookiePath))
            if (!boost::filesystem::remove(Globals::globalConfig.curlConf.sCookiePath))
                std::cerr << "Failed to delete " << Globals::globalConfig.curlConf.sCookiePath << std::endl;

        int iLoginResult = gogWebsite->Login(email, password);

        if (iLoginResult < 1)
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
        }

        if (gogWebsite->IsLoggedIn())
        {
            std::cerr << "HTTP: Login successful" << std::endl;
        }
        else
        {
            std::cerr << "HTTP: Login failed" << std::endl;
            return 0;
        }
    }

    return 1;
}

void Downloader::checkNotifications()
{
    Json::Value userData = gogGalaxy->getUserData();

    if (userData.empty())
    {
        std::cout << "Empty JSON response" << std::endl;
        return;
    }

    if (!userData.isMember("updates"))
    {
        std::cout << "Invalid JSON response" << std::endl;
        return;
    }

    std::cout << "New forum replies: " << userData["updates"]["messages"].asInt() << std::endl;
    std::cout << "Updated games: " << userData["updates"]["products"].asInt() << std::endl;
    std::cout << "Unread chat messages: " << userData["updates"]["unreadChatMessages"].asInt() << std::endl;
    std::cout << "Pending friend requests: " << userData["updates"]["pendingFriendRequests"].asInt() << std::endl;
}

void Downloader::clearUpdateNotifications()
{
    Json::Value userData = gogGalaxy->getUserData();
    if (userData.empty())
    {
        return;
    }

    if (!userData.isMember("updates"))
    {
        return;
    }

    if (userData["updates"]["products"].asInt() < 1)
    {
        std::cout << "No updates" << std::endl;
        return;
    }

    Globals::globalConfig.bUpdated = true;
    this->getGameList();

    for (unsigned int i = 0; i < gameItems.size(); ++i)
    {
        // Getting game details should remove the update flag
        std::cerr << "\033[KClearing update flags " << i+1 << " / " << gameItems.size() << "\r" << std::flush;
        Json::Value details = gogWebsite->getGameDetailsJSON(gameItems[i].id);
    }
    std::cerr << std::endl;
}

void Downloader::getGameList()
{
    gameItems = gogWebsite->getGames();
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
        unsigned int threads = std::min(Globals::globalConfig.iInfoThreads, static_cast<unsigned int>(gameItemQueue.size()));
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
            std::this_thread::sleep_for(std::chrono::milliseconds(Globals::globalConfig.iProgressInterval));
            std::cerr << "\033[J\r" << std::flush; // Clear screen from the current line down to the bottom of the screen

            // Print messages from message queue first
            Message msg;
            while (msgQueue.try_pop(msg))
            {
                if (msg.getLevel() <= Globals::globalConfig.iMsgLevel)
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
    if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_GAMES ||
        Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_TRANSFORMATIONS)
    {
        if (gameItems.empty())
            this->getGameList();

        for (unsigned int i = 0; i < gameItems.size(); ++i)
        {
            std::string gamename = gameItems[i].name;
            if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_TRANSFORMATIONS)
            {
                std::cout << gamename << " -> " << Util::transformGamename(gamename) << std::endl;
            }
            else
            {
                if (gameItems[i].updates > 0)
                {
                    gamename += " [" + std::to_string(gameItems[i].updates) + "]";
                    std::string color = gameItems[i].isnew ? "01;34" : "32";
                    if (Globals::globalConfig.bColor)
                        gamename = "\033[" + color + "m" + gamename + "\033[0m";
                }
                else
                {
                    if (Globals::globalConfig.bColor && gameItems[i].isnew)
                        gamename = "\033[01;34m" + gamename + "\033[0m";
                }
                std::cout << gamename << std::endl;
                for (unsigned int j = 0; j < gameItems[i].dlcnames.size(); ++j)
                    std::cout << "+> " << gameItems[i].dlcnames[j] << std::endl;
            }
        }
    }
    else if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_TAGS)
    {
        std::map<std::string, std::string> tags;
        tags = gogWebsite->getTags();

        if (!tags.empty())
        {
            for (auto tag : tags)
            {
                std::cout << tag.first  << " = " << tag.second << std::endl;
            }
        }
    }
    else if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_USERDATA)
    {
        Json::Value userdata;
        std::istringstream response(gogWebsite->getResponse("https://embed.gog.com/userData.json"));
        try
        {
            response >> userdata;
        }
        catch(const Json::Exception& exc)
        {
            std::cerr << "Failed to get user data" << std::endl;
            return 1;
        }
        std::cout << userdata << std::endl;
    }
    else
    {
        if (this->games.empty()) {
            int res = this->getGameDetails();
            if (res > 0)
                return res;
        }

        if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_DETAILS_JSON)
        {
            Json::Value json(Json::arrayValue);
            for (auto game : this->games)
                json.append(game.getDetailsAsJson());

            std::cout << json << std::endl;
        }
        else if (Globals::globalConfig.iListFormat == GlobalConstants::LIST_FORMAT_DETAILS_TEXT)
        {
            for (auto game : this->games)
                printGameDetailsAsText(game);
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
        Util::getGameSpecificConfig(vGameFiles[i].gamename, &conf);

        unsigned int type = vGameFiles[i].type;
        if (!(type & conf.dlConf.iInclude))
            continue;

        std::string filepath = vGameFiles[i].getFilepath();
        if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
        {
            if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
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

        Json::Value downlinkJson = gogGalaxy->getResponseJson(vGameFiles[i].galaxy_downlink_json_url);

        if (downlinkJson.empty())
        {
            std::cerr << "Empty JSON response, skipping file" << std::endl;
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
        if (XML.empty() && (type & GlobalConstants::GFTYPE_EXTRA))
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

    for (unsigned int i = 0; i < games.size(); ++i)
    {
        gameSpecificConfig conf;
        conf.dlConf = Globals::globalConfig.dlConf;
        conf.dirConf = Globals::globalConfig.dirConf;
        Util::getGameSpecificConfig(games[i].gamename, &conf);

        if (conf.dlConf.bSaveSerials && !games[i].serials.empty())
        {
            std::string filepath = games[i].getSerialsFilepath();
            this->saveSerials(games[i].serials, filepath);
        }

        if (conf.dlConf.bSaveLogo && !games[i].logo.empty())
        {
            std::string filepath = games[i].getLogoFilepath();
            this->downloadFile(games[i].logo, filepath, "", games[i].gamename);
        }

        if (conf.dlConf.bSaveIcon && !games[i].icon.empty())
        {
            std::string filepath = games[i].getIconFilepath();
            this->downloadFile(games[i].icon, filepath, "", games[i].gamename);
        }

        if (conf.dlConf.bSaveChangelogs && !games[i].changelog.empty())
        {
            std::string filepath = games[i].getChangelogFilepath();
            this->saveChangelog(games[i].changelog, filepath);
        }

        if (conf.dlConf.bSaveGameDetailsJson && !games[i].gameDetailsJson.empty())
        {
            std::string filepath = games[i].getGameDetailsJsonFilepath();
            this->saveJsonFile(games[i].gameDetailsJson, filepath);
        }

        if (conf.dlConf.bSaveProductJson && !games[i].productJson.empty())
        {
            std::string filepath = games[i].getProductJsonFilepath();
            this->saveJsonFile(games[i].productJson, filepath);
        }

        if ((conf.dlConf.iInclude & GlobalConstants::GFTYPE_DLC) && !games[i].dlcs.empty())
        {
            for (unsigned int j = 0; j < games[i].dlcs.size(); ++j)
            {
                if (conf.dlConf.bSaveSerials && !games[i].dlcs[j].serials.empty())
                {
                    std::string filepath = games[i].dlcs[j].getSerialsFilepath();
                    this->saveSerials(games[i].dlcs[j].serials, filepath);
                }
                if (conf.dlConf.bSaveLogo && !games[i].dlcs[j].logo.empty())
                {
                    std::string filepath = games[i].dlcs[j].getLogoFilepath();
                    this->downloadFile(games[i].dlcs[j].logo, filepath, "", games[i].dlcs[j].gamename);
                }
                if (conf.dlConf.bSaveIcon && !games[i].dlcs[j].icon.empty())
                {
                    std::string filepath = games[i].dlcs[j].getIconFilepath();
                    this->downloadFile(games[i].dlcs[j].icon, filepath, "", games[i].dlcs[j].gamename);
                }
                if (conf.dlConf.bSaveChangelogs && !games[i].dlcs[j].changelog.empty())
                {
                    std::string filepath = games[i].dlcs[j].getChangelogFilepath();
                    this->saveChangelog(games[i].dlcs[j].changelog, filepath);
                }
                if (conf.dlConf.bSaveProductJson && !games[i].dlcs[j].productJson.empty())
                {
                    std::string filepath = games[i].dlcs[j].getProductJsonFilepath();
                    this->saveJsonFile(games[i].dlcs[j].productJson, filepath);
                }
            }
        }

        auto vFiles = games[i].getGameFileVectorFiltered(conf.dlConf.iInclude);
        for (auto gf : vFiles)
        {
            dlQueue.push(gf);
            unsigned long long filesize = 0;
            try
            {
                filesize = std::stoll(gf.size);
            }
            catch (std::invalid_argument& e)
            {
                filesize = 0;
            }
            iTotalRemainingBytes.fetch_add(filesize);
        }

    }

    if (!dlQueue.empty())
    {
        unsigned long long totalSizeBytes = iTotalRemainingBytes.load();
        std::cout << "Total size: " << Util::makeSizeString(totalSizeBytes) << std::endl;

        if (Globals::globalConfig.dlConf.bFreeSpaceCheck)
        {
            boost::filesystem::path path = boost::filesystem::absolute(Globals::globalConfig.dirConf.sDirectory);
            while(!boost::filesystem::exists(path) && !path.empty())
            {
                path = path.parent_path();
            }

            if(boost::filesystem::exists(path) && !path.empty())
            {
                boost::filesystem::space_info space = boost::filesystem::space(path);

                if (space.available < totalSizeBytes)
                {
                    std::cerr << "Not enough free space in " << boost::filesystem::canonical(path) << " ("
                    << Util::makeSizeString(space.available) << ")"<< std::endl;
                    exit(1);
                }
            }
        }

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
            // Check if file is complete so we can skip it instead of resuming
            if (!xml_data.empty())
            {
                off_t filesize_xml;
                off_t filesize_local = boost::filesystem::file_size(filepath);

                tinyxml2::XMLDocument remote_xml;
                remote_xml.Parse(xml_data.c_str());
                tinyxml2::XMLElement *fileElem = remote_xml.FirstChildElement("file");
                if (fileElem)
                {
                    std::string total_size = fileElem->Attribute("total_size");
                    try
                    {
                        filesize_xml = std::stoull(total_size);
                    }
                    catch (std::invalid_argument& e)
                    {
                        filesize_xml = 0;
                    }
                    if (filesize_local == filesize_xml)
                    {
                        std::cout << "Skipping complete file: " + filepath << std::endl;
                        fclose(outfile);

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

                        res = CURLE_OK;
                        return res;
                    }
                }
            }

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
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    res = this->beginDownload();
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);

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
    if ((res == CURLE_PARTIAL_FILE || res == CURLE_OPERATION_TIMEDOUT || res == CURLE_RECV_ERROR) && (this->retries < Globals::globalConfig.iRetries) )
    {
        this->retries++;

        std::cerr << std::endl << "Retry " << this->retries << "/" << Globals::globalConfig.iRetries;
        if (res == CURLE_PARTIAL_FILE)
            std::cerr << " (partial download)";
        else if (res == CURLE_OPERATION_TIMEDOUT)
            std::cerr << " (timeout)";
        else if (res == CURLE_RECV_ERROR)
            std::cerr << " (failed receiving network data)";
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
            try
            {
                boost::filesystem::last_write_time(filepath, timestamp);
            }
            catch(const boost::filesystem::filesystem_error& e)
            {
                std::cerr << e.what() << std::endl;
            }
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
            long int response_code = 0;
            if (result == CURLE_HTTP_RETURNED_ERROR)
            {
                curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
            }
            if  (
                    /* File doesn't exist so only accept if everything was OK */
                    (!bFileExists && result == CURLE_OK) ||
                    /* File exists so also accept CURLE_RANGE_ERROR and response code 416 */
                    (bFileExists && (result == CURLE_OK || result == CURLE_RANGE_ERROR || response_code == 416))
                )
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
        try
        {
            boost::filesystem::last_write_time(filepath, timestamp);
        }
        catch(const boost::filesystem::filesystem_error& e)
        {
            std::cerr << e.what() << std::endl;
        }
    }
    curl_easy_setopt(curlhandle, CURLOPT_FILETIME, 0L);

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

    int max_retries = std::min(3, Globals::globalConfig.iRetries);
    CURLcode result = Util::CurlHandleGetResponse(curlhandle, response, max_retries);

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
    curl_off_t curl_rate;
    // trying to get rate and setting to NaN if it fails
    if (CURLE_OK != curl_easy_getinfo(downloader->curlhandle, CURLINFO_SPEED_DOWNLOAD_T, &curl_rate))
        rate = std::numeric_limits<double>::quiet_NaN();
    else
        rate = static_cast<double>(curl_rate);

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

        std::string etastring = Util::makeEtaString((dltotal - dlnow), rate);

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
        std::string status_text = Util::formattedString(" %0.2f/%0.2fMB @ %0.2f%s ETA: %s\r", static_cast<double>(dlnow)/1024/1024, static_cast<double>(dltotal)/1024/1024, rate, rate_unit.c_str(), etastring.c_str());
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
        std::string xhtml = Util::htmlToXhtml(cdkey);
        tinyxml2::XMLDocument doc;
        doc.Parse(xhtml.c_str());
        tinyxml2::XMLNode* node = doc.FirstChildElement("html");
        while(node)
        {
            tinyxml2::XMLElement *element = node->ToElement();
            const char* text = element->GetText();
            if (text)
                serials << text << std::endl;

            node = Util::nextXMLNode(node);
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
    Globals::globalConfig.dlConf.iInclude = Util::getOptionValue("all", GlobalConstants::INCLUDE_OPTIONS);
    Globals::globalConfig.dlConf.iInstallerLanguage = Util::getOptionValue("all", GlobalConstants::LANGUAGES);
    Globals::globalConfig.dlConf.iInstallerPlatform = Util::getOptionValue("all", GlobalConstants::PLATFORMS);
    Globals::globalConfig.dlConf.vLanguagePriority.clear();
    Globals::globalConfig.dlConf.vPlatformPriority.clear();
    Config config = Globals::globalConfig;

    // Checking orphans after download.
    // Game details have already been retrieved but possibly filtered.
    // Therefore we need to clear game details and get them again.
    if (config.bDownload)
    {
        this->gameItems.clear();
        this->games.clear();
    }

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
                                    if (config.iMsgLevel >= MSGLEVEL_VERBOSE)
                                        std::cerr << "skipped ignorelisted file " << filepath << std::endl;
                                } else if (config.blacklist.isBlacklisted(filepath.substr(pathlen))) {
                                    if (config.iMsgLevel >= MSGLEVEL_VERBOSE)
                                        std::cerr << "skipped blacklisted file " << filepath << std::endl;
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
            if (Globals::globalConfig.dlConf.bDeleteOrphans)
            {
                std::string filepath = orphans[i];
                std::cout << "Deleting " << filepath << std::endl;
                if (boost::filesystem::exists(filepath))
                    if (!boost::filesystem::remove(filepath))
                        std::cerr << "Failed to delete " << filepath << std::endl;
            }
            else
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
        if (!(type & Globals::globalConfig.dlConf.iInclude))
            continue;

        boost::filesystem::path filepath = vGameFiles[i].getFilepath();

        if (Globals::globalConfig.blacklist.isBlacklisted(filepath.native()))
            continue;

        std::string filePathString = filepath.filename().string();
        std::string gamename = vGameFiles[i].gamename;

        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
        {
            uintmax_t filesize = boost::filesystem::file_size(filepath);
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

            if (Globals::globalConfig.bSizeOnly)
            {
                // Check for incomplete file by comparing the filesizes
                if (filesize_xml > 0 && filesize_xml != filesize)
                {
                    addStatusLine("FS", gamename, filePathString, filesize, "");
                    continue;
                }
                else
                {
                    addStatusLine("OK", gamename, filePathString, filesize, "");
                    continue;
                }
            }
            else
            {
                std::string remoteHash;
                bool bHashOK = true; // assume hash OK

                // GOG only provides xml data for installers, patches and language packs
                if (type & (GlobalConstants::GFTYPE_INSTALLER | GlobalConstants::GFTYPE_PATCH | GlobalConstants::GFTYPE_LANGPACK))
                    remoteHash = this->getRemoteFileHash(vGameFiles[i]);
                std::string localHash = Util::getLocalFileHash(Globals::globalConfig.sXMLDirectory, filepath.string(), gamename, Globals::globalConfig.bUseFastCheck);

                if (!remoteHash.empty())
                {
                    if (remoteHash != localHash)
                        bHashOK = false;
                    else
                    {
                        // Check for incomplete file by comparing the filesizes
                        // Remote hash was saved but download was incomplete and therefore getLocalFileHash returned the same as getRemoteFileHash
                        if (filesize_xml > 0 && filesize_xml != filesize)
                        {
                            localHash = Util::getFileHash(path.string(), RHASH_MD5);
                            addStatusLine("FS", gamename, filePathString, filesize, localHash);
                            continue;
                        }
                    }
                }
                addStatusLine(bHashOK ? "OK" : "MD5", gamename, filePathString, filesize, localHash);
            }
        }
        else
        {
            addStatusLine("ND", gamename, filePathString, 0, "");
        }
    }
    return;
}

void Downloader::addStatusLine(const std::string& statusCode, const std::string& gamename, const std::string& filepath, const uintmax_t& filesize, const std::string& localHash)
{
    std::cout << statusCode << " " << gamename << " " << filepath;

    if (filesize > 0)
        std::cout << " " << filesize;

    if (!localHash.empty())
        std::cout << " " << localHash;

    std::cout << std::endl;
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

std::string Downloader::getRemoteFileHash(const gameFile& gf)
{
    std::string remoteHash;

    // Refresh Galaxy login if token is expired
    if (gogGalaxy->isTokenExpired())
    {
        if (!gogGalaxy->refreshLogin())
        {
            std::cerr << "Galaxy API failed to refresh login" << std::endl;
            return remoteHash;
        }
    }

    // Get downlink JSON from Galaxy API
    Json::Value downlinkJson = gogGalaxy->getResponseJson(gf.galaxy_downlink_json_url);

    if (downlinkJson.empty())
    {
        std::cerr << "Empty JSON response" << std::endl;
        return remoteHash;
    }

    std::string xml_url;
    if (downlinkJson.isMember("checksum"))
        if (!downlinkJson["checksum"].empty())
            xml_url = downlinkJson["checksum"].asString();

    // Get XML data
    std::string xml;
    if (!xml_url.empty())
        xml = gogGalaxy->getResponse(xml_url);

    if (!xml.empty())
    {
        tinyxml2::XMLDocument remote_xml;
        remote_xml.Parse(xml.c_str());
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

    Json::Value root = Util::readJsonFile(cachepath);
    if (root.empty())
    {
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
        conf.dirConf = Globals::globalConfig.dirConf;
        if (Util::getGameSpecificConfig(game.gamename, &conf) > 0)
            std::cerr << game.gamename << " - Language: " << conf.dlConf.iInstallerLanguage << ", Platform: " << conf.dlConf.iInstallerPlatform << ", Include: " << Util::getOptionNameString(conf.dlConf.iInclude, GlobalConstants::INCLUDE_OPTIONS) << std::endl;

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
                        if (!fileDetailsNode["version"].empty())
                            fileDetails.version = fileDetailsNode["version"].asString();

                        if (nodeName != "extras" && !(fileDetails.platform & conf.dlConf.iInstallerPlatform))
                            continue;
                        if (nodeName != "extras" && !(fileDetails.language & conf.dlConf.iInstallerLanguage))
                            continue;
                    }

                    if (nodeName == "extras" &&
                            (conf.dlConf.iInclude & GlobalConstants::GFTYPE_EXTRA))
                    {
                        game.extras.push_back(fileDetails);
                    }
                    else if (nodeName == "installers" &&
                            (conf.dlConf.iInclude & GlobalConstants::GFTYPE_INSTALLER))
                    {
                        game.installers.push_back(fileDetails);
                    }
                    else if (nodeName == "patches" &&
                            (conf.dlConf.iInclude & GlobalConstants::GFTYPE_PATCH))
                    {
                        game.patches.push_back(fileDetails);
                    }
                    else if (nodeName == "languagepacks" &&
                            (conf.dlConf.iInclude & GlobalConstants::GFTYPE_LANGPACK))
                    {
                        game.languagepacks.push_back(fileDetails);
                    }
                    else if (nodeName == "dlcs" && (conf.dlConf.iInclude & GlobalConstants::GFTYPE_DLC))
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
                game.filterWithType(conf.dlConf.iInclude);
                details.push_back(game);
            }
    }
    return details;
}

void Downloader::updateCache()
{
    // Make sure that all details get cached
    Globals::globalConfig.dlConf.iInclude = Util::getOptionValue("all", GlobalConstants::INCLUDE_OPTIONS);
    Globals::globalConfig.sGameRegex = ".*";
    Globals::globalConfig.dlConf.iInstallerLanguage = Util::getOptionValue("all", GlobalConstants::LANGUAGES);
    Globals::globalConfig.dlConf.iInstallerPlatform = Util::getOptionValue("all", GlobalConstants::PLATFORMS);
    Globals::globalConfig.dlConf.vLanguagePriority.clear();
    Globals::globalConfig.dlConf.vPlatformPriority.clear();

    this->getGameList();
    this->getGameDetails();
    if (this->saveGameDetailsCache())
        std::cout << "Failed to save cache" << std::endl;

    return;
}

// Save JSON data to file
void Downloader::saveJsonFile(const std::string& json, const std::string& filepath)
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
        std::cout << "Saving JSON data: " << filepath << std::endl;
        ofs << json;
        ofs.close();
    }
    else
    {
        std::cout << "Failed to create file: " << filepath << std::endl;
    }

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

    // Check whether the changelog has changed
    if (boost::filesystem::exists(filepath))
    {
        std::ifstream ifs(filepath);
        if (ifs)
        {
            std::string existing_changelog((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();
            if (changelog == existing_changelog)
            {
                std::cout << "Changelog unchanged. Skipping: " << filepath << std::endl;
                return;
            }
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
    if (gogGalaxy->isTokenExpired())
    {
        if (!gogGalaxy->refreshLogin())
        {
            std::cerr << "Galaxy API failed to refresh login" << std::endl;
            return 1;
        }
    }

    DownloadConfig dlConf = Globals::globalConfig.dlConf;
    dlConf.iInclude = Util::getOptionValue("all", GlobalConstants::INCLUDE_OPTIONS);
    dlConf.bDuplicateHandler = false; // Disable duplicate handler

    int res = 1;
    CURLcode result = CURLE_RECV_ERROR; // assume network error

    size_t pos = fileid_string.find("/");
    if (pos == std::string::npos)
    {
        std::cerr << "Invalid file id " << fileid_string << ": could not find separator \"/\"" << std::endl;
    }
    else if (!output_filepath.empty() && boost::filesystem::is_directory(output_filepath))
    {
        std::cerr << "Failed to create the file " << output_filepath << ": Is a directory" << std::endl;
    }
    else
    {
        bool bIsDLC = false;
        std::string gamename, dlc_gamename, fileid, url;
        std::vector<std::string> fileid_vector = Util::tokenize(fileid_string, "/");
        if (fileid_vector.size() == 3)
            bIsDLC = true;

        gamename = fileid_vector[0];
        if (bIsDLC)
        {
            dlc_gamename = fileid_vector[1];
            fileid = fileid_vector[2];
        }
        else
            fileid = fileid_vector[1];

        std::string product_id;
        std::string gamename_select = "^" + gamename + "$";
        bool bSelectOK = this->galaxySelectProductIdHelper(gamename_select, product_id);

        if (!bSelectOK || product_id.empty())
        {
            std::cerr << "Failed to get numerical product id" << std::endl;
            return 1;
        }

        Json::Value productInfo = gogGalaxy->getProductInfo(product_id);
        if (productInfo.empty())
        {
            std::cerr << "Failed to get product info" << std::endl;
            return 1;
        }

        gameDetails gd = gogGalaxy->productInfoJsonToGameDetails(productInfo, dlConf);

        auto vFiles = gd.getGameFileVector();
        gameFile gf;
        bool bFoundMatchingFile = false;
        for (auto f : vFiles)
        {
            if (bIsDLC)
            {
                if (f.gamename != dlc_gamename)
                    continue;
            }

            if (f.id == fileid)
            {
                gf = f;
                bFoundMatchingFile = true;
                break;
            }
        }

        if (!bFoundMatchingFile)
        {
            std::string error_msg = "Failed to find file info (";
            error_msg += "product id: " + product_id;
            error_msg += (bIsDLC ? " / dlc gamename: " + dlc_gamename : "");
            error_msg += " / file id: " + fileid;
            error_msg += ")";

            std::cerr << error_msg << std::endl;
            return 1;
        }

        Json::Value downlinkJson = gogGalaxy->getResponseJson(gf.galaxy_downlink_json_url);

        if (downlinkJson.empty())
        {
            std::cerr << "Empty JSON response" << std::endl;
            return 1;
        }

        if (downlinkJson.isMember("downlink"))
        {
            url = downlinkJson["downlink"].asString();
        }
        else
        {
            std::cerr << "Invalid JSON response" << std::endl;
            return 1;
        }

        std::string xml_url;
        if (downlinkJson.isMember("checksum"))
        {
            if (!downlinkJson["checksum"].empty())
                xml_url = downlinkJson["checksum"].asString();
        }

        // Get XML data
        std::string xml_data;
        if (!xml_url.empty())
        {
            xml_data = gogGalaxy->getResponse(xml_url);
            if (xml_data.empty())
            {
                std::cerr << "Failed to get XML data" << std::endl;
            }
        }

        std::string filename, filepath;
        filename = gogGalaxy->getPathFromDownlinkUrl(url, gf.gamename);
        if (output_filepath.empty())
            filepath = Util::makeFilepath(Globals::globalConfig.dirConf.sDirectory, filename, gf.gamename);
        else
            filepath = output_filepath;
        std::cout << "Downloading: " << filepath << std::endl;
        result = this->downloadFile(url, filepath, xml_data, gf.gamename);
        std::cout << std::endl;
    }

    if (result == CURLE_OK)
        res = 0;

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

void Downloader::processCloudSaveUploadQueue(Config conf, const unsigned int& tid) {
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    std::unique_ptr<galaxyAPI> galaxy { new galaxyAPI(Globals::globalConfig.curlConf) };
    if (!galaxy->init())
    {
        if (!galaxy->refreshLogin())
        {
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* dlhandle = curl_easy_init();

    Util::CurlHandleSetDefaultOptions(dlhandle, conf.curlConf);
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Util::CurlReadChunkMemoryCallback);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    cloudSaveFile csf;

    std::string access_token;
    if (!Globals::galaxyConf.isExpired()) {
        access_token = Globals::galaxyConf.getAccessToken();
    }

    if (access_token.empty()) {
        return;
    }

    std::string bearer = "Authorization: Bearer " + access_token;

    while(dlCloudSaveQueue.try_pop(csf)) {
        CURLcode result = CURLE_RECV_ERROR; // assume network error
        int iRetryCount = 0;

        iTotalRemainingBytes.fetch_sub(csf.fileSize);

        vDownloadInfo[tid].setFilename(csf.path);

        std::string filecontents;
        {
            std::ifstream in { csf.location, std::ios_base::in | std::ios_base::binary };

            in >> filecontents;

            in.close();
        }

        ChunkMemoryStruct cms {
            &filecontents[0],
            (curl_off_t)filecontents.size()
        };

        auto md5 = Util::getChunkHash((std::uint8_t*)filecontents.data(), filecontents.size(), RHASH_MD5);

        auto url = "https://cloudstorage.gog.com/v1/" + Globals::galaxyConf.getUserId() + '/' + Globals::galaxyConf.getClientId() + '/' + csf.path;

        curl_slist *header = nullptr;
        header = curl_slist_append(header, bearer.c_str());
        header = curl_slist_append(header, ("X-Object-Meta-LocalLastModified: " + boost::posix_time::to_iso_extended_string(csf.lastModified)).c_str());
        header = curl_slist_append(header, ("Etag: " + md5).c_str());
        header = curl_slist_append(header, "Content-Type: Octet-Stream");
        header = curl_slist_append(header, ("Content-Length: " + std::to_string(filecontents.size())).c_str());

        curl_easy_setopt(dlhandle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(dlhandle, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(dlhandle, CURLOPT_HTTPHEADER, header);
        curl_easy_setopt(dlhandle, CURLOPT_READDATA, &cms);
        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());

        msgQueue.push(Message("Begin upload: " + csf.path, MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEFAULT));

        bool bShouldRetry = false;
        long int response_code = 0;
        std::string retry_reason;
        do
        {
            if (conf.iWait > 0)
                usleep(conf.iWait); // Wait before continuing

            response_code = 0; // Make sure that response code is reset

            if (iRetryCount != 0)
            {
                std::string retry_msg = "Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + boost::filesystem::path(csf.location).filename().string();
                if (!retry_reason.empty())
                    retry_msg += " (" + retry_reason + ")";
                msgQueue.push(Message(retry_msg, MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEFAULT));
            }
            retry_reason.clear(); // reset retry reason

            xferinfo.offset = 0;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();
            result = curl_easy_perform(dlhandle);

            switch (result)
            {
                // Retry on these errors
                case CURLE_PARTIAL_FILE:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_RECV_ERROR:
                case CURLE_SSL_CONNECT_ERROR:
                    bShouldRetry = true;
                    break;
                // Retry on CURLE_HTTP_RETURNED_ERROR if response code is not "416 Range Not Satisfiable"
                case CURLE_HTTP_RETURNED_ERROR:
                    curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                    if (response_code == 416 || response_code == 422 || response_code == 400 || response_code == 422) {
                        msgQueue.push(Message(std::to_string(response_code) + ": " + curl_easy_strerror(result), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                        bShouldRetry = false;
                    }
                    else
                        bShouldRetry = true;
                    break;
                default:
                    bShouldRetry = false;
                    break;
            }

            if (bShouldRetry) {
                iRetryCount++;
                retry_reason = std::to_string(response_code) + ": " + curl_easy_strerror(result);
            }
        } while (bShouldRetry && (iRetryCount <= conf.iRetries));

        curl_slist_free_all(header);
    }

    curl_easy_cleanup(dlhandle);

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    msgQueue.push(Message("Finished all tasks", MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEFAULT));
}

void Downloader::processCloudSaveDownloadQueue(Config conf, const unsigned int& tid) {
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    std::unique_ptr<galaxyAPI> galaxy { new galaxyAPI(Globals::globalConfig.curlConf) };
    if (!galaxy->init())
    {
        if (!galaxy->refreshLogin())
        {
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* dlhandle = curl_easy_init();

    Util::CurlHandleSetDefaultOptions(dlhandle, conf.curlConf);

    curl_slist *header = nullptr;

    std::string access_token;
    if (!Globals::galaxyConf.isExpired())
        access_token = Globals::galaxyConf.getAccessToken();
    if (!access_token.empty())
    {
        std::string bearer = "Authorization: Bearer " + access_token;
        header = curl_slist_append(header, bearer.c_str());
    }

    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    cloudSaveFile csf;
    while(dlCloudSaveQueue.try_pop(csf)) {
        CURLcode result = CURLE_RECV_ERROR; // assume network error
        int iRetryCount = 0;
        off_t iResumePosition = 0;

        bool bResume = false;

        iTotalRemainingBytes.fetch_sub(csf.fileSize);

        // Get directory from filepath
        boost::filesystem::path filepath = csf.location + ".~incomplete";
        filepath = boost::filesystem::absolute(filepath, boost::filesystem::current_path());
        boost::filesystem::path directory = filepath.parent_path();

        vDownloadInfo[tid].setFilename(csf.path);

        bResume = boost::filesystem::exists(filepath);

        msgQueue.push(Message("Begin download: " + csf.path, MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

        // Check that directory exists and create subdirectories
        std::unique_lock<std::mutex> ul { mtx_create_directories }; // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory)) {
                msgQueue.push(Message(directory.string() + " is not directory, skipping file (" + filepath.filename().string() + ")", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_ALWAYS));
                continue;
            }
        }
        else if (!boost::filesystem::create_directories(directory))
        {
            msgQueue.push(Message("Failed to create directory (" + directory.string() + "), skipping file (" + filepath.filename().string() + ")", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
            continue;
        }

        auto url = "https://cloudstorage.gog.com/v1/" + Globals::galaxyConf.getUserId() + '/' + Globals::galaxyConf.getClientId() + '/' + csf.path;
        curl_easy_setopt(dlhandle, CURLOPT_HTTPHEADER, header);
        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
        long int response_code = 0;
        bool bShouldRetry = false;
        std::string retry_reason;
        do
        {
            if (conf.iWait > 0)
                usleep(conf.iWait); // Wait before continuing

            response_code = 0; // Make sure that response code is reset

            if (iRetryCount != 0)
            {
                std::string retry_msg = "Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + filepath.filename().string();
                if (!retry_reason.empty())
                    retry_msg += " (" + retry_reason + ")";
                msgQueue.push(Message(retry_msg, MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
            }
            retry_reason = ""; // reset retry reason

            FILE* outfile;
            // If a file was partially downloaded
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
                    msgQueue.push(Message("Failed to open " + filepath.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                    msgQueue.push(Message("Failed to create " + filepath.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    break;
                }
            }

            xferinfo.offset = iResumePosition;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();
            result = curl_easy_perform(dlhandle);
            fclose(outfile);

            switch (result)
            {
                // Retry on these errors
                case CURLE_PARTIAL_FILE:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_RECV_ERROR:
                case CURLE_SSL_CONNECT_ERROR:
                    bShouldRetry = true;
                    break;
                // Retry on CURLE_HTTP_RETURNED_ERROR if response code is not "416 Range Not Satisfiable"
                case CURLE_HTTP_RETURNED_ERROR:
                    curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                    if (response_code == 416)
                        bShouldRetry = false;
                    else
                        bShouldRetry = true;
                    break;
                default:
                    bShouldRetry = false;
                    break;
            }

            if (bShouldRetry)
            {
                iRetryCount++;
                retry_reason = std::string(curl_easy_strerror(result));
                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath)) {
                    bResume = true;
                }
            }

        } while (bShouldRetry && (iRetryCount <= conf.iRetries));

        if (result == CURLE_OK || result == CURLE_RANGE_ERROR || (result == CURLE_HTTP_RETURNED_ERROR && response_code == 416))
        {
            // Set timestamp for downloaded file to same value as file on server
            // and rename "filename.~incomplete" to "filename"
            long filetime = -1;
            CURLcode res = curl_easy_getinfo(dlhandle, CURLINFO_FILETIME, &filetime);
            if (res == CURLE_OK && filetime >= 0)
            {
                std::time_t timestamp = (std::time_t)filetime;
                try
                {
                    boost::filesystem::rename(filepath, csf.location);
                    boost::filesystem::last_write_time(csf.location, timestamp);
                }
                catch(const boost::filesystem::filesystem_error& e)
                {
                    msgQueue.push(Message(e.what(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                }
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

            msgQueue.push(Message("Download complete: " + csf.path + " (@ " + dlrate_avg.str() + ")", MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_DEFAULT));
        }
        else
        {
            std::string msg = "Download complete (" + static_cast<std::string>(curl_easy_strerror(result));
            if (response_code > 0)
                msg += " (" + std::to_string(response_code) + ")";
            msg += "): " + filepath.filename().string();
            msgQueue.push(Message(msg, MSGTYPE_WARNING, msg_prefix, MSGLEVEL_DEFAULT));

            // Delete the file if download failed and was not a resume attempt or the result is zero length file
            if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
            {
                if ((result != CURLE_PARTIAL_FILE && !bResume && result != CURLE_OPERATION_TIMEDOUT) || boost::filesystem::file_size(filepath) == 0)
                {
                    if (!boost::filesystem::remove(filepath))
                        msgQueue.push(Message("Failed to delete " + filepath.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                }
            }
        }
    }

    curl_slist_free_all(header);
    curl_easy_cleanup(dlhandle);

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    msgQueue.push(Message("Finished all tasks", MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEFAULT));
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
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* curlheader = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(curlheader, conf.curlConf);
    curl_easy_setopt(curlheader, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curlheader, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
    curl_easy_setopt(curlheader, CURLOPT_HEADER, 1L);
    curl_easy_setopt(curlheader, CURLOPT_NOBODY, 1L);

    CURL* dlhandle = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(dlhandle, conf.curlConf);
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

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

        unsigned long long filesize = 0;
        try
        {
            filesize = std::stoll(gf.size);
        }
        catch (std::invalid_argument& e)
        {
            filesize = 0;
        }
        iTotalRemainingBytes.fetch_sub(filesize);

        // Get directory from filepath
        boost::filesystem::path filepath = gf.getFilepath();
        filepath = boost::filesystem::absolute(filepath, boost::filesystem::current_path());
        boost::filesystem::path directory = filepath.parent_path();

        // Skip blacklisted files
        if (conf.blacklist.isBlacklisted(filepath.string()))
        {
            msgQueue.push(Message("Blacklisted file: " + filepath.string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
            continue;
        }

        std::string filenameXML = filepath.filename().string() + ".xml";
        std::string xml_directory = conf.sXMLDirectory + "/" + gf.gamename;
        boost::filesystem::path local_xml_file = xml_directory + "/" + filenameXML;

        vDownloadInfo[tid].setFilename(filepath.filename().string());
        msgQueue.push(Message("Begin download: " + filepath.filename().string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

        // Check that directory exists and create subdirectories
        mtx_create_directories.lock(); // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory))
            {
                mtx_create_directories.unlock();
                msgQueue.push(Message(directory.string() + " is not directory, skipping file (" + filepath.filename().string() + ")", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_ALWAYS));
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
                msgQueue.push(Message("Failed to create directory (" + directory.string() + "), skipping file (" + filepath.filename().string() + ")", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                delete galaxy;
                return;
            }
        }

        // Get downlink JSON from Galaxy API
        Json::Value downlinkJson = galaxy->getResponseJson(gf.galaxy_downlink_json_url);

        if (downlinkJson.empty())
        {
            msgQueue.push(Message("Empty JSON response, skipping file", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
            continue;
        }

        if (!downlinkJson.isMember("downlink"))
        {
            msgQueue.push(Message("Invalid JSON response, skipping file", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
            continue;
        }

        std::string xml;
        if (gf.type & (GlobalConstants::GFTYPE_INSTALLER | GlobalConstants::GFTYPE_PATCH) && conf.dlConf.bRemoteXML)
        {
            std::string xml_url;
            if (downlinkJson.isMember("checksum"))
                if (!downlinkJson["checksum"].empty())
                    xml_url = downlinkJson["checksum"].asString();

            // Get XML data
            if (conf.dlConf.bRemoteXML && !xml_url.empty())
                xml = galaxy->getResponse(xml_url);

            if (!xml.empty() && !Globals::globalConfig.bSizeOnly)
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

        bool bIsComplete = false;
        bool bResume = false;
        if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
        {
            if (bSameVersion)
            {
                bResume = true;

                // Check if file is complete so we can skip it instead of resuming
                if (!xml.empty())
                {
                    off_t filesize_xml;
                    off_t filesize_local = boost::filesystem::file_size(filepath);

                    tinyxml2::XMLDocument remote_xml;
                    remote_xml.Parse(xml.c_str());
                    tinyxml2::XMLElement *fileElem = remote_xml.FirstChildElement("file");
                    if (fileElem)
                    {
                        std::string total_size = fileElem->Attribute("total_size");
                        try
                        {
                            filesize_xml = std::stoull(total_size);
                        }
                        catch (std::invalid_argument& e)
                        {
                            filesize_xml = 0;
                        }
                        if (filesize_local == filesize_xml)
                        {
                            msgQueue.push(Message("Skipping complete file: " + filepath.filename().string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                            bIsComplete = true; // Set to true so we can skip after saving xml data
                        }
                    }
                }
                // Special case for extras because they don't have remote XML data
                // and the API responses for extras can't be trusted
                else if (gf.type & GlobalConstants::GFTYPE_EXTRA)
                {
                    off_t filesize_local = boost::filesystem::file_size(filepath);
                    off_t filesize_api = 0;
                    try
                    {
                        filesize_api = std::stol(gf.size);
                    }
                    catch (std::invalid_argument& e)
                    {
                        filesize_api = 0;
                    }

                    // Check file size against file size reported by API
                    if(Globals::globalConfig.bTrustAPIForExtras)
                    {
                        if (filesize_local == filesize_api)
                        {
                            bIsComplete = true;
                        }
                    }
                    else
                    {
                        // API is not trusted to give correct details for extras
                        // Get size from content-length header and compare to it instead
                        off_t filesize_content_length = 0;
                        std::ostringstream memory;
                        std::string url = downlinkJson["downlink"].asString();

                        curl_easy_setopt(curlheader, CURLOPT_URL, url.c_str());
                        curl_easy_setopt(curlheader, CURLOPT_WRITEDATA, &memory);
                        curl_easy_perform(curlheader);
                        curl_easy_getinfo(curlheader, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &filesize_content_length);
                        memory.str(std::string());

                        if (filesize_local == filesize_content_length)
                        {
                            bIsComplete = true;
                        }

                        msgQueue.push(Message(filepath.filename().string() + ": filesize_local: " + std::to_string(filesize_local) + ", filesize_api: " + std::to_string(filesize_api) + ", filesize_content_length: " + std::to_string(filesize_content_length), MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEBUG));
                    }

                    if (bIsComplete)
                        msgQueue.push(Message("Skipping complete file: " + filepath.filename().string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                }
            }
            else
            {
                msgQueue.push(Message("Remote file is different, renaming local file", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                std::string date_old = "." + bptime::to_iso_string(bptime::second_clock::local_time()) + ".old";
                boost::filesystem::path new_name = filepath.string() + date_old; // Rename old file by appending date and ".old" to filename
                boost::system::error_code ec;
                boost::filesystem::rename(filepath, new_name, ec); // Rename the file
                if (ec)
                {
                    msgQueue.push(Message("Failed to rename " + filepath.string() + " to " + new_name.string() + " - Skipping file", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
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
                        msgQueue.push(Message(path.string() + " is not directory", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_ALWAYS));
                    }
                }
                else
                {
                    if (!boost::filesystem::create_directories(path))
                    {
                        msgQueue.push(Message("Failed to create directory: " + path.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                    msgQueue.push(Message("Can't create " + local_xml_file.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                }
            }
        }

        // File was complete and we have saved xml data so we can skip it
        if (bIsComplete)
            continue;

        std::string url = downlinkJson["downlink"].asString();
        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
        long int response_code = 0;
        bool bShouldRetry = false;
        std::string retry_reason;
        do
        {
            if (conf.iWait > 0)
                usleep(conf.iWait); // Wait before continuing

            response_code = 0; // Make sure that response code is reset

            if (iRetryCount != 0)
            {
                std::string retry_msg = "Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + filepath.filename().string();
                if (!retry_reason.empty())
                    retry_msg += " (" + retry_reason + ")";
                msgQueue.push(Message(retry_msg, MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
            }
            retry_reason = ""; // reset retry reason

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
                    msgQueue.push(Message("Failed to open " + filepath.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                    msgQueue.push(Message("Failed to create " + filepath.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    break;
                }
            }

            xferinfo.offset = iResumePosition;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();
            result = curl_easy_perform(dlhandle);
            fclose(outfile);

            switch (result)
            {
                // Retry on these errors
                case CURLE_PARTIAL_FILE:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_RECV_ERROR:
                case CURLE_SSL_CONNECT_ERROR:
                    bShouldRetry = true;
                    break;
                // Retry on CURLE_HTTP_RETURNED_ERROR if response code is not "416 Range Not Satisfiable"
                case CURLE_HTTP_RETURNED_ERROR:
                    curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                    if (response_code == 416)
                        bShouldRetry = false;
                    else
                        bShouldRetry = true;
                    break;
                default:
                    bShouldRetry = false;
                    break;
            }

            if (bShouldRetry)
            {
                iRetryCount++;
                retry_reason = std::string(curl_easy_strerror(result));
                if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
                    bResume = true;
            }

        } while (bShouldRetry && (iRetryCount <= conf.iRetries));

        if (result == CURLE_OK || result == CURLE_RANGE_ERROR || (result == CURLE_HTTP_RETURNED_ERROR && response_code == 416))
        {
            // Set timestamp for downloaded file to same value as file on server
            long filetime = -1;
            CURLcode res = curl_easy_getinfo(dlhandle, CURLINFO_FILETIME, &filetime);
            if (res == CURLE_OK && filetime >= 0)
            {
                std::time_t timestamp = (std::time_t)filetime;
                try
                {
                    boost::filesystem::last_write_time(filepath, timestamp);
                }
                catch(const boost::filesystem::filesystem_error& e)
                {
                    msgQueue.push(Message(e.what(), MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
                }
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

            msgQueue.push(Message("Download complete: " + filepath.filename().string() + " (@ " + dlrate_avg.str() + ")", MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_DEFAULT));
        }
        else
        {
            std::string msg = "Download complete (" + static_cast<std::string>(curl_easy_strerror(result));
            if (response_code > 0)
                msg += " (" + std::to_string(response_code) + ")";
            msg += "): " + filepath.filename().string();
            msgQueue.push(Message(msg, MSGTYPE_WARNING, msg_prefix, MSGLEVEL_DEFAULT));

            // Delete the file if download failed and was not a resume attempt or the result is zero length file
            if (boost::filesystem::exists(filepath) && boost::filesystem::is_regular_file(filepath))
            {
                if ((result != CURLE_PARTIAL_FILE && !bResume && result != CURLE_OPERATION_TIMEDOUT) || boost::filesystem::file_size(filepath) == 0)
                {
                    if (!boost::filesystem::remove(filepath))
                        msgQueue.push(Message("Failed to delete " + filepath.filename().string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                }
            }
        }

        // Automatic xml creation
        if (conf.dlConf.bAutomaticXMLCreation)
        {
            if (result == CURLE_OK)
            {
                if ((gf.type & GlobalConstants::GFTYPE_EXTRA) || (conf.dlConf.bRemoteXML && !bLocalXMLExists && xml.empty()))
                    createXMLQueue.push(gf);
            }
        }
    }

    curl_easy_cleanup(curlheader);
    curl_easy_cleanup(dlhandle);
    delete galaxy;

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    msgQueue.push(Message("Finished all tasks", MSGTYPE_INFO, msg_prefix, MSGLEVEL_DEFAULT));

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
        curl_off_t curl_rate;

        if (xferinfo->isChunk)
        {
            info.dlnow += xferinfo->chunk_file_offset;
            info.dltotal = xferinfo->chunk_file_total;
        }

        // trying to get rate and setting to NaN if it fails
        if (CURLE_OK != curl_easy_getinfo(xferinfo->curlhandle, CURLINFO_SPEED_DOWNLOAD_T, &curl_rate))
            info.rate_avg = std::numeric_limits<double>::quiet_NaN();
        else
            info.rate_avg = static_cast<double>(curl_rate);

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
            if (msg.getLevel() <= Globals::globalConfig.iMsgLevel)
                std::cout << msg.getFormattedString(Globals::globalConfig.bColor, true) << std::endl;

            if (Globals::globalConfig.bReport)
            {
                this->report_ofs << msg.getTimestampString() << ": " << msg.getMessage() << std::endl;
            }
        }

        int iTermWidth = Util::getTerminalWidth();
        double total_rate = 0;
        bptime::time_duration eta_total_seconds;

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
            eta_total_seconds += eta;
            std::string etastring = Util::makeEtaString(eta);

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

            std::string progress_status_text = Util::formattedString(" %0.2f/%0.2fMB @ %0.2f%s ETA: %s", static_cast<double>(progress_info.dlnow)/1024/1024, static_cast<double>(progress_info.dltotal)/1024/1024, progress_info.rate, rate_unit.c_str(), etastring.c_str());
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
            unsigned long long total_remaining = iTotalRemainingBytes.load();
            std::string total_eta_str;
            if (total_remaining > 0)
            {
                bptime::time_duration eta(bptime::seconds((long)(total_remaining / total_rate)));
                eta += eta_total_seconds;
                std::string eta_str = Util::makeEtaString(eta);

                double total_remaining_double = static_cast<double>(total_remaining)/1048576;
                std::string total_remaining_unit = "MB";
                std::vector<std::string> units = { "GB", "TB", "PB" };

                if (total_remaining_double > 1024)
                {
                    for (const auto& unit : units)
                    {
                        total_remaining_double /= 1024;
                        total_remaining_unit = unit;

                        if (total_remaining_double < 1024)
                            break;
                    }
                }

                total_eta_str = Util::formattedString(" (%0.2f%s) ETA: %s", total_remaining_double, total_remaining_unit.c_str(), eta_str.c_str());
            }

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

            if (!total_eta_str.empty())
                ss << total_eta_str;

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
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
        msgQueue.push(Message("Website not logged in", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                ss << game_item.name << " - " << iOptionsOverridden << " options overridden with game specific options";
                if (config.iMsgLevel >= MSGLEVEL_DEBUG)
                {
                    if (conf.dlConf.bIgnoreDLCCount)
                        ss << std::endl << "\tIgnore DLC count";
                    if (conf.dlConf.iInclude != config.dlConf.iInclude)
                        ss << std::endl << "\tInclude: " << Util::getOptionNameString(conf.dlConf.iInclude, GlobalConstants::INCLUDE_OPTIONS);
                    if (conf.dlConf.iInstallerLanguage != config.dlConf.iInstallerLanguage)
                        ss << std::endl << "\tLanguage: " << Util::getOptionNameString(conf.dlConf.iInstallerLanguage, GlobalConstants::LANGUAGES);
                    if (conf.dlConf.vLanguagePriority != config.dlConf.vLanguagePriority)
                    {
                        ss << std::endl << "\tLanguage priority:";
                        for (unsigned int j = 0; j < conf.dlConf.vLanguagePriority.size(); ++j)
                        {
                            ss << std::endl << "\t  " << j << ": " << Util::getOptionNameString(conf.dlConf.vLanguagePriority[j], GlobalConstants::LANGUAGES);
                        }
                    }
                    if (conf.dlConf.iInstallerPlatform != config.dlConf.iInstallerPlatform)
                        ss << std::endl << "\tPlatform: " << Util::getOptionNameString(conf.dlConf.iInstallerPlatform, GlobalConstants::PLATFORMS);
                    if (conf.dlConf.vPlatformPriority != config.dlConf.vPlatformPriority)
                    {
                        ss << std::endl << "\tPlatform priority:";
                        for (unsigned int j = 0; j < conf.dlConf.vPlatformPriority.size(); ++j)
                        {
                            ss << std::endl << "\t  " << j << ": " << Util::getOptionNameString(conf.dlConf.vPlatformPriority[j], GlobalConstants::PLATFORMS);
                        }
                    }
                }
                msgQueue.push(Message(ss.str(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
            }
        }

        // Refresh Galaxy login if token is expired
        if (galaxy->isTokenExpired())
        {
            if (!galaxy->refreshLogin())
            {
                msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                break;
            }
        }

        Json::Value product_info = galaxy->getProductInfo(game_item.id);
        game = galaxy->productInfoJsonToGameDetails(product_info, conf.dlConf);
        game.filterWithPriorities(conf);
        game.filterWithType(conf.dlConf.iInclude);

        if (conf.dlConf.bSaveProductJson && game.productJson.empty())
            game.productJson = product_info.toStyledString();

        if (conf.dlConf.bSaveProductJson && game.dlcs.size()) {
            for (unsigned int i = 0; i < game.dlcs.size(); ++i) {
                if (game.dlcs[i].productJson.empty()) {
                    Json::Value dlc_info = galaxy->getProductInfo(game.dlcs[i].product_id);
                    game.dlcs[i].productJson = dlc_info.toStyledString();
                }
            }
        }

        if ((conf.dlConf.bSaveSerials && game.serials.empty())
            || (conf.dlConf.bSaveChangelogs && game.changelog.empty())
            || (conf.dlConf.bSaveGameDetailsJson && game.gameDetailsJson.empty())
        )
        {
            Json::Value gameDetailsJSON;

            if (!game_item.gamedetailsjson.empty())
                gameDetailsJSON = game_item.gamedetailsjson;

            if (gameDetailsJSON.empty())
                gameDetailsJSON = website->getGameDetailsJSON(game_item.id);

            if (conf.dlConf.bSaveSerials && game.serials.empty())
                game.serials = Downloader::getSerialsFromJSON(gameDetailsJSON);

            if (conf.dlConf.bSaveChangelogs && game.changelog.empty())
                game.changelog = Downloader::getChangelogFromJSON(gameDetailsJSON);

            if (conf.dlConf.bSaveGameDetailsJson && game.gameDetailsJson.empty())
                game.gameDetailsJson = gameDetailsJSON.toStyledString();
        }

        game.makeFilepaths(conf.dirConf);
        gameDetailsQueue.push(game);
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

bool Downloader::galaxySelectProductIdHelper(const std::string& product_id, std::string& selected_product)
{
    selected_product = product_id;

    // Check to see if product_id is id or gamename
    boost::regex expression("^[0-9]+$");
    boost::match_results<std::string::const_iterator> what;
    if (!boost::regex_search(product_id, what, expression))
    {
        Globals::globalConfig.sGameRegex = product_id;
        this->getGameList();
        if (this->gameItems.empty())
        {
            std::cerr << "Didn't match any products" << std::endl;
            return false;
        }

        if (this->gameItems.size() == 1)
        {
            selected_product = this->gameItems[0].id;
        }
        else
        {
            std::cout << "Select product:" << std::endl;
            for (unsigned int i = 0; i < this->gameItems.size(); ++i)
                std::cout << i << ": " << this->gameItems[i].name << std::endl;

            if (!isatty(STDIN_FILENO)) {
                std::cerr << "Unable to read selection" << std::endl;
                return false;
            }

            int iSelect = -1;
            int iSelectMax = this->gameItems.size();
            while (iSelect < 0 || iSelect >= iSelectMax)
            {
                std::cerr << "> ";
                std::string selection;

                std::getline(std::cin, selection);
                try
                {
                    iSelect = std::stoi(selection);
                }
                catch(std::invalid_argument& e)
                {
                    std::cerr << e.what() << std::endl;
                }
            }
            selected_product = this->gameItems[iSelect].id;
        }
    }
    return true;
}

std::vector<galaxyDepotItem> Downloader::galaxyGetDepotItemVectorFromJson(const Json::Value& json, const unsigned int& iGalaxyArch)
{
    std::string product_id = json["baseProductId"].asString();

    std::string sLanguageRegex = "en|eng|english|en[_-]US";
    unsigned int iLanguage = Globals::globalConfig.dlConf.iGalaxyLanguage;
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        if (GlobalConstants::LANGUAGES[i].id == iLanguage)
        {
            sLanguageRegex = GlobalConstants::LANGUAGES[i].regexp;
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

    std::vector<galaxyDepotItem> items;
    for (unsigned int i = 0; i < json["depots"].size(); ++i)
    {
        std::vector<galaxyDepotItem> vec = gogGalaxy->getFilteredDepotItemsVectorFromJson(json["depots"][i], sLanguageRegex, sGalaxyArch);

        if (!vec.empty())
            items.insert(std::end(items), std::begin(vec), std::end(vec));
    }

    if (!(Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC))
    {
        std::vector<galaxyDepotItem> items_no_dlc;
        for (auto it : items)
        {
            if (it.product_id == product_id)
                items_no_dlc.push_back(it);
        }
        items = items_no_dlc;
    }

    // Add dependency ids to vector
    std::vector<std::string> dependencies;
    if (json.isMember("dependencies") && Globals::globalConfig.dlConf.bGalaxyDependencies)
    {
        for (unsigned int i = 0; i < json["dependencies"].size(); ++i)
        {
            dependencies.push_back(json["dependencies"][i].asString());
        }
    }

    // Add dependencies to items vector
    if (!dependencies.empty())
    {
        Json::Value dependenciesJson = gogGalaxy->getDependenciesJson();
        if (!dependenciesJson.empty() && dependenciesJson.isMember("depots"))
        {
            for (unsigned int i = 0; i < dependenciesJson["depots"].size(); ++i)
            {
                std::string dependencyId = dependenciesJson["depots"][i]["dependencyId"].asString();
                if (std::any_of(dependencies.begin(), dependencies.end(), [dependencyId](std::string dependency){return dependency == dependencyId;}))
                {
                    std::vector<galaxyDepotItem> vec = gogGalaxy->getFilteredDepotItemsVectorFromJson(dependenciesJson["depots"][i], sLanguageRegex, sGalaxyArch, true);

                    if (!vec.empty())
                        items.insert(std::end(items), std::begin(vec), std::end(vec));
                }
            }
        }
    }

    // Set product id for items
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        if (it->product_id.empty())
        {
            it->product_id = product_id;
        }
    }

    return items;
}

void Downloader::galaxyInstallGame(const std::string& product_id, int build_index, const unsigned int& iGalaxyArch)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->galaxyInstallGameById(id, build_index, iGalaxyArch);
    }
}

void Downloader::galaxyInstallGameById(const std::string& product_id, int build_index, const unsigned int& iGalaxyArch)
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

    Json::Value json = gogGalaxy->getProductBuilds(product_id, sPlatform);

    // JSON is empty and platform is Linux. Most likely cause is that Galaxy API doesn't have Linux support
    if (json.empty() && iPlatform == GlobalConstants::PLATFORM_LINUX)
    {
        std::cout << "Galaxy API doesn't have Linux support" << std::endl;

        // Galaxy install hack for Linux
        std::cout << "Trying to use installers as repository" << std::endl;
        this->galaxyInstallGame_MojoSetupHack(product_id);

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

    // Save builds json to another variable for later use
    Json::Value json_builds = json;

    json = gogGalaxy->getManifestV2(buildHash);
    std::string game_title = json["products"][0]["name"].asString();
    std::string install_directory;

    if (Globals::globalConfig.dirConf.bSubDirectories)
    {
        install_directory = this->getGalaxyInstallDirectory(gogGalaxy, json);
    }

    std::string install_path = Globals::globalConfig.dirConf.sDirectory + install_directory;

    std::vector<galaxyDepotItem> items = this->galaxyGetDepotItemVectorFromJson(json, iGalaxyArch);

    // Remove blacklisted files from items vector
    for (std::vector<galaxyDepotItem>::iterator it = items.begin(); it != items.end();)
    {
        std::string item_install_path = install_path + "/" + it->path;
        if (Globals::globalConfig.blacklist.isBlacklisted(item_install_path))
        {
            if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
                std::cout << "Skipping blacklisted file: " << item_install_path << std::endl;
            it = items.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Check for differences between previously installed build and new build
    std::vector<galaxyDepotItem> items_old;

    std::string info_path = install_path + "/goggame-" + product_id + ".info";
    std::string old_build_id;
    int old_build_index = -1;
    if (boost::filesystem::exists(info_path))
    {
        Json::Value info_json = Util::readJsonFile(info_path);
        if (!info_json.empty())
            old_build_id = info_json["buildId"].asString();

        if (!old_build_id.empty())
        {
            for (unsigned int i = 0; i < json_builds["items"].size(); ++i)
            {
                std::string build_id = json_builds["items"][i]["build_id"].asString();
                if (build_id == old_build_id)
                {
                    old_build_index = i;
                    break;
                }
            }
        }
    }

    // Check for deleted files between builds
    if (old_build_index >= 0 && old_build_index != build_index)
    {
        std::string link = json_builds["items"][old_build_index]["link"].asString();
        std::string buildHash_old;
        buildHash_old.assign(link.begin()+link.find_last_of("/")+1, link.end());

        Json::Value json_old = gogGalaxy->getManifestV2(buildHash_old);
        items_old = this->galaxyGetDepotItemVectorFromJson(json_old, iGalaxyArch);
    }

    std::vector<std::string> deleted_filepaths;
    if (!items_old.empty())
    {
        for (auto old_item: items_old)
        {
            bool isDeleted = true;
            for (auto item: items)
            {
                if (old_item.path == item.path)
                {
                    isDeleted = false;
                    break;
                }
            }
            if (isDeleted)
                deleted_filepaths.push_back(old_item.path);
        }
    }

    // Delete old files
    if (!deleted_filepaths.empty())
    {
        for (auto path : deleted_filepaths)
        {
            std::string filepath = install_path + "/" + path;
            std::cout << "Deleting " << filepath << std::endl;
            if (boost::filesystem::exists(filepath))
                if (!boost::filesystem::remove(filepath))
                    std::cerr << "Failed to delete " << filepath << std::endl;
        }
    }

    uintmax_t totalSize = 0;
    for (unsigned int i = 0; i < items.size(); ++i)
    {
        if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
        {
            std::cout << items[i].path << std::endl;
            std::cout << "\tChunks: " << items[i].chunks.size() << std::endl;
            std::cout << "\tmd5: " << items[i].md5 << std::endl;
        }
        totalSize += items[i].totalSizeUncompressed;
        iTotalRemainingBytes.fetch_add(items[i].totalSizeCompressed);
        dlQueueGalaxy.push(items[i]);
    }

    std::cout << game_title << std::endl;
    std::cout << "Files: " << items.size() << std::endl;
    std::cout << "Total size installed: " << Util::makeSizeString(totalSize) << std::endl;

    if (Globals::globalConfig.dlConf.bFreeSpaceCheck)
    {
        boost::filesystem::path path = boost::filesystem::absolute(install_path);
        while(!boost::filesystem::exists(path) && !path.empty())
        {
            path = path.parent_path();
        }

        if(boost::filesystem::exists(path) && !path.empty())
        {
            boost::filesystem::space_info space = boost::filesystem::space(path);

            if (space.available < totalSize)
            {
                std::cerr << "Not enough free space in " << boost::filesystem::canonical(path) << " ("
                << Util::makeSizeString(space.available) << ")"<< std::endl;
                exit(1);
            }
        }
    }

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
    {
        if (Globals::globalConfig.dlConf.bDeleteOrphans)
        {
            std::string filepath = orphans[i];
            std::cout << "Deleting " << filepath << std::endl;
            if (boost::filesystem::exists(filepath))
                if (!boost::filesystem::remove(filepath))
                    std::cerr << "Failed to delete " << filepath << std::endl;
        }
        else
            std::cout << "\t" << orphans[i] << std::endl;
    }
}

void Downloader::galaxyListCDNs(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->galaxyListCDNsById(id, build_index);
    }
}

void Downloader::galaxyListCDNsById(const std::string& product_id, int build_index)
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

    json = gogGalaxy->getSecureLink(product_id, "/");

    std::vector<std::string> vEndpointNames;
    if (!json.empty())
    {
        for (unsigned int i = 0; i < json["urls"].size(); ++i)
        {
            std::string endpoint_name = json["urls"][i]["endpoint_name"].asString();
            if (!endpoint_name.empty())
                vEndpointNames.push_back(endpoint_name);
        }
    }

    for (auto endpoint : vEndpointNames)
        std::cout << endpoint << std::endl;

    return;
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
            msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
            vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
            return;
        }
    }

    CURL* dlhandle = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(dlhandle, Globals::globalConfig.curlConf);
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    galaxyDepotItem item;
    std::string prev_product_id = "";
    std::vector<std::string> cdnUrlTemplates;
    while (dlQueueGalaxy.try_pop(item))
    {
        xferinfo.isChunk = false;
        xferinfo.chunk_file_offset = 0;
        xferinfo.chunk_file_total = item.totalSizeCompressed;

        if (item.product_id != prev_product_id)
            cdnUrlTemplates.clear();

        vDownloadInfo[tid].setStatus(DLSTATUS_STARTING);
        iTotalRemainingBytes.fetch_sub(item.totalSizeCompressed);

        boost::filesystem::path path = install_path + "/" + item.path;

        // Check that directory exists and create it
        boost::filesystem::path directory = path.parent_path();
        mtx_create_directories.lock(); // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory))
            {
                msgQueue.push(Message(directory.string() + " is not directory", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                msgQueue.push(Message("Failed to create directory: " + directory.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
            msgQueue.push(Message("File already exists: " + path.string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

            unsigned int resume_chunk = 0;
            uintmax_t filesize = boost::filesystem::file_size(path);
            if (filesize == item.totalSizeUncompressed)
            {
                // File is same size
                if (item.totalSizeUncompressed == 0 || Util::getFileHash(path.string(), RHASH_MD5) == item.md5)
                {
                    msgQueue.push(Message(path.string() + ": OK", MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_DEFAULT));
                    continue;
                }
                else
                {
                    msgQueue.push(Message(path.string() + ": MD5 mismatch", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_DEFAULT));
                    if (!boost::filesystem::remove(path))
                    {
                        msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                        continue;
                    }
                }
            }
            else if (filesize > item.totalSizeUncompressed)
            {
                // File is bigger than on server, delete old file and start from beginning
                msgQueue.push(Message(path.string() + ": File is bigger than expected. Deleting old file and starting from beginning", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                if (!boost::filesystem::remove(path))
                {
                    msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
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
                    msgQueue.push(Message(path.string() + ": Resume from chunk " + std::to_string(resume_chunk), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                    // Get chunk hash for previous chunk
                    FILE* f = fopen(path.string().c_str(), "r");
                    if (!f)
                    {
                        msgQueue.push(Message(path.string() + ": Failed to open", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
                        continue;
                    }

                    unsigned int previous_chunk = resume_chunk - 1;
                    uintmax_t chunk_size = item.chunks[previous_chunk].size_uncompressed;
                    // use fseeko to support large files on 32 bit platforms
                    fseeko(f, item.chunks[previous_chunk].offset_uncompressed, SEEK_SET);
                    unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
                    if (chunk == NULL)
                    {
                        msgQueue.push(Message(path.string() + ": Memory error - Chunk " + std::to_string(resume_chunk), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
                        fclose(f);
                        continue;
                    }

                    uintmax_t fread_size = fread(chunk, 1, chunk_size, f);
                    fclose(f);

                    if (fread_size != chunk_size)
                    {
                        msgQueue.push(Message(path.string() + ": Read error - Chunk " + std::to_string(resume_chunk), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
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
                        msgQueue.push(Message(path.string() + ": Chunk hash is different. Deleting old file and starting from beginning.", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
                        if (!boost::filesystem::remove(path))
                        {
                            msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
                            continue;
                        }
                    }
                }
                else
                {
                    msgQueue.push(Message(path.string() + ": Failed to find valid resume position. Deleting old file and starting from beginning.", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
                    if (!boost::filesystem::remove(path))
                    {
                        msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                        continue;
                    }
                }
            }
        }

        bool bChunkFailure = false;
        std::time_t timestamp = -1;
        // Handle empty files
        if (item.chunks.empty())
        {
            // Create empty file
            std::ofstream ofs(path.string(), std::ofstream::out | std::ofstream::binary);
            if (ofs)
                ofs.close();
        }
        for (unsigned int j = start_chunk; j < item.chunks.size(); ++j)
        {
            xferinfo.isChunk = true;
            xferinfo.chunk_file_offset = item.chunks[j].offset_compressed; // Set offset for progress info

            // Refresh Galaxy login if token is expired
            if (galaxy->isTokenExpired())
            {
                if (!galaxy->refreshLogin())
                {
                    msgQueue.push(Message("Galaxy API failed to refresh login", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                    delete galaxy;
                    return;
                }
            }

            std::string galaxyPath = galaxy->hashToGalaxyPath(item.chunks[j].md5_compressed);
            // Get url templates for cdns
            // Regular files can re-use these
            // Dependencies require new url everytime
            if (cdnUrlTemplates.empty() || item.isDependency)
            {
                Json::Value json;
                if (item.isDependency)
                    json = galaxy->getDependencyLink(galaxyPath);
                else
                    json = galaxy->getSecureLink(item.product_id, "/");

                if (json.empty())
                {
                    bChunkFailure = true;
                    std::string error_message = path.string() + ": Empty JSON response (product: " + item.product_id + ", chunk #"+ std::to_string(j) + ": " + item.chunks[j].md5_compressed + ")";
                    msgQueue.push(Message(error_message, MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                    break;
                }

                cdnUrlTemplates = galaxy->cdnUrlTemplatesFromJson(json, conf.dlConf.vGalaxyCDNPriority);
            }

            if (cdnUrlTemplates.empty())
            {
                bChunkFailure = true;
                msgQueue.push(Message(path.string() + ": Failed to get download url", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
                break;
            }

            std::string url = cdnUrlTemplates[0];
            if (item.isDependency)
            {
                while(Util::replaceString(url, "{LGOGDOWNLOADER_GALAXY_PATH}", ""));
                cdnUrlTemplates.clear(); // Clear templates
            }
            else
            {
                galaxyPath = "/" + galaxyPath;
                while(Util::replaceString(url, "{LGOGDOWNLOADER_GALAXY_PATH}", galaxyPath));
                prev_product_id = item.product_id;
            }

            ChunkMemoryStruct chunk;
            chunk.memory = (char *) malloc(1);
            chunk.size = 0;

            curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
            curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteChunkMemoryCallback);
            curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, &chunk);
            curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
            curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);
            curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);
            curl_easy_setopt(dlhandle, CURLOPT_RESUME_FROM_LARGE, 0);

            std::string filepath_and_chunk = path.string() + " (chunk " + std::to_string(j + 1) + "/" + std::to_string(item.chunks.size()) + ")";
            vDownloadInfo[tid].setFilename(filepath_and_chunk);

            CURLcode result;
            int iRetryCount = 0;
            long int response_code = 0;
            bool bShouldRetry = false;
            std::string retry_reason;
            do
            {
                if (Globals::globalConfig.iWait > 0)
                    usleep(Globals::globalConfig.iWait); // Delay the request by specified time

                response_code = 0; // Make sure that response code is reset

                if (iRetryCount != 0)
                {
                    std::string retry_msg = "Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + filepath_and_chunk;
                    if (!retry_reason.empty())
                        retry_msg += " (" + retry_reason + ")";
                    msgQueue.push(Message(retry_msg, MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

                    curl_easy_setopt(dlhandle, CURLOPT_RESUME_FROM_LARGE, chunk.size);
                }
                retry_reason = ""; // reset retry reason

                xferinfo.offset = chunk.size;
                xferinfo.timer.reset();
                xferinfo.TimeAndSize.clear();
                result = curl_easy_perform(dlhandle);

                switch (result)
                {
                    // Retry on these errors
                    case CURLE_PARTIAL_FILE:
                    case CURLE_OPERATION_TIMEDOUT:
                    case CURLE_RECV_ERROR:
                        bShouldRetry = true;
                        break;
                    // Retry on CURLE_HTTP_RETURNED_ERROR if response code is not "416 Range Not Satisfiable"
                    case CURLE_HTTP_RETURNED_ERROR:
                        curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                        if (response_code == 416)
                            bShouldRetry = false;
                        else
                            bShouldRetry = true;
                        break;
                    default:
                        bShouldRetry = false;
                        break;
                }

                if (bShouldRetry)
                {
                    iRetryCount++;
                    retry_reason = std::string(curl_easy_strerror(result));
                }

            } while (bShouldRetry && (iRetryCount <= conf.iRetries));

            curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
            curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
            curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 0L);

            if (result != CURLE_OK)
            {
                msgQueue.push(Message(std::string(curl_easy_strerror(result)), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
                if (result == CURLE_HTTP_RETURNED_ERROR)
                {
                    long int response_code = 0;
                    result = curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                    if (result == CURLE_OK)
                        msgQueue.push(Message("HTTP ERROR: " + std::to_string(response_code) + " (" + url + ")", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    else
                        msgQueue.push(Message("HTTP ERROR: failed to get error code: " + std::string(curl_easy_strerror(result)) + " (" + url + ")", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
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

        if (bChunkFailure)
        {
            msgQueue.push(Message(path.string() + ": Chunk failure, skipping file", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_VERBOSE));
            continue;
        }

        // Set timestamp for downloaded file to same value as file on server
        if (boost::filesystem::exists(path) && timestamp >= 0)
        {
            try
            {
                boost::filesystem::last_write_time(path, timestamp);
            }
            catch(const boost::filesystem::filesystem_error& e)
            {
                msgQueue.push(Message(e.what(), MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
            }
        }

        msgQueue.push(Message("Download complete: " + path.string(), MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_DEFAULT));
    }

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    delete galaxy;
    curl_easy_cleanup(dlhandle);

    return;
}

void Downloader::galaxyShowBuilds(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->galaxyShowBuildsById(id, build_index);
    }
}

void Downloader::galaxyShowBuildsById(const std::string& product_id, int build_index)
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

        std::cout << "Checking for installers that can be used as repository" << std::endl;
        DownloadConfig dlConf = Globals::globalConfig.dlConf;
        dlConf.iInclude = GlobalConstants::GFTYPE_INSTALLER;
        dlConf.iInstallerPlatform = dlConf.iGalaxyPlatform;
        dlConf.iInstallerLanguage = dlConf.iGalaxyLanguage;

        Json::Value product_info = gogGalaxy->getProductInfo(product_id);
        gameDetails game = gogGalaxy->productInfoJsonToGameDetails(product_info, dlConf);

        std::vector<gameFile> vInstallers;
        if (!game.installers.empty())
        {
            vInstallers.push_back(game.installers[0]);
            for (unsigned int i = 0; i < game.dlcs.size(); ++i)
            {
                if (!game.dlcs[i].installers.empty())
                    vInstallers.push_back(game.dlcs[i].installers[0]);
            }
        }

        if (vInstallers.empty())
        {
            std::cout << "No installers found" << std::endl;
        }
        else
        {
            std::cout << "Using these installers" << std::endl;
            for (unsigned int i = 0; i < vInstallers.size(); ++i)
                std::cout << "\t" << vInstallers[i].gamename << "/" << vInstallers[i].id << std::endl;
        }

        return;
    }

    if (build_index < 0)
    {
        for (unsigned int i = 0; i < json["items"].size(); ++i)
        {
            std::cout << i << ": " << "Version " << json["items"][i]["version_name"].asString()
            << " - " << json["items"][i]["date_published"].asString()
            << " (Gen " << json["items"][i]["generation"].asInt() << ")"
            << " (Build id: " << json["items"][i]["build_id"].asString() << ")" << std::endl;
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

    Json::StyledStreamWriter().write(std::cout, json);

    return;
}

std::string parseLocationHelper(const std::string &location, const std::map<std::string, std::string> &var) {
    char search_arg[2] {'?', '>'};
    auto it = std::search(std::begin(location), std::end(location), std::begin(search_arg), std::end(search_arg));

    if(it == std::end(location)) {
        return location;
    }

    std::string var_name { std::begin(location) + 2, it };
    auto relative_path = it + 2;

    auto var_value = var.find(var_name);
    if(var_value == std::end(var)) {
        return location;
    }

    std::string parsedLocation;
    parsedLocation.insert(std::end(parsedLocation), std::begin(var_value->second), std::end(var_value->second));
    parsedLocation.insert(std::end(parsedLocation), relative_path, std::end(location));

    return parsedLocation;
}
std::string parseLocation(const std::string &location, const std::map<std::string, std::string> &var) {
    auto parsedLocation = parseLocationHelper(location, var);
    Util::replaceAllString(parsedLocation, "\\", "/");

    return parsedLocation;
}

std::pair<std::string::const_iterator, std::string::const_iterator> getline(std::string::const_iterator begin, std::string::const_iterator end) {
    while(begin != end) {
        if(*begin == '\r') {
            return { begin, begin + 2 };
        }
        if(*begin == '\n') {
            return { begin, begin + 1 };
        }

        ++begin;
    }

    return { end, end };
}

void Downloader::uploadCloudSaves(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->uploadCloudSavesById(id, build_index);
    }
}

void Downloader::deleteCloudSaves(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->deleteCloudSavesById(id, build_index);
    }
}

void Downloader::downloadCloudSaves(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->downloadCloudSavesById(id, build_index);
    }
}

void Downloader::galaxyShowCloudSaves(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->galaxyShowCloudSavesById(id, build_index);
    }
}

void Downloader::galaxyShowLocalCloudSaves(const std::string& product_id, int build_index)
{
    std::string id;
    if(this->galaxySelectProductIdHelper(product_id, id))
    {
        if (!id.empty())
            this->galaxyShowLocalCloudSavesById(id, build_index);
    }
}

std::map<std::string, std::string> Downloader::cloudSaveLocations(const std::string& product_id, int build_index) {
    std::string sPlatform;
    unsigned int iPlatform = Globals::globalConfig.dlConf.iGalaxyPlatform;
    if (iPlatform == GlobalConstants::PLATFORM_LINUX) {
        // Linux is not yet supported for cloud saves
        std::cout << "Cloud saves for Linux builds not yet supported" << std::endl;
        return {};
    }
    else if (iPlatform == GlobalConstants::PLATFORM_MAC) {
        std::cout << "Cloud saves for Mac builds not yet supported" << std::endl;
        return {};
    }
    else
        sPlatform = "windows";

    Json::Value json = gogGalaxy->getProductBuilds(product_id, sPlatform);

    build_index = std::max(0, build_index);

    std::string link = json["items"][build_index]["link"].asString();

    Json::Value manifest;
    if (json["items"][build_index]["generation"].asInt() != 2)
    {
        std::cout << "Only generation 2 builds are supported currently" << std::endl;
        return {};
    }
    std::string buildHash;
    buildHash.assign(link.begin()+link.find_last_of("/")+1, link.end());
    manifest = gogGalaxy->getManifestV2(buildHash);

    std::string clientId = manifest["clientId"].asString();
    std::string secret = manifest["clientSecret"].asString();

    if(!gogGalaxy->refreshLogin(clientId, secret, Globals::galaxyConf.getRefreshToken(), false)) {
        std::cout << "Couldn't refresh login" << std::endl;
        return {};
    }

    std::string install_directory;
    if (Globals::globalConfig.dirConf.bSubDirectories)
    {
        install_directory = this->getGalaxyInstallDirectory(gogGalaxy, json);
    }

    std::string platform;
    switch(iPlatform) {
        case GlobalConstants::PLATFORM_WINDOWS:
            platform = "Windows";
            break;
        default:
            std::cout << "Only Windows supported for now for cloud support" << std::endl;
            return {};
    }

    std::string install_path = Globals::globalConfig.dirConf.sDirectory + install_directory;
    std::string document_path = Globals::globalConfig.dirConf.sWinePrefix + "drive_c/users/" + username() + "/Documents/";
    std::string appdata_roaming = Globals::globalConfig.dirConf.sWinePrefix + "drive_c/users/" + username() + "/AppData/Roaming/";
    std::string appdata_local_path = Globals::globalConfig.dirConf.sWinePrefix + "drive_c/users/" + username() + "/AppData/Local/";
    std::string appdata_local_low_path = Globals::globalConfig.dirConf.sWinePrefix + "drive_c/users/" + username() + "/AppData/LocalLow/";
    std::string saved_games = Globals::globalConfig.dirConf.sWinePrefix + "drive_c/users/" + username() + "/Save Games/";

    auto cloud_saves_json = gogGalaxy->getCloudPathAsJson(manifest["clientId"].asString())["content"][platform]["cloudStorage"];
    auto enabled = cloud_saves_json["enabled"].asBool();

    if(!enabled) {
        return {};
    }

    std::map<std::string, std::string> vars {
        { "INSTALL", std::move(install_path) },
        { "DOCUMENTS", std::move(document_path) },
        { "APPLICATION_DATA_ROAMING", std::move(appdata_roaming)},
        { "APPLICATION_DATA_LOCAL", std::move(appdata_local_path) },
        { "APPLICATION_DATA_LOCAL_LOW", std::move(appdata_local_low_path) },
        { "SAVED_GAMES", std::move(saved_games) },
    };

    std::map<std::string, std::string> name_to_location;
    for(auto &cloud_save : cloud_saves_json["locations"]) {
        std::string location = parseLocation(cloud_save["location"].asString(), vars);

        name_to_location.insert({cloud_save["name"].asString(), std::move(location)});
    }

    if(name_to_location.empty()) {
        std::string location;
        switch(iPlatform) {
            case GlobalConstants::PLATFORM_WINDOWS:
                location = vars["APPLICATION_DATA_LOCAL"] + "/GOG.com/Galaxy/Applications/" + Globals::galaxyConf.getClientId() + "/Storage";
                break;
            default:
                std::cout << "Only Windows supported for now for cloud support" << std::endl;
                return {};
        }

        name_to_location.insert({"__default", std::move(location)});
    }

    return name_to_location;
}

int Downloader::cloudSaveListByIdForEach(const std::string& product_id, int build_index, const std::function<void(cloudSaveFile &)> &f) {
    auto name_to_location = this->cloudSaveLocations(product_id, build_index);
    if(name_to_location.empty()) {
        std::cout << "No cloud save locations found" << std::endl;
        return -1;
    }

    std::string url = "https://cloudstorage.gog.com/v1/" + Globals::galaxyConf.getUserId() + "/" + Globals::galaxyConf.getClientId();
    auto fileList = gogGalaxy->getResponseJson(url, "application/json");

    for(auto &fileJson : fileList) {
        auto path = fileJson["name"].asString();

        if(!whitelisted(path)) {
            continue;
        }

        auto pos = path.find_first_of('/');

        auto location = name_to_location[path.substr(0, pos)] + path.substr(pos);

        auto filesize = fileJson["bytes"].asUInt64();

        auto last_modified = boost::posix_time::from_iso_extended_string(fileJson["last_modified"].asString());

        cloudSaveFile csf {
            last_modified,
            filesize,
            std::move(path),
            std::move(location)
        };

        f(csf);
    }

    return 0;
}

void Downloader::uploadCloudSavesById(const std::string& product_id, int build_index)
{
    auto name_to_locations = cloudSaveLocations(product_id, build_index);

    if(name_to_locations.empty()) {
        std::cout << "Cloud saves not supported for this game" << std::endl;
    }

    std::map<std::string, cloudSaveFile> path_to_cloudSaveFile;
    for(auto &name_to_location : name_to_locations) {
        auto &name = name_to_location.first;
        auto &location = name_to_location.second;

        if(!boost::filesystem::exists(location) || !boost::filesystem::is_directory(location)) {
            continue;
        }

        const char endswith[] = ".~incomplete";
        dirForEach(location, [&](boost::filesystem::directory_iterator file) {
            auto path = file->path();

            // If path ends with ".~incomplete", then skip this file
            if(
                path.size() >= sizeof(endswith) &&
                strcmp(path.c_str() + (path.size() + 1 - sizeof(endswith)), endswith) == 0
            ) {
                return;
            }

            auto remote_path = (name / boost::filesystem::relative(*file, location)).string();
            if(!whitelisted(remote_path)) {
                return;
            }


            cloudSaveFile csf {
                boost::posix_time::from_time_t(boost::filesystem::last_write_time(*file) - 1),
                boost::filesystem::file_size(*file),
                std::move(remote_path),
                file->path().string()
            };

            path_to_cloudSaveFile.insert(std::make_pair(csf.path, std::move(csf)));
        });
    }

    if(path_to_cloudSaveFile.empty()) {
        std::cout << "No local cloud saves found" << std::endl;

        return;
    }

    auto res = this->cloudSaveListByIdForEach(product_id, build_index, [&](cloudSaveFile &csf) {
        auto it = path_to_cloudSaveFile.find(csf.path);

        //If remote save is not locally stored, skip
        if(it == std::end(path_to_cloudSaveFile)) {
            return;
        }

        cloudSaveFile local_csf { std::move(it->second) };
        path_to_cloudSaveFile.erase(it);

        if(Globals::globalConfig.bCloudForce || csf.lastModified < local_csf.lastModified) {
            iTotalRemainingBytes.fetch_add(local_csf.fileSize);

            dlCloudSaveQueue.push(local_csf);
        }
    });

    for(auto &path_csf : path_to_cloudSaveFile) {
        auto &csf = path_csf.second;

        iTotalRemainingBytes.fetch_add(csf.fileSize);

        dlCloudSaveQueue.push(csf);
    }

    if(res || dlCloudSaveQueue.empty()) {
        return;
    }

    // Limit thread count to number of items in upload queue
    unsigned int iThreads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(dlCloudSaveQueue.size()));

    // Create download threads
    std::vector<std::thread> vThreads;
    for (unsigned int i = 0; i < iThreads; ++i)
    {
        DownloadInfo dlInfo;
        dlInfo.setStatus(DLSTATUS_NOTSTARTED);
        vDownloadInfo.push_back(dlInfo);
        vThreads.push_back(std::thread(Downloader::processCloudSaveUploadQueue, Globals::globalConfig, i));
    }

    this->printProgress(dlCloudSaveQueue);

    // Join threads
    for (unsigned int i = 0; i < vThreads.size(); ++i) {
        vThreads[i].join();
    }

    vThreads.clear();
    vDownloadInfo.clear();
}

void Downloader::deleteCloudSavesById(const std::string& product_id, int build_index) {
    if(Globals::globalConfig.cloudWhiteList.empty() && !Globals::globalConfig.bCloudForce) {
        std::cout << "No files have been whitelisted, either use \'--cloud-whitelist\' or \'--cloud-force\'" << std::endl;
        return;
    }


    curl_slist *header = nullptr;

    std::string access_token;
    if (!Globals::galaxyConf.isExpired()) {
        access_token = Globals::galaxyConf.getAccessToken();
    }

    if (!access_token.empty()) {
        std::string bearer = "Authorization: Bearer " + access_token;
        header = curl_slist_append(header, bearer.c_str());
    }

    auto dlhandle = curl_easy_init();

    curl_easy_setopt(dlhandle, CURLOPT_HTTPHEADER, header);
    curl_easy_setopt(dlhandle, CURLOPT_CUSTOMREQUEST, "DELETE");

    this->cloudSaveListByIdForEach(product_id, build_index, [dlhandle](cloudSaveFile &csf) {
        auto url = "https://cloudstorage.gog.com/v1/" + Globals::galaxyConf.getUserId() + '/' + Globals::galaxyConf.getClientId() + '/' + csf.path;
        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());

        auto result = curl_easy_perform(dlhandle);
        if(result == CURLE_HTTP_RETURNED_ERROR) {
            long response_code = 0;
            curl_easy_getinfo(dlhandle, CURLINFO_RESPONSE_CODE, &response_code);

            std::cout << response_code << ": " << curl_easy_strerror(result);
        }
    });

    curl_slist_free_all(header);

    curl_easy_cleanup(dlhandle);
}

void Downloader::downloadCloudSavesById(const std::string& product_id, int build_index) {
    auto res = this->cloudSaveListByIdForEach(product_id, build_index, [](cloudSaveFile &csf) {
        boost::filesystem::path filepath = csf.location;

        if(boost::filesystem::exists(filepath)) {
            // last_write_time minus a single second, since time_t is only accurate to the second unlike boost::posix_time::ptime
            auto time = boost::posix_time::from_time_t(boost::filesystem::last_write_time(filepath) - 1);

            if(!Globals::globalConfig.bCloudForce && time <= csf.lastModified) {
                std::cout << "Already up to date -- skipping: " << csf.path << std::endl;
                return; // This file is already completed
            }
        }

        if(boost::filesystem::is_directory(filepath)) {
            std::cout << "is a directory: " << csf.location << std::endl;
            return;
        }

        iTotalRemainingBytes.fetch_add(csf.fileSize);

        dlCloudSaveQueue.push(std::move(csf));
    });

    if(res || dlCloudSaveQueue.empty()) {
        return;
    }

    // Limit thread count to number of items in download queue
    unsigned int iThreads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(dlCloudSaveQueue.size()));

    // Create download threads
    std::vector<std::thread> vThreads;
    for (unsigned int i = 0; i < iThreads; ++i)
    {
        DownloadInfo dlInfo;
        dlInfo.setStatus(DLSTATUS_NOTSTARTED);
        vDownloadInfo.push_back(dlInfo);
        vThreads.push_back(std::thread(Downloader::processCloudSaveDownloadQueue, Globals::globalConfig, i));
    }

    this->printProgress(dlCloudSaveQueue);

    // Join threads
    for (unsigned int i = 0; i < vThreads.size(); ++i) {
        vThreads[i].join();
    }

    vThreads.clear();
    vDownloadInfo.clear();
}

void Downloader::galaxyShowCloudSavesById(const std::string& product_id, int build_index)
{
    this->cloudSaveListByIdForEach(product_id, build_index, [](cloudSaveFile &csf) {
        boost::filesystem::path filepath = csf.location;
        filepath = boost::filesystem::absolute(filepath, boost::filesystem::current_path());

        if(boost::filesystem::exists(filepath)) {
            auto size = boost::filesystem::file_size(filepath);

            // last_write_time minus a single second, since time_t is only accurate to the second unlike boost::posix_time::ptime
            auto time = boost::filesystem::last_write_time(filepath) - 1;

            if(csf.fileSize < size) {
                std::cout << csf.path << " :: not yet completed download" << std::endl;
            }
            else if(boost::posix_time::from_time_t(time) <= csf.lastModified) {
                std::cout << csf.path << " :: Already up to date" << std::endl;
            }
            else {
                std::cout << csf.path << " :: Out of date"  << std::endl;
            }
        }
        else {
            std::cout << csf.path << " :: Isn't downloaded yet"  << std::endl;
        }
    });
}

void Downloader::galaxyShowLocalCloudSavesById(const std::string& product_id, int build_index) {
    auto name_to_locations = cloudSaveLocations(product_id, build_index);

    if(name_to_locations.empty()) {
        std::cout << "Cloud saves not supported for this game" << std::endl;
    }

    std::map<std::string, cloudSaveFile> path_to_cloudSaveFile;
    for(auto &name_to_location : name_to_locations) {
        auto &name = name_to_location.first;
        auto &location = name_to_location.second;

        if(!boost::filesystem::exists(location) || !boost::filesystem::is_directory(location)) {
            continue;
        }

        dirForEach(location, [&](boost::filesystem::directory_iterator file) {
            auto path = (name / boost::filesystem::relative(*file, location)).string();

            if(!whitelisted(path)) {
                return;
            }

            cloudSaveFile csf {
                boost::posix_time::from_time_t(boost::filesystem::last_write_time(*file) - 1),
                boost::filesystem::file_size(*file),
                std::move(path),
                file->path().string()
            };

            path_to_cloudSaveFile.insert(std::make_pair(csf.path, std::move(csf)));
        });
    }

    if(path_to_cloudSaveFile.empty()) {
        std::cout << "No local cloud saves found" << std::endl;

        return;
    }

    this->cloudSaveListByIdForEach(product_id, build_index, [&](cloudSaveFile &csf) {
        auto it = path_to_cloudSaveFile.find(csf.path);

        //If remote save is not locally stored, skip
        if(it == std::end(path_to_cloudSaveFile)) {
            return;
        }

        cloudSaveFile local_csf { std::move(it->second) };
        path_to_cloudSaveFile.erase(it);

        std::cout << csf.path << ": ";
        if(csf.lastModified < local_csf.lastModified) {
            std::cout << "remote save out of date: it should be synchronized" << std::endl;
        }
        else {
            std::cout << "up to date" << std::endl;
        }
    });

    for(auto &path_csf : path_to_cloudSaveFile) {
        auto &csf = path_csf.second;

        std::cout << csf.path << ": there's only a local copy" << std::endl;
    }
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
                            if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
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

void Downloader::galaxyInstallGame_MojoSetupHack(const std::string& product_id)
{
    DownloadConfig dlConf = Globals::globalConfig.dlConf;
    dlConf.iInclude |= GlobalConstants::GFTYPE_BASE_INSTALLER;
    dlConf.iInstallerPlatform = dlConf.iGalaxyPlatform;
    dlConf.iInstallerLanguage = dlConf.iGalaxyLanguage;

    Json::Value product_info = gogGalaxy->getProductInfo(product_id);
    gameDetails game = gogGalaxy->productInfoJsonToGameDetails(product_info, dlConf);

    std::vector<gameFile> vInstallers;
    if (!game.installers.empty())
    {
        vInstallers.push_back(game.installers[0]);
        for (unsigned int i = 0; i < game.dlcs.size(); ++i)
        {
            if (!game.dlcs[i].installers.empty())
                vInstallers.push_back(game.dlcs[i].installers[0]);
        }
    }

    if (!vInstallers.empty())
    {
        std::vector<zipFileEntry> zipFileEntries;
        for (unsigned int i = 0; i < vInstallers.size(); ++i)
        {
            std::vector<zipFileEntry> vFiles;
            std::cout << "Getting file list for " << vInstallers[i].gamename << "/" << vInstallers[i].id << std::endl;
            if (this->mojoSetupGetFileVector(vInstallers[i], vFiles))
            {
                std::cerr << "Failed to get file list" << std::endl;
                return;
            }
            else
            {
                zipFileEntries.insert(std::end(zipFileEntries), std::begin(vFiles), std::end(vFiles));
            }
        }

        std::string install_directory;

        if (Globals::globalConfig.dirConf.bSubDirectories)
        {
            Json::Value windows_builds = gogGalaxy->getProductBuilds(product_id, "windows");
            if (!windows_builds.empty())
            {
                std::string link = windows_builds["items"][0]["link"].asString();
                std::string buildHash;
                buildHash.assign(link.begin()+link.find_last_of("/")+1, link.end());

                Json::Value manifest = gogGalaxy->getManifestV2(buildHash);
                if (!manifest.empty())
                {
                    install_directory = this->getGalaxyInstallDirectory(gogGalaxy, manifest);
                }
            }
        }

        std::string install_path = Globals::globalConfig.dirConf.sDirectory + "/" + install_directory + "/";
        std::vector<zipFileEntry> vZipDirectories;
        std::vector<zipFileEntry> vZipFiles;
        std::vector<zipFileEntry> vZipFilesSymlink;
        std::vector<zipFileEntry> vZipFilesSplit;

        // Determine if installer contains split files and get list of base file paths
        std::vector<std::string> vSplitFileBasePaths;
        for (const auto& zfe : zipFileEntries)
        {
            std::string noarch = "data/noarch/";
            std::string split_files = noarch + "support/split_files";
            if (zfe.filepath.find(split_files) != std::string::npos)
            {
                std::cout << "Getting info about split files" << std::endl;
                std::string url = zfe.installer_url;
                std::string dlrange = std::to_string(zfe.start_offset_mojosetup) + "-" + std::to_string(zfe.end_offset);
                curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());

                std::stringstream splitfiles_compressed;
                std::stringstream splitfiles_uncompressed;

                CURLcode result = CURLE_RECV_ERROR;
                curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
                curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &splitfiles_compressed);
                curl_easy_setopt(curlhandle, CURLOPT_RANGE, dlrange.c_str());
                result = curl_easy_perform(curlhandle);
                curl_easy_setopt(curlhandle, CURLOPT_RANGE, NULL);

                if (result == CURLE_OK)
                {
                    if (ZipUtil::extractStream(&splitfiles_compressed, &splitfiles_uncompressed) == 0)
                    {
                        std::string path;
                        while (std::getline(splitfiles_uncompressed, path))
                        {
                            // Replace the leading "./" in base file path with install path
                            Util::replaceString(path, "./", install_path);
                            while (Util::replaceString(path, "//", "/")); // Replace any double slashes with single slash
                            vSplitFileBasePaths.push_back(path);
                        }
                    }
                }
            }
        }

        bool bContainsSplitFiles = !vSplitFileBasePaths.empty();

        for (std::uintmax_t i = 0; i < zipFileEntries.size(); ++i)
        {
            // Ignore all files and directories that are not in "data/noarch/" directory
            std::string noarch = "data/noarch/";
            if (zipFileEntries[i].filepath.find(noarch) == std::string::npos || zipFileEntries[i].filepath == noarch)
                continue;

            zipFileEntry zfe = zipFileEntries[i];
            Util::replaceString(zfe.filepath, noarch, install_path);
            while (Util::replaceString(zfe.filepath, "//", "/")); // Replace any double slashes with single slash

            if (zfe.filepath.at(zfe.filepath.length()-1) == '/')
                vZipDirectories.push_back(zfe);
            else if (ZipUtil::isSymlink(zfe.file_attributes))
                vZipFilesSymlink.push_back(zfe);
            else
            {
                // Check for split files
                if (bContainsSplitFiles)
                {
                    boost::regex expression("^(.*)(\\.split\\d+)$");
                    boost::match_results<std::string::const_iterator> what;
                    if (boost::regex_search(zfe.filepath, what, expression))
                    {
                        std::string basePath = what[1];
                        std::string partExt = what[2];

                        // Check against list of base file paths read from "data/noarch/support/split_files"
                        if (
                            std::any_of(
                                vSplitFileBasePaths.begin(),
                                vSplitFileBasePaths.end(),
                                [basePath](const std::string& path)
                                {
                                    return path == basePath;
                                }
                            )
                        )
                        {
                            zfe.isSplitFile = true;
                            zfe.splitFileBasePath = basePath;
                            zfe.splitFilePartExt = partExt;
                        }
                    }

                    if (zfe.isSplitFile)
                        vZipFilesSplit.push_back(zfe);
                    else
                        vZipFiles.push_back(zfe);
                }
                else
                {
                     vZipFiles.push_back(zfe);
                }
            }
        }

        // Set start and end offsets for split files
        // Create map of split files for combining them later
        splitFilesMap mSplitFiles;
        if (!vZipFilesSplit.empty())
        {
            std::sort(vZipFilesSplit.begin(), vZipFilesSplit.end(), [](const zipFileEntry& i, const zipFileEntry& j) -> bool { return i.filepath < j.filepath; });

            std::string prevBasePath = "";
            off_t prevEndOffset = 0;
            for (auto& zfe : vZipFilesSplit)
            {
                if (zfe.splitFileBasePath == prevBasePath)
                    zfe.splitFileStartOffset = prevEndOffset;
                else
                    zfe.splitFileStartOffset = 0;

                zfe.splitFileEndOffset = zfe.splitFileStartOffset + zfe.uncomp_size;

                prevBasePath = zfe.splitFileBasePath;
                prevEndOffset = zfe.splitFileEndOffset;

                if (mSplitFiles.count(zfe.splitFileBasePath) > 0)
                {
                    mSplitFiles[zfe.splitFileBasePath].push_back(zfe);
                }
                else
                {
                    std::vector<zipFileEntry> vec;
                    vec.push_back(zfe);
                    mSplitFiles[zfe.splitFileBasePath] = vec;
                }
            }

            vZipFiles.insert(std::end(vZipFiles), std::begin(vZipFilesSplit), std::end(vZipFilesSplit));
        }

        // Add files to download queue
        uintmax_t totalSize = 0;
        for (std::uintmax_t i = 0; i < vZipFiles.size(); ++i)
        {
            // Don't add blacklisted files
            if (Globals::globalConfig.blacklist.isBlacklisted(vZipFiles[i].filepath))
            {
                if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
                    std::cout << "Skipping blacklisted file: " << vZipFiles[i].filepath << std::endl;

                continue;
            }
            dlQueueGalaxy_MojoSetupHack.push(vZipFiles[i]);
            iTotalRemainingBytes.fetch_add(vZipFiles[i].comp_size);
            totalSize += vZipFiles[i].uncomp_size;
        }

        // Add symlinks to download queue
        for (std::uintmax_t i = 0; i < vZipFilesSymlink.size(); ++i)
        {
            // Don't add blacklisted files
            if (Globals::globalConfig.blacklist.isBlacklisted(vZipFilesSymlink[i].filepath))
            {
                if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
                    std::cout << "Skipping blacklisted file: " << vZipFilesSymlink[i].filepath << std::endl;

                continue;
            }
            dlQueueGalaxy_MojoSetupHack.push(vZipFilesSymlink[i]);
            iTotalRemainingBytes.fetch_add(vZipFilesSymlink[i].comp_size);
            totalSize += vZipFilesSymlink[i].uncomp_size;
        }

        std::cout << game.title << std::endl;
        std::cout << "Files: " << dlQueueGalaxy_MojoSetupHack.size() << std::endl;
        std::cout << "Total size installed: " << Util::makeSizeString(totalSize) << std::endl;

        if (Globals::globalConfig.dlConf.bFreeSpaceCheck)
        {
            boost::filesystem::path path = boost::filesystem::absolute(install_path);
            while(!boost::filesystem::exists(path) && !path.empty())
            {
                path = path.parent_path();
            }

            if(boost::filesystem::exists(path) && !path.empty())
            {
                boost::filesystem::space_info space = boost::filesystem::space(path);

                if (space.available < totalSize)
                {
                    std::cerr << "Not enough free space in " << boost::filesystem::canonical(path) << " ("
                    << Util::makeSizeString(space.available) << ")"<< std::endl;
                    exit(1);
                }
            }
        }

        // Create directories
        for (std::uintmax_t i = 0; i < vZipDirectories.size(); ++i)
        {
            if (!boost::filesystem::exists(vZipDirectories[i].filepath))
            {
                if (!boost::filesystem::create_directories(vZipDirectories[i].filepath))
                {
                    std::cerr << "Failed to create directory " << vZipDirectories[i].filepath << std::endl;
                    return;
                }
            }
        }

        // Limit thread count to number of items in download queue
        unsigned int iThreads = std::min(Globals::globalConfig.iThreads, static_cast<unsigned int>(dlQueueGalaxy_MojoSetupHack.size()));

        // Create download threads
        std::vector<std::thread> vThreads;
        for (unsigned int i = 0; i < iThreads; ++i)
        {
            DownloadInfo dlInfo;
            dlInfo.setStatus(DLSTATUS_NOTSTARTED);
            vDownloadInfo.push_back(dlInfo);
            vThreads.push_back(std::thread(Downloader::processGalaxyDownloadQueue_MojoSetupHack, Globals::globalConfig, i));
        }

        this->printProgress(dlQueueGalaxy_MojoSetupHack);

        // Join threads
        for (unsigned int i = 0; i < vThreads.size(); ++i)
            vThreads[i].join();

        vThreads.clear();
        vDownloadInfo.clear();

        // Combine split files
        if (!mSplitFiles.empty())
        {
            this->galaxyInstallGame_MojoSetupHack_CombineSplitFiles(mSplitFiles, true);
        }
    }
    else
    {
        std::cout << "No installers found" << std::endl;
    }
}

void Downloader::galaxyInstallGame_MojoSetupHack_CombineSplitFiles(const splitFilesMap& mSplitFiles, const bool& bAppendToFirst)
{
    for (const auto& baseFile : mSplitFiles)
    {
        // Check that all parts exist
        bool bAllPartsExist = true;
        for (const auto& splitFile : baseFile.second)
        {
            if (!boost::filesystem::exists(splitFile.filepath))
            {
                bAllPartsExist = false;
                break;
            }
        }

        bool bBaseFileExists = boost::filesystem::exists(baseFile.first);

        if (!bAllPartsExist)
        {
            if (bBaseFileExists)
            {
                // Base file exist and we're missing parts.
                // This should mean that we already have complete file.
                // So we can safely skip this file without informing the user
                continue;
            }
            else
            {
                // Base file doesn't exist and we're missing parts. Print message about it before skipping file.
                std::cout << baseFile.first << " is missing parts. Skipping this file." << std::endl;
                continue;
            }
        }

        // Delete base file if it already exists
        if (bBaseFileExists)
        {
            std::cout << baseFile.first << " already exists. Deleting old file." << std::endl;
            if (!boost::filesystem::remove(baseFile.first))
            {
                std::cout << baseFile.first << ": Failed to delete" << std::endl;
                continue;
            }
        }

        std::cout << "Beginning to combine " << baseFile.first << std::endl;
        std::ofstream ofs;

        // Create base file for appending if we aren't appending to first part
        if (!bAppendToFirst)
        {
            ofs.open(baseFile.first, std::ios_base::binary | std::ios_base::app);
            if (!ofs.is_open())
            {
                std::cout << "Failed to create " << baseFile.first << std::endl;
                continue;
            }
        }

        for (const auto& splitFile : baseFile.second)
        {
            std::cout << "\t" << splitFile.filepath << std::endl;

            // Append to first file is set and current file is first in vector.
            // Open file for appending and continue to next file
            if (bAppendToFirst && (&splitFile == &baseFile.second.front()))
            {
                ofs.open(splitFile.filepath, std::ios_base::binary | std::ios_base::app);
                if (!ofs.is_open())
                {
                    std::cout << "Failed to open " << splitFile.filepath << std::endl;
                    break;
                }
                continue;
            }

            std::ifstream ifs(splitFile.filepath, std::ios_base::binary);
            if (!ifs)
            {
                std::cout << "Failed to open " << splitFile.filepath << ". Deleting incomplete file." << std::endl;

                ofs.close();
                if (!boost::filesystem::remove(baseFile.first))
                {
                    std::cout << baseFile.first << ": Failed to delete" << std::endl;
                }
                break;
            }

            ofs << ifs.rdbuf();
            ifs.close();

            // Delete split file
            if (!boost::filesystem::remove(splitFile.filepath))
            {
                std::cout << splitFile.filepath << ": Failed to delete" << std::endl;
            }
        }

        if (ofs)
            ofs.close();

        // Appending to first file so we must rename it
        if (bAppendToFirst)
        {
            boost::filesystem::path splitFilePath = baseFile.second.front().filepath;
            boost::filesystem::path baseFilePath = baseFile.first;

            boost::system::error_code ec;
            boost::filesystem::rename(splitFilePath, baseFilePath, ec);
            if (ec)
            {
                std::cout << "Failed to rename " << splitFilePath.string() << "to " << baseFilePath.string();
            }
        }
    }

    return;
}

void Downloader::processGalaxyDownloadQueue_MojoSetupHack(Config conf, const unsigned int& tid)
{
    std::string msg_prefix = "[Thread #" + std::to_string(tid) + "]";

    CURL* dlhandle = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(dlhandle, conf.curlConf);
    curl_easy_setopt(dlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(dlhandle, CURLOPT_FILETIME, 1L);

    xferInfo xferinfo;
    xferinfo.tid = tid;
    xferinfo.curlhandle = dlhandle;

    curl_easy_setopt(dlhandle, CURLOPT_XFERINFOFUNCTION, Downloader::progressCallbackForThread);
    curl_easy_setopt(dlhandle, CURLOPT_XFERINFODATA, &xferinfo);

    zipFileEntry zfe;
    while (dlQueueGalaxy_MojoSetupHack.try_pop(zfe))
    {
        vDownloadInfo[tid].setStatus(DLSTATUS_STARTING);
        iTotalRemainingBytes.fetch_sub(zfe.comp_size);

        boost::filesystem::path path = zfe.filepath;
        boost::filesystem::path path_tmp = zfe.filepath + ".lgogdltmp";

        // Check that directory exists and create it
        boost::filesystem::path directory = path.parent_path();
        mtx_create_directories.lock(); // Use mutex to avoid possible race conditions
        if (boost::filesystem::exists(directory))
        {
            if (!boost::filesystem::is_directory(directory))
            {
                msgQueue.push(Message(directory.string() + " is not directory", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                mtx_create_directories.unlock();
                return;
            }
        }
        else
        {
            if (!boost::filesystem::create_directories(directory))
            {
                msgQueue.push(Message("Failed to create directory: " + directory.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
                mtx_create_directories.unlock();
                return;
            }
        }
        mtx_create_directories.unlock();

        vDownloadInfo[tid].setFilename(path.string());

        if (ZipUtil::isSymlink(zfe.file_attributes))
        {
            if (boost::filesystem::is_symlink(path))
            {
                msgQueue.push(Message("Symlink already exists: " + path.string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                continue;
            }
        }
        else
        {
            if (zfe.isSplitFile)
            {
                if (boost::filesystem::exists(zfe.splitFileBasePath))
                {
                    msgQueue.push(Message(path.string() + ": Complete file (" + zfe.splitFileBasePath + ") of split file exists. Checking if it is same version.", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

                    std::string crc32 = Util::getFileHashRange(zfe.splitFileBasePath, RHASH_CRC32, zfe.splitFileStartOffset, zfe.splitFileEndOffset);
                    if (crc32 == Util::formattedString("%08x", zfe.crc32))
                    {
                        msgQueue.push(Message(path.string() + ": Complete file (" + zfe.splitFileBasePath + ") of split file is same version. Skipping file.", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                        continue;
                    }
                    else
                    {
                        msgQueue.push(Message(path.string() + ": Complete file (" + zfe.splitFileBasePath + ") of split file is different version. Continuing to download file.", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                    }
                }
            }

            if (boost::filesystem::exists(path))
            {
                msgQueue.push(Message("File already exists: " + path.string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));

                off_t filesize = static_cast<off_t>(boost::filesystem::file_size(path));
                if (filesize == zfe.uncomp_size)
                {
                    // File is same size
                    if (Util::getFileHash(path.string(), RHASH_CRC32) == Util::formattedString("%08x", zfe.crc32))
                    {
                        msgQueue.push(Message(path.string() + ": OK", MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_VERBOSE));
                        continue;
                    }
                    else
                    {
                        msgQueue.push(Message(path.string() + ": CRC32 mismatch. Deleting old file.", MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
                        if (!boost::filesystem::remove(path))
                        {
                            msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                            continue;
                        }
                    }
                }
                else
                {
                    // File size mismatch
                    msgQueue.push(Message(path.string() + ": File size mismatch. Deleting old file.", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                    if (!boost::filesystem::remove(path))
                    {
                        msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                        continue;
                    }
                }
            }
        }

        off_t resume_from = 0;
        if (boost::filesystem::exists(path_tmp))
        {
            off_t filesize = static_cast<off_t>(boost::filesystem::file_size(path_tmp));
            if (filesize < zfe.comp_size)
            {
                // Continue
                resume_from = filesize;
            }
            else
            {
                // Delete old file
                msgQueue.push(Message(path_tmp.string() + ": Deleting old file.", MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                if (!boost::filesystem::remove(path_tmp))
                {
                    msgQueue.push(Message(path_tmp.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    continue;
                }
            }
        }

        std::string url = zfe.installer_url;
        std::string dlrange = std::to_string(zfe.start_offset_mojosetup) + "-" + std::to_string(zfe.end_offset);
        curl_easy_setopt(dlhandle, CURLOPT_URL, url.c_str());
        if (ZipUtil::isSymlink(zfe.file_attributes))
        {
            // Symlink
            std::stringstream symlink_compressed;
            std::stringstream symlink_uncompressed;
            std::string link_target;

            CURLcode result = CURLE_RECV_ERROR;
            curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
            curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, &symlink_compressed);
            curl_easy_setopt(dlhandle, CURLOPT_RANGE, dlrange.c_str());

            vDownloadInfo[tid].setFilename(path.string());

            if (conf.iWait > 0)
                usleep(conf.iWait); // Delay the request by specified time

            xferinfo.offset = 0;
            xferinfo.timer.reset();
            xferinfo.TimeAndSize.clear();

            result = curl_easy_perform(dlhandle);

            if (result != CURLE_OK)
            {
                symlink_compressed.str(std::string());
                msgQueue.push(Message(path.string() + ": Failed to download", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                continue;
            }

            int res = ZipUtil::extractStream(&symlink_compressed, &symlink_uncompressed);
            symlink_compressed.str(std::string());

            if (res != 0)
            {
                std::string msg = "Extraction failed (";
                switch (res)
                {
                    case 1:
                        msg += "invalid input stream";
                        break;
                    case 2:
                        msg += "unsupported compression method";
                        break;
                    case 3:
                        msg += "invalid output stream";
                        break;
                    case 4:
                        msg += "zlib error";
                        break;
                    default:
                        msg += "unknown error";
                        break;
                }
                msg += ")";

                msgQueue.push(Message(msg + " " + path.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                symlink_uncompressed.str(std::string());
                continue;
            }

            link_target = symlink_uncompressed.str();
            symlink_uncompressed.str(std::string());

            if (!link_target.empty())
            {
                if (!boost::filesystem::exists(path))
                {
                    msgQueue.push(Message(path.string() + ": Creating symlink to " + link_target, MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));
                    boost::filesystem::create_symlink(link_target, path);
                }
            }
        }
        else
        {
            // Download file
            CURLcode result = CURLE_RECV_ERROR;

            off_t max_size_memory = 5 << 20; // 5MB
            if (zfe.comp_size < max_size_memory) // Handle small files in memory
            {
                std::ofstream ofs(path.string(), std::ofstream::out | std::ofstream::binary);
                if (!ofs)
                {
                    msgQueue.push(Message("Failed to create " + path_tmp.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    continue;
                }

                std::stringstream data_compressed;
                vDownloadInfo[tid].setFilename(path.string());
                curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
                curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, &data_compressed);
                curl_easy_setopt(dlhandle, CURLOPT_RANGE, dlrange.c_str());

                xferinfo.offset = 0;
                xferinfo.timer.reset();
                xferinfo.TimeAndSize.clear();

                result = curl_easy_perform(dlhandle);

                if (result != CURLE_OK)
                {
                    data_compressed.str(std::string());
                    ofs.close();
                    msgQueue.push(Message(path.string() + ": Failed to download", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    if (boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path))
                    {
                        if (!boost::filesystem::remove(path))
                        {
                            msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                        }
                    }
                    continue;
                }

                int res = ZipUtil::extractStream(&data_compressed, &ofs);
                data_compressed.str(std::string());
                ofs.close();

                if (res != 0)
                {
                    std::string msg = "Extraction failed (";
                    switch (res)
                    {
                        case 1:
                            msg += "invalid input stream";
                            break;
                        case 2:
                            msg += "unsupported compression method";
                            break;
                        case 3:
                            msg += "invalid output stream";
                            break;
                        case 4:
                            msg += "zlib error";
                            break;
                        default:
                            msg += "unknown error";
                            break;
                    }
                    msg += ")";

                    msgQueue.push(Message(msg + " " + path.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                    data_compressed.str(std::string());
                    if (boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path))
                    {
                        if (!boost::filesystem::remove(path))
                        {
                            msgQueue.push(Message(path.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                        }
                    }
                    continue;
                }

                if (boost::filesystem::exists(path))
                {
                    // Set file permission
                    boost::filesystem::perms permissions = ZipUtil::getBoostFilePermission(zfe.file_attributes);
                    Util::setFilePermissions(path, permissions);

                    // Set timestamp
                    if (zfe.timestamp > 0)
                    {
                        try
                        {
                            boost::filesystem::last_write_time(path, zfe.timestamp);
                        }
                        catch(const boost::filesystem::filesystem_error& e)
                        {
                            msgQueue.push(Message(e.what(), MSGTYPE_WARNING, msg_prefix, MSGLEVEL_VERBOSE));
                        }
                    }
                }
            }
            else // Use temporary file for bigger files
            {
                vDownloadInfo[tid].setFilename(path_tmp.string());
                curl_easy_setopt(dlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
                curl_easy_setopt(dlhandle, CURLOPT_READFUNCTION, Downloader::readData);

                int iRetryCount = 0;
                do
                {
                    if (iRetryCount != 0)
                        msgQueue.push(Message("Retry " + std::to_string(iRetryCount) + "/" + std::to_string(conf.iRetries) + ": " + path_tmp.filename().string(), MSGTYPE_INFO, msg_prefix, MSGLEVEL_VERBOSE));


                    FILE* outfile;
                    // File exists, resume
                    if (resume_from > 0)
                    {
                        if ((outfile=fopen(path_tmp.string().c_str(), "r+"))!=NULL)
                        {
                            fseek(outfile, 0, SEEK_END);
                            dlrange = std::to_string(zfe.start_offset_mojosetup + resume_from) + "-" + std::to_string(zfe.end_offset);
                            curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, outfile);
                            curl_easy_setopt(dlhandle, CURLOPT_RANGE, dlrange.c_str());
                        }
                        else
                        {
                            msgQueue.push(Message("Failed to open " + path_tmp.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                            break;
                        }
                    }
                    else // File doesn't exist, create new file
                    {
                        if ((outfile=fopen(path_tmp.string().c_str(), "w"))!=NULL)
                        {
                            curl_easy_setopt(dlhandle, CURLOPT_WRITEDATA, outfile);
                            curl_easy_setopt(dlhandle, CURLOPT_RANGE, dlrange.c_str());
                        }
                        else
                        {
                            msgQueue.push(Message("Failed to create " + path_tmp.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                            break;
                        }
                    }

                    if (conf.iWait > 0)
                        usleep(conf.iWait); // Delay the request by specified time

                    xferinfo.offset = 0;
                    xferinfo.timer.reset();
                    xferinfo.TimeAndSize.clear();
                    result = curl_easy_perform(dlhandle);
                    fclose(outfile);

                    if (result == CURLE_PARTIAL_FILE || result == CURLE_OPERATION_TIMEDOUT || result == CURLE_RECV_ERROR)
                    {
                        iRetryCount++;
                        if (boost::filesystem::exists(path_tmp) && boost::filesystem::is_regular_file(path_tmp))
                            resume_from = static_cast<off_t>(boost::filesystem::file_size(path_tmp));
                    }

                } while ((result == CURLE_PARTIAL_FILE || result == CURLE_OPERATION_TIMEDOUT || result == CURLE_RECV_ERROR) && (iRetryCount <= conf.iRetries));

                if (result == CURLE_OK)
                {
                    // Extract file
                    int res = ZipUtil::extractFile(path_tmp.string(), path.string());
                    bool bFailed = false;
                    if (res != 0)
                    {
                        bFailed = true;
                        std::string msg = "Extraction failed (";
                        unsigned int msg_type = MSGTYPE_ERROR;
                        switch (res)
                        {
                            case 1:
                                msg += "failed to open input file";
                                break;
                            case 2:
                                msg += "unsupported compression method";
                                break;
                            case 3:
                                msg += "failed to create output file";
                                break;
                            case 4:
                                msg += "zlib error";
                                break;
                            case 5:
                                msg += "failed to set timestamp";
                                msg_type = MSGTYPE_WARNING;
                                bFailed = false;
                                break;
                            default:
                                msg += "unknown error";
                                break;
                        }
                        msg += ")";

                        msgQueue.push(Message(msg + " " + path_tmp.string(), msg_type, msg_prefix, MSGLEVEL_ALWAYS));
                    }

                    if (bFailed)
                        continue;

                    if (boost::filesystem::exists(path_tmp) && boost::filesystem::is_regular_file(path_tmp))
                    {
                        if (!boost::filesystem::remove(path_tmp))
                        {
                            msgQueue.push(Message(path_tmp.string() + ": Failed to delete", MSGTYPE_ERROR, msg_prefix, MSGLEVEL_ALWAYS));
                        }
                    }

                    // Set file permission
                    boost::filesystem::perms permissions = ZipUtil::getBoostFilePermission(zfe.file_attributes);
                    if (boost::filesystem::exists(path))
                        Util::setFilePermissions(path, permissions);
                }
                else
                {
                    msgQueue.push(Message("Download failed " + path_tmp.string(), MSGTYPE_ERROR, msg_prefix, MSGLEVEL_DEFAULT));
                    continue;
                }
            }
        }

        msgQueue.push(Message("Download complete: " + path.string(), MSGTYPE_SUCCESS, msg_prefix, MSGLEVEL_DEFAULT));
    }

    vDownloadInfo[tid].setStatus(DLSTATUS_FINISHED);
    curl_easy_cleanup(dlhandle);

    return;
}

int Downloader::mojoSetupGetFileVector(const gameFile& gf, std::vector<zipFileEntry>& vFiles)
{
    Json::Value downlinkJson = gogGalaxy->getResponseJson(gf.galaxy_downlink_json_url);

    if (downlinkJson.empty())
    {
        std::cerr << "Empty JSON response" << std::endl;
        return 1;
    }

    if (!downlinkJson.isMember("downlink"))
    {
        std::cerr << "Invalid JSON response" << std::endl;
        return 1;
    }

    std::string xml_url;
    if (downlinkJson.isMember("checksum"))
    {
        if (!downlinkJson["checksum"].empty())
            xml_url = downlinkJson["checksum"].asString();
    }
    else
    {
        std::cerr << "Invalid JSON response. Response doesn't contain XML url." << std::endl;
        return 1;
    }


    // Get XML data
    curl_off_t file_size = 0;
    bool bMissingXML = false;
    bool bXMLParsingError = false;
    std::string xml_data = gogGalaxy->getResponse(xml_url);
    if (xml_data.empty())
    {
        std::cerr << "Failed to get XML data" << std::endl;
        bMissingXML = true;
    }

    if (!bMissingXML)
    {
        tinyxml2::XMLDocument xml;
        xml.Parse(xml_data.c_str());
        tinyxml2::XMLElement *fileElem = xml.FirstChildElement("file");

        if (!fileElem)
        {
            std::cerr << "Failed to parse XML data" << std::endl;
            bXMLParsingError = true;
        }
        else
        {
            std::string total_size = fileElem->Attribute("total_size");
            try
            {
                file_size = std::stoull(total_size);
            }
            catch (std::invalid_argument& e)
            {
                file_size = 0;
            }
        }
    }

    std::string installer_url = downlinkJson["downlink"].asString();
    if (installer_url.empty())
    {
        std::cerr << "Failed to get installer url" << std::endl;
        return 1;
    }

    if (bXMLParsingError || bMissingXML || file_size == 0)
    {
        std::cerr << "Failed to get file size from XML data, trying to get Content-Length header" << std::endl;
        std::stringstream header;
        curl_easy_setopt(curlhandle, CURLOPT_URL, installer_url.c_str());
        curl_easy_setopt(curlhandle, CURLOPT_HEADER, 1);
        curl_easy_setopt(curlhandle, CURLOPT_NOBODY, 1);
        curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &header);
        curl_easy_perform(curlhandle);
        curl_easy_setopt(curlhandle, CURLOPT_HEADER, 0);
        curl_easy_setopt(curlhandle, CURLOPT_NOBODY, 0);
        curl_easy_getinfo(curlhandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &file_size);
    }

    if (file_size <= 0)
    {
        std::cerr << "Failed to get file size" << std::endl;
        return 1;
    }

    off_t head_size = 100 << 10; // 100 kB
    off_t tail_size = 200 << 10; // 200 kB
    std::string head_range = "0-" + std::to_string(head_size);
    std::string tail_range = std::to_string(file_size - tail_size) + "-" + std::to_string(file_size);

    CURLcode result;

    // Get head
    std::stringstream head;
    curl_easy_setopt(curlhandle, CURLOPT_URL, installer_url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &head);
    curl_easy_setopt(curlhandle, CURLOPT_RANGE, head_range.c_str());
    result = curl_easy_perform(curlhandle);

    if (result != CURLE_OK)
    {
        std::cerr << "Failed to download data" << std::endl;
        return 1;
    }

    // Get zip start offset in MojoSetup installer
    off_t mojosetup_zip_offset = 0;
    off_t mojosetup_script_size = ZipUtil::getMojoSetupScriptSize(&head);
    head.seekg(0, head.beg);
    off_t mojosetup_installer_size = ZipUtil::getMojoSetupInstallerSize(&head);
    head.str(std::string());

    if (mojosetup_script_size == -1 || mojosetup_installer_size == -1)
    {
        std::cerr << "Failed to get Zip offset" << std::endl;
        return 1;
    }
    else
    {
        mojosetup_zip_offset = mojosetup_script_size + mojosetup_installer_size;
    }

    // Get tail
    std::stringstream tail;
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &tail);
    curl_easy_setopt(curlhandle, CURLOPT_RANGE, tail_range.c_str());
    result = curl_easy_perform(curlhandle);

    if (result != CURLE_OK)
    {
        std::cerr << "Failed to download data" << std::endl;
        return 1;
    }

    off_t offset_zip_eocd = ZipUtil::getZipEOCDOffset(&tail);
    off_t offset_zip64_eocd = ZipUtil::getZip64EOCDOffset(&tail);

    if (offset_zip_eocd < 0)
    {
        std::cerr << "Failed to find Zip EOCD offset" << std::endl;
        return 1;
    }

    zipEOCD eocd = ZipUtil::readZipEOCDStruct(&tail, offset_zip_eocd);

    uint64_t cd_offset = eocd.cd_start_offset;
    uint64_t cd_total = eocd.total_cd_records;

    if (offset_zip64_eocd >= 0)
    {
        zip64EOCD eocd64 = ZipUtil::readZip64EOCDStruct(&tail, offset_zip64_eocd);
        if (cd_offset == UINT32_MAX)
            cd_offset = eocd64.cd_offset;

        if (cd_total == UINT16_MAX)
            cd_total = eocd64.cd_total;
    }

    off_t cd_offset_in_stream = 0;
    off_t mojosetup_cd_offset = mojosetup_zip_offset + cd_offset;
    off_t cd_offset_from_file_end = file_size - mojosetup_cd_offset;

    if (cd_offset_from_file_end > tail_size)
    {
        tail.str(std::string());
        tail_range = std::to_string(mojosetup_cd_offset) + "-" + std::to_string(file_size);
        curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
        curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &tail);
        curl_easy_setopt(curlhandle, CURLOPT_RANGE, tail_range.c_str());
        result = curl_easy_perform(curlhandle);

        if (result != CURLE_OK)
        {
            std::cerr << "Failed to download data" << std::endl;
            return 1;
        }
    }
    else
    {
        cd_offset_in_stream = tail_size - cd_offset_from_file_end;
    }

    tail.seekg(cd_offset_in_stream, tail.beg);
    uint32_t signature = ZipUtil::readUInt32(&tail);
    if (signature != ZIP_CD_HEADER_SIGNATURE)
    {
        std::cerr << "Failed to find Zip Central Directory" << std::endl;
        return 1;
    }


    // Read file entries from Zip Central Directory
    tail.seekg(cd_offset_in_stream, tail.beg);
    for (std::uint64_t i = 0; i < cd_total; ++i)
    {
        zipCDEntry cd;
        cd = ZipUtil::readZipCDEntry(&tail);

        zipFileEntry zfe;
        zfe.filepath = cd.filename;
        zfe.comp_size = cd.comp_size;
        zfe.uncomp_size = cd.uncomp_size;
        zfe.start_offset_zip = cd.disk_offset;
        zfe.start_offset_mojosetup = zfe.start_offset_zip + mojosetup_zip_offset;
        zfe.file_attributes = cd.external_file_attr >> 16;
        zfe.crc32 = cd.crc32;
        zfe.timestamp = cd.timestamp;
        zfe.installer_url = installer_url;

        vFiles.push_back(zfe);
    }
    tail.str(std::string());

    // Set end offset for all entries
    vFiles[vFiles.size() - 1].end_offset = mojosetup_cd_offset - 1;
    for (std::uintmax_t i = 0; i < (vFiles.size() - 1); i++)
    {
        vFiles[i].end_offset = vFiles[i+1].start_offset_mojosetup - 1;
    }

    return 0;
}

std::string Downloader::getGalaxyInstallDirectory(galaxyAPI *galaxyHandle, const Json::Value& manifest)
{
    std::string install_directory = Globals::globalConfig.dirConf.sGalaxyInstallSubdir;
    std::string product_id = manifest["baseProductId"].asString();

    // Templates for installation subdir
    std::map<std::string, std::string> templates;
    templates["%install_dir%"] = manifest["installDirectory"].asString();
    templates["%product_id%"] = product_id;

    std::vector<std::string> templates_need_info =
    {
        "%gamename%",
        "%title%",
        "%title_stripped%"
    };

    if (std::any_of(templates_need_info.begin(), templates_need_info.end(), [install_directory](std::string template_dir){return template_dir == install_directory;}))
    {
        Json::Value productInfo = galaxyHandle->getProductInfo(product_id);
        std::string gamename = productInfo["slug"].asString();
        std::string title = productInfo["title"].asString();

        if (!gamename.empty())
            templates["%gamename%"] = productInfo["slug"].asString();
        if (!title.empty())
            templates["%title%"] = productInfo["title"].asString();
    }

    if (templates.count("%install_dir%"))
    {
        templates["%install_dir_stripped%"] = Util::getStrippedString(templates["%install_dir%"]);
    }

    if (templates.count("%title%"))
    {
        templates["%title_stripped%"] = Util::getStrippedString(templates["%title%"]);;
    }

    if (templates.count(install_directory))
    {
        install_directory = templates[install_directory];
    }

    return install_directory;
}

void Downloader::printGameDetailsAsText(gameDetails& game)
{
    std::cout   << "gamename: " << game.gamename << std::endl
                << "product id: " << game.product_id << std::endl
                << "title: " << game.title << std::endl
                << "icon: " << game.icon << std::endl;
    if (!game.serials.empty())
        std::cout << "serials:" << std::endl << game.serials << std::endl;

    // List installers
    if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_BASE_INSTALLER) && !game.installers.empty())
    {
        std::cout << "installers: " << std::endl;
        for (auto gf : game.installers)
        {
            this->printGameFileDetailsAsText(gf);
        }
    }
    // List extras
    if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_BASE_EXTRA) && !game.extras.empty())
    {
        std::cout << "extras: " << std::endl;
        for (auto gf : game.extras)
        {
            this->printGameFileDetailsAsText(gf);
        }
    }
    // List patches
    if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_BASE_PATCH) && !game.patches.empty())
    {
        std::cout << "patches: " << std::endl;
        for (auto gf : game.patches)
        {
            this->printGameFileDetailsAsText(gf);
        }
    }
    // List language packs
    if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_BASE_LANGPACK) && !game.languagepacks.empty())
    {
        std::cout << "language packs: " << std::endl;
        for (auto gf : game.languagepacks)
        {
            this->printGameFileDetailsAsText(gf);
        }
    }
    if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC) && !game.dlcs.empty())
    {
        std::cout << "DLCs: " << std::endl;
        for (auto dlc : game.dlcs)
        {
            std::cout   << "DLC gamename: " << dlc.gamename << std::endl
                        << "product id: " << dlc.product_id << std::endl;

            if (!dlc.serials.empty())
                std::cout << "serials:" << dlc.serials << std::endl;

            if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC_INSTALLER) && !dlc.installers.empty())
            {
                for (auto gf : dlc.installers)
                {
                    this->printGameFileDetailsAsText(gf);
                }
            }
            if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC_PATCH) && !dlc.patches.empty())
            {
                for (auto gf : dlc.patches)
                {
                    this->printGameFileDetailsAsText(gf);
                }
            }
            if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC_EXTRA) && !dlc.extras.empty())
            {
                for (auto gf : dlc.extras)
                {
                    this->printGameFileDetailsAsText(gf);
                }
            }
            if ((Globals::globalConfig.dlConf.iInclude & GlobalConstants::GFTYPE_DLC_LANGPACK) && !dlc.languagepacks.empty())
            {
                for (auto gf : dlc.languagepacks)
                {
                    this->printGameFileDetailsAsText(gf);
                }
            }
        }
    }
}

void Downloader::printGameFileDetailsAsText(gameFile& gf)
{
    std::string filepath = gf.getFilepath();
    if (Globals::globalConfig.blacklist.isBlacklisted(filepath))
    {
        if (Globals::globalConfig.iMsgLevel >= MSGLEVEL_VERBOSE)
            std::cerr << "skipped blacklisted file " << filepath << std::endl;
        return;
    }

    std::cout   << "\tid: " << gf.id << std::endl
                << "\tname: " << gf.name << std::endl
                << "\tpath: " << gf.path << std::endl
                << "\tsize: " << gf.size << std::endl;

    if (gf.type & GlobalConstants::GFTYPE_INSTALLER)
        std::cout << "\tupdated: " << (gf.updated ? "True" : "False") << std::endl;

    if (gf.type & (GlobalConstants::GFTYPE_INSTALLER | GlobalConstants::GFTYPE_PATCH))
    {
        std::string languages = Util::getOptionNameString(gf.language, GlobalConstants::LANGUAGES);
        std::cout << "\tlanguage: " << languages << std::endl;
    }

    if (gf.type & GlobalConstants::GFTYPE_INSTALLER)
        std::cout << "\tversion: " << gf.version << std::endl;

    std::cout << std::endl;
}
