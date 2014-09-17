/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef CONFIG_H__
#define CONFIG_H__

#include <iostream>
#include <curl/curl.h>

#include "blacklist.h"

class Config
{
    public:
        Config() {};
        virtual ~Config() {};
        bool bVerbose;
        bool bRemoteXML;
        bool bCover;
        bool bUpdateCheck;
        bool bDownload;
        bool bList;
        bool bListDetails;
        bool bLogin;
        bool bRepair;
        bool bInstallers;
        bool bExtras;
        bool bPatches;
        bool bLanguagePacks;
        bool bDLC;
        bool bUnicode; // use Unicode in console output
        bool bColor;   // use colors
        bool bVerifyPeer;
        bool bCheckStatus;
        bool bDuplicateHandler;
        bool bSaveConfig;
        bool bResetConfig;
        bool bReport;
        bool bSubDirectories;
        std::string sGameRegex;
        std::string sDirectory;
        std::string sXMLFile;
        std::string sXMLDirectory;
        std::string sToken;
        std::string sSecret;
        std::string sVersionString;
        std::string sConfigDirectory;
        std::string sCookiePath;
        std::string sConfigFilePath;
        std::string sBlacklistFilePath;
        std::string sOrphanRegex;
        std::string sCoverList;
        std::string sReportFilePath;
        unsigned int iInstallerType;
        unsigned int iInstallerLanguage;
        int iRetries;
        int iWait;
        size_t iChunkSize;
        curl_off_t iDownloadRate;
        long int iTimeout;
        Blacklist blacklist;
};

#endif // CONFIG_H__
