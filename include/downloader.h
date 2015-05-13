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
#include "api.h"
#include "progressbar.h"
#include <curl/curl.h>
#include <json/json.h>
#include <ctime>
#include <fstream>
#include <deque>

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

class gameItem {
    public:
        std::string name;
        std::string id;
        std::vector<std::string> dlcnames;
};

class Downloader
{
    public:
        Downloader(Config &conf);
        virtual ~Downloader();
        int init();
        int login();
        void listGames();
        void updateCheck();
        void repair();
        void download();
        void checkOrphans();
        void checkStatus();
        void updateCache();
        void downloadFileWithId(const std::string& fileid_string);
        void showWishlist();
        CURL* curlhandle;
        Timer timer;
        Config config;
        ProgressBar* progressbar;
        std::deque< std::pair<time_t, size_t> > TimeAndSize;
    protected:
    private:
        CURLcode downloadFile(const std::string& url, const std::string& filepath, const std::string& xml_data = std::string(), const std::string& gamename = std::string());
        int repairFile(const std::string& url, const std::string& filepath, const std::string& xml_data = std::string(), const std::string& gamename = std::string());
        int downloadCovers(const std::string& gamename, const std::string& directory, const std::string& cover_xml_data);
        int getGameDetails();
        void getGameList();
        size_t getResumePosition();
        CURLcode beginDownload();
        std::string getResponse(const std::string& url);
        std::string getLocalFileHash(const std::string& filepath, const std::string& gamename = std::string());
        std::string getRemoteFileHash(const std::string& gamename, const std::string& id);
        int loadGameDetailsCache();
        int saveGameDetailsCache();
        std::vector<gameDetails> getGameDetailsFromJsonNode(Json::Value root, const int& recursion_level = 0);
        int HTTP_Login(const std::string& email, const std::string& password);
        std::vector<gameItem> getGames();
        std::vector<gameItem> getFreeGames();
        std::vector<gameFile> getExtrasFromJSON(const Json::Value& json, const std::string& gamename);
        Json::Value getGameDetailsJSON(const std::string& gameid);
        std::string getSerialsFromJSON(const Json::Value& json);
        void saveSerials(const std::string& serials, const std::string& filepath);

        static int progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);
        static size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp);
        static size_t writeData(void *ptr, size_t size, size_t nmemb, FILE *stream);
        static size_t readData(void *ptr, size_t size, size_t nmemb, FILE *stream);


        API *gogAPI;
        std::vector<gameItem> gameItems;
        std::vector<gameDetails> games;
        std::string coverXML;

        size_t resume_position;
        int retries;
        std::ofstream report_ofs;
};

#endif // DOWNLOADER_H
