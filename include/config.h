/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef CONFIG_H__
#define CONFIG_H__

#include <iostream>
#include <curl/curl.h>
#include <json/json.h>
#include <mutex>
#include <ctime>

#include "blacklist.h"

struct DirectoryConfig
{
    bool bSubDirectories;
    std::string sDirectory;
    std::string sWinePrefix;
    std::string sGameSubdir;
    std::string sInstallersSubdir;
    std::string sExtrasSubdir;
    std::string sPatchesSubdir;
    std::string sLanguagePackSubdir;
    std::string sDLCSubdir;
    std::string sGalaxyInstallSubdir;
};

struct DownloadConfig
{
    unsigned int iInstallerPlatform;
    unsigned int iInstallerLanguage;
    unsigned int iGalaxyCDN;
    std::vector<unsigned int> vPlatformPriority;
    std::vector<unsigned int> vLanguagePriority;
    std::vector<unsigned int> vGalaxyCDNPriority;
    std::vector<std::string> vTags;
    unsigned int iInclude;
    unsigned int iGalaxyPlatform;
    unsigned int iGalaxyLanguage;
    unsigned int iGalaxyArch;

    bool bRemoteXML;
    bool bSaveChangelogs;
    bool bSaveSerials;
    bool bSaveGameDetailsJson;
    bool bSaveProductJson;
    bool bSaveLogo;
    bool bSaveIcon;
    bool bAutomaticXMLCreation;
    bool bFreeSpaceCheck;

    bool bIgnoreDLCCount;
    bool bDuplicateHandler;
    bool bGalaxyDependencies;
    bool bDeleteOrphans;
};

struct gameSpecificConfig
{
    DownloadConfig dlConf;
    DirectoryConfig dirConf;
};

class GalaxyConfig
{
    public:
        bool isExpired()
        {
            std::unique_lock<std::mutex> lock(m);
            bool bExpired = true; // assume that token is expired
            intmax_t time_now = time(NULL);
            if (this->token_json.isMember("expires_at"))
                bExpired = (time_now > this->token_json["expires_at"].asLargestInt());
            return bExpired;
        }

        std::string getAccessToken()
        {
            std:: string access_token;
            std::unique_lock<std::mutex> lock(m);
            if (this->token_json.isMember("access_token"))
                access_token = this->token_json["access_token"].asString();
            return access_token;
        }

        std::string getRefreshToken()
        {
            std::string refresh_token;
            std::unique_lock<std::mutex> lock(m);
            if (this->token_json.isMember("refresh_token"))
                refresh_token = this->token_json["refresh_token"].asString();
            return refresh_token;
        }

        Json::Value getJSON()
        {
            std::unique_lock<std::mutex> lock(m);
            return this->token_json;
        }

        std::string getUserId() {
            std::unique_lock<std::mutex> lock(m);

            if(this->token_json.isMember("user_id")) {
                return this->token_json["user_id"].asString();
            }

            return {};
        }

        void setJSON(Json::Value json)
        {
            std::unique_lock<std::mutex> lock(m);
            if (!json.isMember("expires_at"))
            {
                intmax_t time_now = time(NULL);
                Json::Value::LargestInt expires_in = 3600;
                if (json.isMember("expires_in"))
                    if (!json["expires_in"].isNull())
                        expires_in = json["expires_in"].asLargestInt();

                Json::Value::LargestInt expires_at = time_now + expires_in;
                json["expires_at"] = expires_at;
            }
            this->token_json = json;
        }

        void setFilepath(const std::string& path)
        {
            std::unique_lock<std::mutex> lock(m);
            this->filepath = path;
        }

        std::string getFilepath()
        {
            std::unique_lock<std::mutex> lock(m);
            return this->filepath;
        }

        void resetClient() {
            std::lock_guard<std::mutex> lock(m);
            if(token_json.isMember("client_id")) {
                token_json["client_id"] = default_client_id;
            }
            if(token_json.isMember("client_secret")) {
                token_json["client_secret"] = default_client_secret;
            }
        }

        std::string getClientId()
        {
            std::lock_guard<std::mutex> lock(m);
            if(token_json.isMember("client_id")) {
                return token_json["client_id"].asString();
            }

            return default_client_id;
        }

        std::string getClientSecret()
        {
            std::lock_guard<std::mutex> lock(m);
            if(token_json.isMember("client_secret")) {
                return token_json["client_secret"].asString();
            }

            return default_client_secret;
        }

        std::string getRedirectUri()
        {
            std::unique_lock<std::mutex> lock(m);
            return this->redirect_uri;
        }

        GalaxyConfig() = default;

        GalaxyConfig(const GalaxyConfig& other)
        {
            std::lock_guard<std::mutex> guard(other.m);
            redirect_uri = other.redirect_uri;
            filepath = other.filepath;
            token_json = other.token_json;
        }

        GalaxyConfig& operator= (GalaxyConfig& other)
        {
            if(&other == this)
                return *this;

            std::unique_lock<std::mutex> lock1(m, std::defer_lock);
            std::unique_lock<std::mutex> lock2(other.m, std::defer_lock);
            std::lock(lock1, lock2);
            redirect_uri = other.redirect_uri;
            filepath = other.filepath;
            token_json = other.token_json;
            return *this;
        }
    protected:
    private:
        const std::string default_client_id = "46899977096215655";
        const std::string default_client_secret = "9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9";

        std::string redirect_uri = "https://embed.gog.com/on_login_success?origin=client";
        std::string filepath;
        Json::Value token_json;
        mutable std::mutex m;
};

struct CurlConfig
{
    bool bVerifyPeer;
    bool bVerbose;
    std::string sCACertPath;
    std::string sCookiePath;
    std::string sUserAgent;
    long int iTimeout;
    curl_off_t iDownloadRate;
    long int iLowSpeedTimeout;
    long int iLowSpeedTimeoutRate;
};

class Config
{
    public:
        Config() {};
        virtual ~Config() {};

        // Booleans
        bool bLogin;
        bool bSaveConfig;
        bool bResetConfig;

        bool bDownload;
        bool bRepair;
        bool bUpdated;
        bool bNew;
        bool bCheckStatus;
        bool bShowWishlist;
        bool bNotifications;
        bool bIncludeHiddenProducts;
        bool bSizeOnly;

        bool bUnicode; // use Unicode in console output
        bool bColor;   // use colors
        bool bReport;
        bool bRespectUmask;
        bool bPlatformDetection;
#ifdef USE_QT_GUI_LOGIN
        bool bEnableLoginGUI;
        bool bForceGUILogin;
#endif
        bool bUseFastCheck;
        bool bTrustAPIForExtras;

        // Cache
        bool bUseCache;
        bool bUpdateCache;
        int iCacheValid;

        // Download with file id options
        std::string sFileIdString;
        std::string sOutputFilename;

        // Curl
        CurlConfig curlConf;

        // Download
        DownloadConfig dlConf;

        // Directories
        DirectoryConfig dirConf;
        std::string sCacheDirectory;
        std::string sXMLDirectory;
        std::string sConfigDirectory;

        // File paths
        std::string sConfigFilePath;
        std::string sBlacklistFilePath;
        std::string sIgnorelistFilePath;
        std::string sGameHasDLCListFilePath;
        std::string sReportFilePath;
        std::string sTransformConfigFilePath;

        std::string sXMLFile;

        // Regex
        std::string sGameRegex;
        std::string sOrphanRegex;
        std::string sIgnoreDLCCountRegex;

        // Priorities
        std::string sPlatformPriority;
        std::string sLanguagePriority;

        // General strings
        std::string sVersionString;
        std::string sVersionNumber;
        std::string sEmail;
        std::string sPassword;

        // Lists
        Blacklist blacklist;
        Blacklist ignorelist;
        Blacklist gamehasdlc;

        // Cloud save options
        std::vector<std::string> cloudWhiteList;
        std::vector<std::string> cloudBlackList;
        bool bCloudForce;

        std::string sGameHasDLCList;

        // Integers
        int iRetries;
        unsigned int iThreads;
        unsigned int iInfoThreads;
        int iWait;
        size_t iChunkSize;
        int iProgressInterval;
        int iMsgLevel;
        unsigned int iListFormat;

        Json::Value transformationsJSON;
};

#endif // CONFIG_H__
