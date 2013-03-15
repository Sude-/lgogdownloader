/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "config.h"
#include "api.h"
#include "progressbar.h"
#include <curl/curl.h>
#include <ctime>

#if __GNUC__
#   if __x86_64__ || __ppc64__ || __LP64__
#       define ENVIRONMENT64
#   else
#       define ENVIRONMENT32
#   endif
#endif

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

class Downloader
{
    public:
        Downloader(Config &conf);
        virtual ~Downloader();
        int init();
        void listGames();
        void updateCheck();
        void repair();
        void download();
        CURL* curlhandle;
        Timer timer;
        Config config;
        ProgressBar* progressbar;
    protected:
    private:
        CURLcode downloadFile(std::string url, std::string filepath);
        int repairFile(std::string url, std::string filepath, std::string xml_data = std::string(), std::string xml_dir = std::string());
        int downloadCovers(std::string gamename, std::string directory, std::string cover_xml_data);
        int login();
        int getGameDetails();
        void getGameList();
        size_t getResumePosition();
        CURLcode beginDownload();
        std::string getResponse(const std::string& url);

        int HTTP_Login(const std::string& email, const std::string& password);
        std::vector<std::string> getGames();
        std::vector<std::string> getFreeGames();

        static int progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);
        static size_t writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp);
        static size_t writeData(void *ptr, size_t size, size_t nmemb, FILE *stream);
        static size_t readData(void *ptr, size_t size, size_t nmemb, FILE *stream);


        API *gogAPI;
        std::vector<std::string> gameNames;
        std::vector<gameDetails> games;
        std::string coverXML;

        size_t resume_position;
};

#endif // DOWNLOADER_H
