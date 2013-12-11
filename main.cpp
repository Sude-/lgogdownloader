/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "config.h"
#include "util.h"
#include "globalconstants.h"

#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#if __GNUC__
#   if __x86_64__ || __ppc64__ || __LP64__
#       define ENVIRONMENT64
#   else
#       define ENVIRONMENT32
#   endif
#endif

#define VERSION_NUMBER "2.10"

#ifndef VERSION_STRING
#   define VERSION_STRING "LGOGDownloader " VERSION_NUMBER
#endif

namespace bpo = boost::program_options;

int main(int argc, char *argv[])
{
    Config config;
    config.sVersionString = VERSION_STRING;
    char *xdgconfig = getenv("XDG_CONFIG_HOME");
    char *xdgcache = getenv("XDG_CACHE_HOME");
    std::string home = (std::string)getenv("HOME");

    if (xdgconfig)
    {
        config.sConfigDirectory = (std::string)xdgconfig + "/lgogdownloader";
        config.sCookiePath = config.sConfigDirectory + "/cookies.txt";
        config.sConfigFilePath = config.sConfigDirectory + "/config.cfg";
    }
    else
    {
        config.sConfigDirectory = home + "/.config/lgogdownloader";
        config.sCookiePath = config.sConfigDirectory + "/cookies.txt";
        config.sConfigFilePath = config.sConfigDirectory + "/config.cfg";
    }

    if (xdgcache)
        config.sXMLDirectory = (std::string)xdgcache + "/lgogdownloader/xml";
    else
        config.sXMLDirectory = home + "/.cache/lgogdownloader/xml";

    // Create lgogdownloader directories
    boost::filesystem::path path = config.sXMLDirectory;
    if (!boost::filesystem::exists(path))
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return 1;
        }
    }

    path = config.sConfigDirectory;
    if (!boost::filesystem::exists(path))
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return 1;
        }
    }

    // Create help text for --platform option
    std::string platform_text = "Select which installers are downloaded\n";
    unsigned int platform_sum = 0;
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        platform_text += std::to_string(GlobalConstants::PLATFORMS[i].platformId) + " = " + GlobalConstants::PLATFORMS[i].platformString + "\n";
        platform_sum += GlobalConstants::LANGUAGES[i].languageId;
    }
    platform_text += std::to_string(platform_sum) + " = All";

    // Create help text for --language option
    std::string language_text = "Select which language installers are downloaded\n";
    unsigned int language_sum = 0;
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        language_text += std::to_string(GlobalConstants::LANGUAGES[i].languageId) + " = " + GlobalConstants::LANGUAGES[i].languageString + "\n";
        language_sum += GlobalConstants::LANGUAGES[i].languageId;
    }
    language_text += "Add the values to download multiple languages\nAll = " + std::to_string(language_sum) + "\n"
                    + "French + Polish = " + std::to_string(GlobalConstants::LANGUAGE_FR) + "+" + std::to_string(GlobalConstants::LANGUAGE_PL) + " = " + std::to_string(GlobalConstants::LANGUAGE_FR | GlobalConstants::LANGUAGE_PL);

    bpo::variables_map vm;
    bpo::options_description desc("Options");
    bpo::options_description config_file_options("Configuration");
    try
    {
        bool bInsecure = false;
        bool bNoColor = false;
        bool bNoUnicode = false;
        bool bNoDuplicateHandler = false;
        bool bNoCover = false;
        bool bNoInstallers = false;
        bool bNoExtras = false;
        bool bNoPatches = false;
        bool bNoLanguagePacks = false;
        bool bNoRemoteXML = false;
        desc.add_options()
            ("help,h", "Print help message")
            ("login", bpo::value<bool>(&config.bLogin)->zero_tokens()->default_value(false), "Login")
            ("list", bpo::value<bool>(&config.bList)->zero_tokens()->default_value(false), "List games")
            ("list-details", bpo::value<bool>(&config.bListDetails)->zero_tokens()->default_value(false), "List games with detailed info")
            ("download", bpo::value<bool>(&config.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&config.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files\nUse --repair --download to redownload files when filesizes don't match (possibly different version). Redownload will delete the old file")
            ("game", bpo::value<std::string>(&config.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)\nAliases: \"all\", \"free\"")
            ("directory", bpo::value<std::string>(&config.sDirectory)->default_value(""), "Set download directory")
            ("limit-rate", bpo::value<curl_off_t>(&config.iDownloadRate)->default_value(0), "Limit download rate to value in kB\n0 = unlimited")
            ("create-xml", bpo::value<std::string>(&config.sXMLFile)->default_value(""), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("xml-directory", bpo::value<std::string>(&config.sXMLDirectory), "Set directory for GOG XML files")
            ("chunk-size", bpo::value<size_t>(&config.iChunkSize)->default_value(10), "Chunk size (in MB) when creating XML")
            ("update-check", bpo::value<bool>(&config.bUpdateCheck)->zero_tokens()->default_value(false), "Check for update notifications")
            ("platform", bpo::value<unsigned int>(&config.iInstallerType)->default_value(GlobalConstants::PLATFORM_WINDOWS), platform_text.c_str())
            ("language", bpo::value<unsigned int>(&config.iInstallerLanguage)->default_value(GlobalConstants::LANGUAGE_EN), language_text.c_str())
            ("no-installers", bpo::value<bool>(&bNoInstallers)->zero_tokens()->default_value(false), "Don't download/list/repair installers")
            ("no-extras", bpo::value<bool>(&bNoExtras)->zero_tokens()->default_value(false), "Don't download/list/repair extras")
            ("no-patches", bpo::value<bool>(&bNoPatches)->zero_tokens()->default_value(false), "Don't download/list/repair patches")
            ("no-language-packs", bpo::value<bool>(&bNoLanguagePacks)->zero_tokens()->default_value(false), "Don't download/list/repair language packs")
            ("no-cover", bpo::value<bool>(&bNoCover)->zero_tokens()->default_value(false), "Don't download cover images")
            ("no-remote-xml", bpo::value<bool>(&bNoRemoteXML)->zero_tokens()->default_value(false), "Don't use remote XML for repair")
            ("no-unicode", bpo::value<bool>(&bNoUnicode)->zero_tokens()->default_value(false), "Don't use Unicode in the progress bar")
            ("no-color", bpo::value<bool>(&bNoColor)->zero_tokens()->default_value(false), "Don't use coloring in the progress bar")
            ("no-duplicate-handling", bpo::value<bool>(&bNoDuplicateHandler)->zero_tokens()->default_value(false), "Don't use duplicate handler for installers\nDuplicate installers from different languages are handled separately")
            ("verbose", bpo::value<bool>(&config.bVerbose)->zero_tokens()->default_value(false), "Print lots of information")
            ("insecure", bpo::value<bool>(&bInsecure)->zero_tokens()->default_value(false), "Don't verify authenticity of SSL certificates")
            ("timeout", bpo::value<long int>(&config.iTimeout)->default_value(10), "Set timeout for connection\nMaximum time in seconds that connection phase is allowed to take")
            ("check-orphans", bpo::value<bool>(&config.bCheckOrphans)->zero_tokens()->default_value(false), "Check for orphaned files (files found on local filesystem that are not found on GOG servers)")
            ("status", bpo::value<bool>(&config.bCheckStatus)->zero_tokens()->default_value(false), "Show status of files\n\nOutput format:\nstatuscode gamename filename filesize filehash\n\nStatus codes:\nOK - File is OK\nND - File is not downloaded\nMD5 - MD5 mismatch, different version")
        ;

        bpo::store(bpo::parse_command_line(argc, argv, desc), vm);
        bpo::notify(vm);

        // Read token and secret from config file
        config_file_options.add_options()
            ("token", bpo::value<std::string>(&config.sToken)->default_value(""), "oauth token")
            ("secret", bpo::value<std::string>(&config.sSecret)->default_value(""), "oauth secret")
        ;

        if (boost::filesystem::exists(config.sConfigFilePath))
        {
            std::ifstream ifs(config.sConfigFilePath.c_str());
            if (!ifs)
            {
                std::cout << "Could not open config file: " << config.sConfigFilePath << std::endl;
                return 1;
            }
            else
            {
                bpo::store(bpo::parse_config_file(ifs, config_file_options), vm);
                bpo::notify(vm);
                ifs.close();
            }
        }

        if (vm.count("help"))
        {
            std::cout   << config.sVersionString << std::endl
                        << desc << std::endl;
            return 0;
        }

        if (vm.count("chunk-size"))
            config.iChunkSize <<= 20; // Convert chunk size from bytes to megabytes

        if (vm.count("limit-rate"))
            config.iDownloadRate <<= 10; // Convert download rate from bytes to kilobytes

        config.bVerifyPeer = !bInsecure;
        config.bColor = !bNoColor;
        config.bUnicode = !bNoUnicode;
        config.bDuplicateHandler = !bNoDuplicateHandler;
        config.bCover = !bNoCover;
        config.bInstallers = !bNoInstallers;
        config.bExtras = !bNoExtras;
        config.bPatches = !bNoPatches;
        config.bLanguagePacks = !bNoLanguagePacks;
        config.bRemoteXML = !bNoRemoteXML;
    }
    catch (std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "Exception of unknown type!" << std::endl;
        return 1;
    }

    if (config.iInstallerType < GlobalConstants::PLATFORMS[0].platformId || config.iInstallerType > platform_sum)
    {
        std::cout << "Invalid value for --platform" << std::endl;
        return 1;
    }

    if (config.iInstallerLanguage < GlobalConstants::LANGUAGES[0].languageId || config.iInstallerLanguage > language_sum)
    {
        std::cout << "Invalid value for --language" << std::endl;
        return 1;
    }

    if (!config.sXMLDirectory.empty())
    {
        // Make sure that xml directory doesn't have trailing slash
        if (config.sXMLDirectory.at(config.sXMLDirectory.length()-1)=='/')
            config.sXMLDirectory.assign(config.sXMLDirectory.begin(),config.sXMLDirectory.end()-1);
    }

    // Create GOG XML for a file
    if (!config.sXMLFile.empty() && (config.sXMLFile != "automatic"))
    {
        Util::createXML(config.sXMLFile, config.iChunkSize, config.sXMLDirectory);
        return 0;
    }

    // Make sure that directory has trailing slash
    if (!config.sDirectory.empty())
        if (config.sDirectory.at(config.sDirectory.length()-1)!='/')
            config.sDirectory += "/";

    Downloader downloader(config);
    int result = downloader.init();

    if (config.bLogin)
        return result;
    else if (config.bUpdateCheck) // Update check has priority over download and list
        downloader.updateCheck();
    else if (config.bRepair) // Repair file
        downloader.repair();
    else if (config.bDownload) // Download games
        downloader.download();
    else if (config.bListDetails || config.bList) // Detailed list of games/extras
        downloader.listGames();
    else if (config.bCheckOrphans)
        downloader.checkOrphans();
    else if (config.bCheckStatus)
        downloader.checkStatus();
    else
    {   // Show help message
        std::cout   << config.sVersionString << std::endl
                    << desc << std::endl;
    }

    return 0;
}
