/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#if __GNUC__
#   if !(__x86_64__ || __ppc64__ || __LP64__)
#       ifndef _LARGEFILE_SOURCE
#           define _LARGEFILE_SOURCE
#       endif
#       ifndef _LARGEFILE64_SOURCE
#           define _LARGEFILE64_SOURCE
#       endif
#       if !defined(_FILE_OFFSET_BITS) || (_FILE_OFFSET_BITS == 32)
#           define _FILE_OFFSET_BITS 64
#       endif
#   endif
#endif

#include "config.h"
#include "progressbar.h"
#include "website.h"
#include "threadsafequeue.h"
#include "galaxyapi.h"
#include "globals.h"
#include "util.h"

#include <curl/curl.h>
#include <json/json.h>
#include <ctime>
#include <functional>
#include <fstream>
#include <deque>

class cloudSaveFile;
class Timer
{
    public:
        Timer() { this->reset(); };
        void reset() { gettimeofday(&(this->last_update), NULL); };
        double getTimeBetweenUpdates()
        { // Returns time elapsed between updates in milliseconds
            struct timeval time_now;
            gettimeofday(&time_now, NULL);
            double time_between = ( (time_now.tv_sec+(time_now.tv_usec/1000000.0))*1000.0 - (this->last_update.tv_sec+(this->last_update.tv_usec/1000000.0))*1000.0 );
            return time_between;
        };
        ~Timer() {};
    private:
        struct timeval last_update;
};

struct xferInfo
{
    unsigned int tid;
    CURL* curlhandle;
    Timer timer;
    std::deque< std::pair<time_t, uintmax_t> > TimeAndSize;
    curl_off_t offset;
};

typedef struct
{
    std::string filepath;
    off_t comp_size;
    off_t uncomp_size;
    off_t start_offset_zip;
    off_t start_offset_mojosetup;
    off_t end_offset;
    uint16_t file_attributes;
    uint32_t crc32;
    time_t timestamp;

    std::string installer_url;

    // For split file handling
    bool isSplitFile = false;
    std::string splitFileBasePath;
    std::string splitFilePartExt;
    off_t splitFileStartOffset;
    off_t splitFileEndOffset;
} zipFileEntry;

typedef std::map<std::string,std::vector<zipFileEntry>> splitFilesMap;

class Downloader
{
    public:
        Downloader();
        virtual ~Downloader();
        bool isLoggedIn();
        int init();
        int login();
        int listGames();
        void checkNotifications();
        void clearUpdateNotifications();
        void repair();
        void download();

        void downloadCloudSaves(const std::string& product_id, int build_index = -1);
        void downloadCloudSavesById(const std::string& product_id, int build_index = -1);
        void uploadCloudSaves(const std::string& product_id, int build_index = -1);
        void uploadCloudSavesById(const std::string& product_id, int build_index = -1);
        void deleteCloudSaves(const std::string& product_id, int build_index = -1);
        void deleteCloudSavesById(const std::string& product_id, int build_index = -1);

        void checkOrphans();
        void checkStatus();
        void updateCache();
        int downloadFileWithId(const std::string& fileid_string, const std::string& output_filepath);
        void showWishlist();
        CURL* curlhandle;
        Timer timer;
        ProgressBar* progressbar;
        std::deque< std::pair<time_t, uintmax_t> > TimeAndSize;
        void saveGalaxyJSON();

        void galaxyInstallGame(const std::string& product_id, int build_index = -1, const unsigned int& iGalaxyArch = GlobalConstants::ARCH_X64);
        void galaxyInstallGameById(const std::string& product_id, int build_index = -1, const unsigned int& iGalaxyArch = GlobalConstants::ARCH_X64);
        void galaxyShowBuilds(const std::string& product_id, int build_index = -1);
        void galaxyShowCloudSaves(const std::string& product_id, int build_index = -1);
        void galaxyShowLocalCloudSaves(const std::string& product_id, int build_index = -1);
        void galaxyShowLocalCloudSavesById(const std::string& product_id, int build_index = -1);
        void galaxyShowBuildsById(const std::string& product_id, int build_index = -1);
        void galaxyShowCloudSavesById(const std::string& product_id, int build_index = -1);
    protected:
    private:
        std::map<std::string, std::string> cloudSaveLocations(const std::string& product_id, int build_index);
        int cloudSaveListByIdForEach(const std::string& product_id, int build_index, const std::function<void(cloudSaveFile &)> &f);

        CURLcode downloadFile(const std::string& url, const std::string& filepath, const std::string& xml_data = std::string(), const std::string& gamename = std::string());
        int repairFile(const std::string& url, const std::string& filepath, const std::string& xml_data = std::string(), const std::string& gamename = std::string());
        int getGameDetails();
        void getGameList();
        uintmax_t getResumePosition();
        CURLcode beginDownload();
        std::string getResponse(const std::string& url);
        std::string getLocalFileHash(const std::string& filepath, const std::string& gamename = std::string());
        std::string getRemoteFileHash(const gameFile& gf);
        void addStatusLine(const std::string& statusCode, const std::string& gamename, const std::string& filepath, const uintmax_t& filesize, const std::string& localHash);
        int loadGameDetailsCache();
        int saveGameDetailsCache();
        std::vector<gameDetails> getGameDetailsFromJsonNode(Json::Value root, const int& recursion_level = 0);
        static std::string getSerialsFromJSON(const Json::Value& json);
        void saveSerials(const std::string& serials, const std::string& filepath);
        static std::string getChangelogFromJSON(const Json::Value& json);
        void saveGameDetailsJson(const std::string& json, const std::string& filepath);
        void saveChangelog(const std::string& changelog, const std::string& filepath);
        static void processDownloadQueue(Config conf, const unsigned int& tid);
        static void processCloudSaveDownloadQueue(Config conf, const unsigned int& tid);
        static void processCloudSaveUploadQueue(Config conf, const unsigned int& tid);
        static int progressCallbackForThread(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
        template <typename T> void printProgress(const ThreadSafeQueue<T>& download_queue);
        static void getGameDetailsThread(Config config, const unsigned int& tid);
        void printGameDetailsAsText(gameDetails& game);
        void printGameFileDetailsAsText(gameFile& gf);

        static int progressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
        static size_t writeData(void *ptr, size_t size, size_t nmemb, FILE *stream);
        static size_t readData(void *ptr, size_t size, size_t nmemb, FILE *stream);

        std::vector<std::string> galaxyGetOrphanedFiles(const std::vector<galaxyDepotItem>& items, const std::string& install_path);
        static void processGalaxyDownloadQueue(const std::string& install_path, Config conf, const unsigned int& tid);
        void galaxyInstallGame_MojoSetupHack(const std::string& product_id);
        void galaxyInstallGame_MojoSetupHack_CombineSplitFiles(const splitFilesMap& mSplitFiles, const bool& bAppendtoFirst = false);
        static void processGalaxyDownloadQueue_MojoSetupHack(Config conf, const unsigned int& tid);
        int mojoSetupGetFileVector(const gameFile& gf, std::vector<zipFileEntry>& vFiles);
        std::string getGalaxyInstallDirectory(galaxyAPI *galaxyHandle, const Json::Value& manifest);
        bool galaxySelectProductIdHelper(const std::string& product_id, std::string& selected_product);
        std::vector<galaxyDepotItem> galaxyGetDepotItemVectorFromJson(const Json::Value& json, const unsigned int& iGalaxyArch = GlobalConstants::ARCH_X64);

        Website *gogWebsite;
        galaxyAPI *gogGalaxy;
        std::vector<gameItem> gameItems;
        std::vector<gameDetails> games;

        off_t resume_position;
        int retries;
        std::ofstream report_ofs;
};

#endif // DOWNLOADER_H
