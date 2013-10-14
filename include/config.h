/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef CONFIG_H__
#define CONFIG_H__

#include <iostream>
#include <curl/curl.h>

class Config
{
    public:
        Config() {};
        virtual ~Config() {};
        bool bVerbose;
        bool bNoRemoteXML;
        bool bNoCover;
        bool bUpdateCheck;
        bool bHelp;
        bool bDownload;
        bool bList;
        bool bListDetails;
        bool bLogin;
        bool bRepair;
        bool bNoInstallers;
        bool bNoExtras;
        bool bNoPatches;
        bool bNoLanguagePacks;
        bool bNoUnicode; // don't use Unicode in console output
        bool bNoColor;   // don't use colors
        bool bVerifyPeer;
        bool bCheckOrphans;
        std::string sGameRegex;
        std::string sDirectory;
        std::string sXMLFile;
        std::string sXMLDirectory;
        std::string sToken;
        std::string sSecret;
        std::string sVersionString;
        std::string sHome;
        std::string sCookiePath;
        std::string sConfigFilePath;
        unsigned int iInstallerType;
        unsigned int iInstallerLanguage;
        size_t iChunkSize;
        curl_off_t iDownloadRate;
        long int iTimeout;
};

#endif // CONFIG_H__
