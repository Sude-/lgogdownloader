/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "config.h"
#include "util.h"
#include "globalconstants.h"
#include "ssl_thread_setup.h"
#include "galaxyapi.h"
#include "globals.h"

#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

namespace bpo = boost::program_options;
Config Globals::globalConfig;

template<typename T> void set_vm_value(std::map<std::string, bpo::variable_value>& vm, const std::string& option, const T& value)
{
    vm[option].value() = boost::any(value);
}

int main(int argc, char *argv[])
{
    // Constants for option selection with include/exclude
    /* TODO: Add options to give better control for user
             For example: option to select base game and DLC installers separately,
             this requires some changes to Downloader class to implement */
    const unsigned int OPTION_INSTALLERS = 1 << 0;
    const unsigned int OPTION_EXTRAS     = 1 << 1;
    const unsigned int OPTION_PATCHES    = 1 << 2;
    const unsigned int OPTION_LANGPACKS  = 1 << 3;
    const unsigned int OPTION_DLCS       = 1 << 4;

    const std::vector<GlobalConstants::optionsStruct> INCLUDE_OPTIONS =
    {
        { OPTION_INSTALLERS, "i", "Installers",     "i|installers"              },
        { OPTION_EXTRAS,     "e", "Extras",         "e|extras"                  },
        { OPTION_PATCHES,    "p", "Patches",        "p|patches"                 },
        { OPTION_LANGPACKS,  "l", "Language packs", "l|languagepacks|langpacks" },
        { OPTION_DLCS,       "d", "DLCs",           "d|dlc|dlcs"                }
    };

    Globals::globalConfig.sVersionString = VERSION_STRING;
    Globals::globalConfig.sVersionNumber = VERSION_NUMBER;

    Globals::globalConfig.sCacheDirectory = Util::getCacheHome() + "/lgogdownloader";
    Globals::globalConfig.sXMLDirectory = Globals::globalConfig.sCacheDirectory + "/xml";

    Globals::globalConfig.sConfigDirectory = Util::getConfigHome() + "/lgogdownloader";
    Globals::globalConfig.curlConf.sCookiePath = Globals::globalConfig.sConfigDirectory + "/cookies.txt";
    Globals::globalConfig.sConfigFilePath = Globals::globalConfig.sConfigDirectory + "/config.cfg";
    Globals::globalConfig.sBlacklistFilePath = Globals::globalConfig.sConfigDirectory + "/blacklist.txt";
    Globals::globalConfig.sIgnorelistFilePath = Globals::globalConfig.sConfigDirectory + "/ignorelist.txt";
    Globals::globalConfig.sGameHasDLCListFilePath = Globals::globalConfig.sConfigDirectory + "/game_has_dlc.txt";

    Globals::galaxyConf.setFilepath(Globals::globalConfig.sConfigDirectory + "/galaxy_tokens.json");

    std::string priority_help_text = "Set priority by separating values with \",\"\nCombine values by separating with \"+\"";
    // Create help text for --platform option
    std::string platform_text = "Select which installers are downloaded\n";
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        platform_text += GlobalConstants::PLATFORMS[i].str + " = " + GlobalConstants::PLATFORMS[i].regexp + "\n";
    }
    platform_text += "All = all";
    platform_text += "\n\n" + priority_help_text;
    platform_text += "\nExample: Linux if available otherwise Windows and Mac: l,w+m";

    // Create help text for --galaxy-platform option
    std::string galaxy_platform_text = "Select platform\n";
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        galaxy_platform_text += GlobalConstants::PLATFORMS[i].str + " = " + GlobalConstants::PLATFORMS[i].regexp + "\n";
    }

    // Create help text for --language option
    std::string language_text = "Select which language installers are downloaded\n";
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        language_text +=  GlobalConstants::LANGUAGES[i].str + " = " + GlobalConstants::LANGUAGES[i].regexp + "\n";
    }
    language_text += "All = all";
    language_text += "\n\n" + priority_help_text;
    language_text += "\nExample: German if available otherwise English and French: de,en+fr";

    // Create help text for --galaxy-language option
    std::string galaxy_language_text = "Select language\n";
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        galaxy_language_text +=  GlobalConstants::LANGUAGES[i].str + " = " + GlobalConstants::LANGUAGES[i].regexp + "\n";
    }

    // Create help text for --galaxy-arch option
    std::string galaxy_arch_text = "Select architecture\n";
    for (unsigned int i = 0; i < GlobalConstants::GALAXY_ARCHS.size(); ++i)
    {
        galaxy_arch_text +=  GlobalConstants::GALAXY_ARCHS[i].str + " = " + GlobalConstants::GALAXY_ARCHS[i].regexp + "\n";
    }

    // Create help text for --check-orphans
    std::string orphans_regex_default = ".*\\.(zip|exe|bin|dmg|old|deb|tar\\.gz|pkg|sh)$"; // Limit to files with these extensions (".old" is for renamed older version files)
    std::string check_orphans_text = "Check for orphaned files (files found on local filesystem that are not found on GOG servers). Sets regular expression filter (Perl syntax) for files to check. If no argument is given then the regex defaults to '" + orphans_regex_default + "'";

    // Help text for subdir options
    std::string subdir_help_text = "\nTemplates:\n- %platform%\n- %gamename%\n- %dlcname%";

    // Help text for include and exclude options
    std::string include_options_text;
    for (unsigned int i = 0; i < INCLUDE_OPTIONS.size(); ++i)
    {
        include_options_text +=  INCLUDE_OPTIONS[i].str + " = " + INCLUDE_OPTIONS[i].regexp + "\n";
    }
    include_options_text += "Separate with \",\" to use multiple values";

    std::string galaxy_product_id_install;
    std::string galaxy_product_id_show_builds;

    std::vector<std::string> vFileIdStrings;
    std::vector<std::string> unrecognized_options_cfg;
    std::vector<std::string> unrecognized_options_cli;
    bpo::variables_map vm;
    bpo::options_description options_cli_all("Options");
    bpo::options_description options_cli_no_cfg;
    bpo::options_description options_cli_no_cfg_hidden;
    bpo::options_description options_cli_all_include_hidden;
    bpo::options_description options_cli_experimental("Experimental");
    bpo::options_description options_cli_cfg;
    bpo::options_description options_cfg_only;
    bpo::options_description options_cfg_all("Configuration");
    try
    {
        bool bInsecure = false;
        bool bNoColor = false;
        bool bNoUnicode = false;
        bool bNoDuplicateHandler = false;
        bool bNoRemoteXML = false;
        bool bNoSubDirectories = false;
        bool bNoPlatformDetection = false;
        bool bLogin = false;
        std::string sInstallerPlatform;
        std::string sInstallerLanguage;
        std::string sIncludeOptions;
        std::string sExcludeOptions;
        std::string sGalaxyPlatform;
        std::string sGalaxyLanguage;
        std::string sGalaxyArch;
        Globals::globalConfig.bReport = false;
        // Commandline options (no config file)
        options_cli_no_cfg.add_options()
            ("help,h", "Print help message")
            ("version", "Print version information")
            ("login", bpo::value<bool>(&bLogin)->zero_tokens()->default_value(false), "Login")
            ("list", bpo::value<bool>(&Globals::globalConfig.bList)->zero_tokens()->default_value(false), "List games")
            ("list-details", bpo::value<bool>(&Globals::globalConfig.bListDetails)->zero_tokens()->default_value(false), "List games with detailed info")
            ("download", bpo::value<bool>(&Globals::globalConfig.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&Globals::globalConfig.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files\nUse --repair --download to redownload files when filesizes don't match (possibly different version). Redownload will rename the old file (appends .old to filename)")
            ("game", bpo::value<std::string>(&Globals::globalConfig.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)\nAliases: \"all\", \"free\"\nAlias \"free\" doesn't work with cached details")
            ("create-xml", bpo::value<std::string>(&Globals::globalConfig.sXMLFile)->implicit_value("automatic"), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("update-check", bpo::value<bool>(&Globals::globalConfig.bUpdateCheck)->zero_tokens()->default_value(false), "Check for update notifications")
            ("check-orphans", bpo::value<std::string>(&Globals::globalConfig.sOrphanRegex)->implicit_value(""), check_orphans_text.c_str())
            ("status", bpo::value<bool>(&Globals::globalConfig.bCheckStatus)->zero_tokens()->default_value(false), "Show status of files\n\nOutput format:\nstatuscode gamename filename filesize filehash\n\nStatus codes:\nOK - File is OK\nND - File is not downloaded\nMD5 - MD5 mismatch, different version\nFS - File size mismatch, incomplete download")
            ("save-config", bpo::value<bool>(&Globals::globalConfig.bSaveConfig)->zero_tokens()->default_value(false), "Create config file with current settings")
            ("reset-config", bpo::value<bool>(&Globals::globalConfig.bResetConfig)->zero_tokens()->default_value(false), "Reset config settings to default")
            ("report", bpo::value<std::string>(&Globals::globalConfig.sReportFilePath)->implicit_value("lgogdownloader-report.log"), "Save report of downloaded/repaired files to specified file\nDefault filename: lgogdownloader-report.log")
            ("update-cache", bpo::value<bool>(&Globals::globalConfig.bUpdateCache)->zero_tokens()->default_value(false), "Update game details cache")
            ("no-platform-detection", bpo::value<bool>(&bNoPlatformDetection)->zero_tokens()->default_value(false), "Don't try to detect supported platforms from game shelf.\nSkips the initial fast platform detection and detects the supported platforms from game details which is slower but more accurate.\nUseful in case platform identifier is missing for some games in the game shelf.\nUsing --platform with --list doesn't work with this option.")
            ("download-file", bpo::value<std::string>(&Globals::globalConfig.sFileIdString)->default_value(""), "Download files using fileid\n\nFormat:\n\"gamename/fileid\"\nor: \"gogdownloader://gamename/fileid\"\n\nMultiple files:\n\"gamename1/fileid1,gamename2/fileid2\"\nor: \"gogdownloader://gamename1/fileid1,gamename2/fileid2\"\n\nThis option ignores all subdir options. The files are downloaded to directory specified with --directory option.")
            ("output-file,o", bpo::value<std::string>(&Globals::globalConfig.sOutputFilename)->default_value(""), "Set filename of file downloaded with --download-file.")
            ("wishlist", bpo::value<bool>(&Globals::globalConfig.bShowWishlist)->zero_tokens()->default_value(false), "Show wishlist")
            ("login-api", bpo::value<bool>(&Globals::globalConfig.bLoginAPI)->zero_tokens()->default_value(false), "Login (API only)")
            ("login-website", bpo::value<bool>(&Globals::globalConfig.bLoginHTTP)->zero_tokens()->default_value(false), "Login (website only)")
            ("cacert", bpo::value<std::string>(&Globals::globalConfig.curlConf.sCACertPath)->default_value(""), "Path to CA certificate bundle in PEM format")
            ("respect-umask", bpo::value<bool>(&Globals::globalConfig.bRespectUmask)->zero_tokens()->default_value(false), "Do not adjust permissions of sensitive files")
            ("user-agent", bpo::value<std::string>(&Globals::globalConfig.curlConf.sUserAgent)->default_value(Globals::globalConfig.sVersionString), "Set user agent")
        ;
        // Commandline options (config file)
        options_cli_cfg.add_options()
            ("directory", bpo::value<std::string>(&Globals::globalConfig.dirConf.sDirectory)->default_value("."), "Set download directory")
            ("limit-rate", bpo::value<curl_off_t>(&Globals::globalConfig.curlConf.iDownloadRate)->default_value(0), "Limit download rate to value in kB\n0 = unlimited")
            ("xml-directory", bpo::value<std::string>(&Globals::globalConfig.sXMLDirectory), "Set directory for GOG XML files")
            ("chunk-size", bpo::value<size_t>(&Globals::globalConfig.iChunkSize)->default_value(10), "Chunk size (in MB) when creating XML")
            ("platform", bpo::value<std::string>(&sInstallerPlatform)->default_value("w+l"), platform_text.c_str())
            ("language", bpo::value<std::string>(&sInstallerLanguage)->default_value("en"), language_text.c_str())
            ("no-remote-xml", bpo::value<bool>(&bNoRemoteXML)->zero_tokens()->default_value(false), "Don't use remote XML for repair")
            ("no-unicode", bpo::value<bool>(&bNoUnicode)->zero_tokens()->default_value(false), "Don't use Unicode in the progress bar")
            ("no-color", bpo::value<bool>(&bNoColor)->zero_tokens()->default_value(false), "Don't use coloring in the progress bar or status messages")
            ("no-duplicate-handling", bpo::value<bool>(&bNoDuplicateHandler)->zero_tokens()->default_value(false), "Don't use duplicate handler for installers\nDuplicate installers from different languages are handled separately")
            ("no-subdirectories", bpo::value<bool>(&bNoSubDirectories)->zero_tokens()->default_value(false), "Don't create subdirectories for extras, patches and language packs")
            ("verbose", bpo::value<bool>(&Globals::globalConfig.bVerbose)->zero_tokens()->default_value(false), "Print lots of information")
            ("insecure", bpo::value<bool>(&bInsecure)->zero_tokens()->default_value(false), "Don't verify authenticity of SSL certificates")
            ("timeout", bpo::value<long int>(&Globals::globalConfig.curlConf.iTimeout)->default_value(10), "Set timeout for connection\nMaximum time in seconds that connection phase is allowed to take")
            ("retries", bpo::value<int>(&Globals::globalConfig.iRetries)->default_value(3), "Set maximum number of retries on failed download")
            ("wait", bpo::value<int>(&Globals::globalConfig.iWait)->default_value(0), "Time to wait between requests (milliseconds)")
            ("subdir-installers", bpo::value<std::string>(&Globals::globalConfig.dirConf.sInstallersSubdir)->default_value(""), ("Set subdirectory for installers" + subdir_help_text).c_str())
            ("subdir-extras", bpo::value<std::string>(&Globals::globalConfig.dirConf.sExtrasSubdir)->default_value("extras"), ("Set subdirectory for extras" + subdir_help_text).c_str())
            ("subdir-patches", bpo::value<std::string>(&Globals::globalConfig.dirConf.sPatchesSubdir)->default_value("patches"), ("Set subdirectory for patches" + subdir_help_text).c_str())
            ("subdir-language-packs", bpo::value<std::string>(&Globals::globalConfig.dirConf.sLanguagePackSubdir)->default_value("languagepacks"), ("Set subdirectory for language packs" + subdir_help_text).c_str())
            ("subdir-dlc", bpo::value<std::string>(&Globals::globalConfig.dirConf.sDLCSubdir)->default_value("dlc/%dlcname%"), ("Set subdirectory for dlc" + subdir_help_text).c_str())
            ("subdir-game", bpo::value<std::string>(&Globals::globalConfig.dirConf.sGameSubdir)->default_value("%gamename%"), ("Set subdirectory for game" + subdir_help_text).c_str())
            ("use-cache", bpo::value<bool>(&Globals::globalConfig.bUseCache)->zero_tokens()->default_value(false), ("Use game details cache"))
            ("cache-valid", bpo::value<int>(&Globals::globalConfig.iCacheValid)->default_value(2880), ("Set how long cached game details are valid (in minutes)\nDefault: 2880 minutes (48 hours)"))
            ("save-serials", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveSerials)->zero_tokens()->default_value(false), "Save serial numbers when downloading")
            ("ignore-dlc-count", bpo::value<std::string>(&Globals::globalConfig.sIgnoreDLCCountRegex)->implicit_value(".*"), "Set regular expression filter for games to ignore DLC count information\nIgnoring DLC count information helps in situations where the account page doesn't provide accurate information about DLCs")
            ("include", bpo::value<std::string>(&sIncludeOptions)->default_value("all"), ("Select what to download/list/repair\n" + include_options_text).c_str())
            ("exclude", bpo::value<std::string>(&sExcludeOptions)->default_value(""), ("Select what not to download/list/repair\n" + include_options_text).c_str())
            ("automatic-xml-creation", bpo::value<bool>(&Globals::globalConfig.dlConf.bAutomaticXMLCreation)->zero_tokens()->default_value(false), "Automatically create XML data after download has completed")
            ("save-changelogs", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveChangelogs)->zero_tokens()->default_value(false), "Save changelogs when downloading")
            ("threads", bpo::value<unsigned int>(&Globals::globalConfig.iThreads)->default_value(4), "Number of download threads")
            ("dlc-list", bpo::value<std::string>(&Globals::globalConfig.sGameHasDLCList)->default_value("https://raw.githubusercontent.com/Sude-/lgogdownloader-lists/master/game_has_dlc.txt"), "Set URL for list of games that have DLC")
            ("progress-interval", bpo::value<int>(&Globals::globalConfig.iProgressInterval)->default_value(100), "Set interval for progress bar update (milliseconds)\nValue must be between 1 and 10000")
            ("lowspeed-timeout", bpo::value<long int>(&Globals::globalConfig.curlConf.iLowSpeedTimeout)->default_value(30), "Set time in number seconds that the transfer speed should be below the rate set with --lowspeed-rate for it to considered too slow and aborted")
            ("lowspeed-rate", bpo::value<long int>(&Globals::globalConfig.curlConf.iLowSpeedTimeoutRate)->default_value(200), "Set average transfer speed in bytes per second that the transfer should be below during time specified with --lowspeed-timeout for it to be considered too slow and aborted")
        ;
        // Options read from config file
        options_cfg_only.add_options()
            ("token", bpo::value<std::string>(&Globals::globalConfig.apiConf.sToken)->default_value(""), "oauth token")
            ("secret", bpo::value<std::string>(&Globals::globalConfig.apiConf.sSecret)->default_value(""), "oauth secret")
        ;

        options_cli_no_cfg_hidden.add_options()
            ("login-email", bpo::value<std::string>(&Globals::globalConfig.sEmail)->default_value(""), "login email")
            ("login-password", bpo::value<std::string>(&Globals::globalConfig.sPassword)->default_value(""), "login password")
        ;

        options_cli_experimental.add_options()
            ("galaxy-install", bpo::value<std::string>(&galaxy_product_id_install)->default_value(""), "Install game using product id")
            ("galaxy-show-builds", bpo::value<std::string>(&galaxy_product_id_show_builds)->default_value(""), "Show game builds using product id")
            ("galaxy-platform", bpo::value<std::string>(&sGalaxyPlatform)->default_value("w"), galaxy_platform_text.c_str())
            ("galaxy-language", bpo::value<std::string>(&sGalaxyLanguage)->default_value("en"), galaxy_language_text.c_str())
            ("galaxy-arch", bpo::value<std::string>(&sGalaxyArch)->default_value("x64"), galaxy_arch_text.c_str())
        ;

        options_cli_all.add(options_cli_no_cfg).add(options_cli_cfg).add(options_cli_experimental);
        options_cfg_all.add(options_cfg_only).add(options_cli_cfg);
        options_cli_all_include_hidden.add(options_cli_all).add(options_cli_no_cfg_hidden);

        bpo::parsed_options parsed = bpo::parse_command_line(argc, argv, options_cli_all_include_hidden);
        bpo::store(parsed, vm);
        unrecognized_options_cli = bpo::collect_unrecognized(parsed.options, bpo::include_positional);
        bpo::notify(vm);

        if (vm.count("help"))
        {
            std::cout   << Globals::globalConfig.sVersionString << std::endl
                        << options_cli_all << std::endl;
            return 0;
        }

        if (vm.count("version"))
        {
            std::cout << VERSION_STRING << std::endl;
            return 0;
        }

        // Create lgogdownloader directories
        boost::filesystem::path path = Globals::globalConfig.sXMLDirectory;
        if (!boost::filesystem::exists(path))
        {
            if (!boost::filesystem::create_directories(path))
            {
                std::cerr << "Failed to create directory: " << path << std::endl;
                return 1;
            }
        }

        path = Globals::globalConfig.sConfigDirectory;
        if (!boost::filesystem::exists(path))
        {
            if (!boost::filesystem::create_directories(path))
            {
                std::cerr << "Failed to create directory: " << path << std::endl;
                return 1;
            }
        }

        path = Globals::globalConfig.sCacheDirectory;
        if (!boost::filesystem::exists(path))
        {
            if (!boost::filesystem::create_directories(path))
            {
                std::cerr << "Failed to create directory: " << path << std::endl;
                return 1;
            }
        }

        if (boost::filesystem::exists(Globals::globalConfig.sConfigFilePath))
        {
            std::ifstream ifs(Globals::globalConfig.sConfigFilePath.c_str());
            if (!ifs)
            {
                std::cerr << "Could not open config file: " << Globals::globalConfig.sConfigFilePath << std::endl;
                return 1;
            }
            else
            {
                bpo::parsed_options parsed = bpo::parse_config_file(ifs, options_cfg_all, true);
                bpo::store(parsed, vm);
                bpo::notify(vm);
                ifs.close();
                unrecognized_options_cfg = bpo::collect_unrecognized(parsed.options, bpo::include_positional);
            }
        }
        if (boost::filesystem::exists(Globals::globalConfig.sBlacklistFilePath))
        {
            std::ifstream ifs(Globals::globalConfig.sBlacklistFilePath.c_str());
            if (!ifs)
            {
                std::cerr << "Could not open blacklist file: " << Globals::globalConfig.sBlacklistFilePath << std::endl;
                return 1;
            }
            else
            {
                std::string line;
                std::vector<std::string> lines;
                while (!ifs.eof())
                {
                    std::getline(ifs, line);
                    lines.push_back(std::move(line));
                }
                Globals::globalConfig.blacklist.initialize(lines);
            }
        }

        if (boost::filesystem::exists(Globals::globalConfig.sIgnorelistFilePath))
        {
            std::ifstream ifs(Globals::globalConfig.sIgnorelistFilePath.c_str());
            if (!ifs)
            {
                std::cerr << "Could not open ignorelist file: " << Globals::globalConfig.sIgnorelistFilePath << std::endl;
                return 1;
            }
            else
            {
                std::string line;
                std::vector<std::string> lines;
                while (!ifs.eof())
                {
                    std::getline(ifs, line);
                    lines.push_back(std::move(line));
                }
                Globals::globalConfig.ignorelist.initialize(lines);
            }
        }

        if (Globals::globalConfig.sIgnoreDLCCountRegex.empty())
        {
            if (boost::filesystem::exists(Globals::globalConfig.sGameHasDLCListFilePath))
            {
                std::ifstream ifs(Globals::globalConfig.sGameHasDLCListFilePath.c_str());
                if (!ifs)
                {
                    std::cerr << "Could not open list of games that have dlc: " << Globals::globalConfig.sGameHasDLCListFilePath << std::endl;
                    return 1;
                }
                else
                {
                    std::string line;
                    std::vector<std::string> lines;
                    while (!ifs.eof())
                    {
                        std::getline(ifs, line);
                        lines.push_back(std::move(line));
                    }
                    Globals::globalConfig.gamehasdlc.initialize(lines);
                }
            }
        }

        if (bLogin || Globals::globalConfig.bLoginAPI || Globals::globalConfig.bLoginHTTP)
        {
            std::string login_conf = Globals::globalConfig.sConfigDirectory + "/login.txt";
            if (boost::filesystem::exists(login_conf))
            {
                std::ifstream ifs(login_conf);
                if (!ifs)
                {
                    std::cerr << "Could not open login conf: " << login_conf << std::endl;
                    return 1;
                }
                else
                {
                    std::string line;
                    std::vector<std::string> lines;
                    while (!ifs.eof())
                    {
                        std::getline(ifs, line);
                        lines.push_back(std::move(line));
                    }
                    Globals::globalConfig.sEmail = lines[0];
                    Globals::globalConfig.sPassword = lines[1];
                }
            }
        }

        if (vm.count("chunk-size"))
            Globals::globalConfig.iChunkSize <<= 20; // Convert chunk size from bytes to megabytes

        if (vm.count("limit-rate"))
            Globals::globalConfig.curlConf.iDownloadRate <<= 10; // Convert download rate from bytes to kilobytes

        if (vm.count("check-orphans"))
            if (Globals::globalConfig.sOrphanRegex.empty())
                Globals::globalConfig.sOrphanRegex = orphans_regex_default;

        if (vm.count("report"))
            Globals::globalConfig.bReport = true;

        if (Globals::globalConfig.iWait > 0)
            Globals::globalConfig.iWait *= 1000;

        if (Globals::globalConfig.iProgressInterval < 1)
            Globals::globalConfig.iProgressInterval = 1;
        else if (Globals::globalConfig.iProgressInterval > 10000)
            Globals::globalConfig.iProgressInterval = 10000;

        if (Globals::globalConfig.iThreads < 1)
        {
            Globals::globalConfig.iThreads = 1;
            set_vm_value(vm, "threads", Globals::globalConfig.iThreads);
        }

        Globals::globalConfig.curlConf.bVerbose = Globals::globalConfig.bVerbose;
        Globals::globalConfig.curlConf.bVerifyPeer = !bInsecure;
        Globals::globalConfig.bColor = !bNoColor;
        Globals::globalConfig.bUnicode = !bNoUnicode;
        Globals::globalConfig.dlConf.bDuplicateHandler = !bNoDuplicateHandler;
        Globals::globalConfig.dlConf.bRemoteXML = !bNoRemoteXML;
        Globals::globalConfig.dirConf.bSubDirectories = !bNoSubDirectories;
        Globals::globalConfig.bPlatformDetection = !bNoPlatformDetection;

        for (auto i = unrecognized_options_cli.begin(); i != unrecognized_options_cli.end(); ++i)
            if (i->compare(0, GlobalConstants::PROTOCOL_PREFIX.length(), GlobalConstants::PROTOCOL_PREFIX) == 0)
                Globals::globalConfig.sFileIdString = *i;

        if (!Globals::globalConfig.sFileIdString.empty())
        {
            if (Globals::globalConfig.sFileIdString.compare(0, GlobalConstants::PROTOCOL_PREFIX.length(), GlobalConstants::PROTOCOL_PREFIX) == 0)
            {
                Globals::globalConfig.sFileIdString.replace(0, GlobalConstants::PROTOCOL_PREFIX.length(), "");
            }
            vFileIdStrings = Util::tokenize(Globals::globalConfig.sFileIdString, ",");
        }

        if (!Globals::globalConfig.sOutputFilename.empty() && vFileIdStrings.size() > 1)
        {
            std::cerr << "Cannot specify an output file name when downloading multiple files." << std::endl;
            return 1;
        }

        if (bLogin)
        {
            Globals::globalConfig.bLoginAPI = true;
            Globals::globalConfig.bLoginHTTP = true;
        }

        if (Globals::globalConfig.sXMLFile == "automatic")
            Globals::globalConfig.dlConf.bAutomaticXMLCreation = true;

        Util::parseOptionString(sInstallerLanguage, Globals::globalConfig.dlConf.vLanguagePriority, Globals::globalConfig.dlConf.iInstallerLanguage, GlobalConstants::LANGUAGES);
        Util::parseOptionString(sInstallerPlatform, Globals::globalConfig.dlConf.vPlatformPriority, Globals::globalConfig.dlConf.iInstallerPlatform, GlobalConstants::PLATFORMS);

        Globals::globalConfig.dlConf.iGalaxyPlatform = Util::getOptionValue(sGalaxyPlatform, GlobalConstants::PLATFORMS);
        Globals::globalConfig.dlConf.iGalaxyLanguage = Util::getOptionValue(sGalaxyLanguage, GlobalConstants::LANGUAGES);
        Globals::globalConfig.dlConf.iGalaxyArch = Util::getOptionValue(sGalaxyArch, GlobalConstants::GALAXY_ARCHS, false);

        if (Globals::globalConfig.dlConf.iGalaxyArch == 0 || Globals::globalConfig.dlConf.iGalaxyArch == Util::getOptionValue("all", GlobalConstants::GALAXY_ARCHS, false))
            Globals::globalConfig.dlConf.iGalaxyArch = GlobalConstants::ARCH_X64;

        unsigned int include_value = 0;
        unsigned int exclude_value = 0;
        std::vector<std::string> vInclude = Util::tokenize(sIncludeOptions, ",");
        std::vector<std::string> vExclude = Util::tokenize(sExcludeOptions, ",");
        for (std::vector<std::string>::iterator it = vInclude.begin(); it != vInclude.end(); it++)
        {
            include_value |= Util::getOptionValue(*it, INCLUDE_OPTIONS);
        }
        for (std::vector<std::string>::iterator it = vExclude.begin(); it != vExclude.end(); it++)
        {
            exclude_value |= Util::getOptionValue(*it, INCLUDE_OPTIONS);
        }
        Globals::globalConfig.dlConf.iInclude = include_value & ~exclude_value;

        // Assign values
        // TODO: Use config.iInclude in Downloader class directly and get rid of this value assignment
        Globals::globalConfig.dlConf.bInstallers = (Globals::globalConfig.dlConf.iInclude & OPTION_INSTALLERS);
        Globals::globalConfig.dlConf.bExtras = (Globals::globalConfig.dlConf.iInclude & OPTION_EXTRAS);
        Globals::globalConfig.dlConf.bPatches = (Globals::globalConfig.dlConf.iInclude & OPTION_PATCHES);
        Globals::globalConfig.dlConf.bLanguagePacks = (Globals::globalConfig.dlConf.iInclude & OPTION_LANGPACKS);
        Globals::globalConfig.dlConf.bDLC = (Globals::globalConfig.dlConf.iInclude & OPTION_DLCS);
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

    if (Globals::globalConfig.dlConf.iInstallerPlatform < GlobalConstants::PLATFORMS[0].id || Globals::globalConfig.dlConf.iInstallerPlatform > Util::getOptionValue("all", GlobalConstants::PLATFORMS))
    {
        std::cerr << "Invalid value for --platform" << std::endl;
        return 1;
    }

    if (Globals::globalConfig.dlConf.iInstallerLanguage < GlobalConstants::LANGUAGES[0].id || Globals::globalConfig.dlConf.iInstallerLanguage > Util::getOptionValue("all", GlobalConstants::LANGUAGES))
    {
        std::cerr << "Invalid value for --language" << std::endl;
        return 1;
    }

    if (!Globals::globalConfig.sXMLDirectory.empty())
    {
        // Make sure that xml directory doesn't have trailing slash
        if (Globals::globalConfig.sXMLDirectory.at(Globals::globalConfig.sXMLDirectory.length()-1)=='/')
            Globals::globalConfig.sXMLDirectory.assign(Globals::globalConfig.sXMLDirectory.begin(), Globals::globalConfig.sXMLDirectory.end()-1);
    }

    // Create GOG XML for a file
    if (!Globals::globalConfig.sXMLFile.empty() && (Globals::globalConfig.sXMLFile != "automatic"))
    {
        Util::createXML(Globals::globalConfig.sXMLFile, Globals::globalConfig.iChunkSize, Globals::globalConfig.sXMLDirectory);
        return 0;
    }

    // Make sure that directory has trailing slash
    if (!Globals::globalConfig.dirConf.sDirectory.empty())
    {
        if (Globals::globalConfig.dirConf.sDirectory.at(Globals::globalConfig.dirConf.sDirectory.length()-1)!='/')
            Globals::globalConfig.dirConf.sDirectory += "/";
    }
    else
    {
        Globals::globalConfig.dirConf.sDirectory = "./"; // Directory wasn't specified, use current directory
    }

    // CA certificate bundle
    if (Globals::globalConfig.curlConf.sCACertPath.empty())
    {
        // Use CURL_CA_BUNDLE environment variable for CA certificate path if it is set
        char *ca_bundle = getenv("CURL_CA_BUNDLE");
        if (ca_bundle)
            Globals::globalConfig.curlConf.sCACertPath = (std::string)ca_bundle;
    }

    if (!unrecognized_options_cfg.empty() && (!Globals::globalConfig.bSaveConfig || !Globals::globalConfig.bResetConfig))
    {
        std::cerr << "Unrecognized options in " << Globals::globalConfig.sConfigFilePath << std::endl;
        for (unsigned int i = 0; i < unrecognized_options_cfg.size(); i+=2)
        {
            std::cerr << unrecognized_options_cfg[i] << " = " << unrecognized_options_cfg[i+1] << std::endl;
        }
        std::cerr << std::endl;
    }

    // Init curl globally
    ssl_thread_setup();
    curl_global_init(CURL_GLOBAL_ALL);

    if (Globals::globalConfig.bLoginAPI)
    {
        Globals::globalConfig.apiConf.sToken = "";
        Globals::globalConfig.apiConf.sSecret = "";
    }

    Downloader downloader;

    int iLoginTries = 0;
    bool bLoginOK = false;

    // Login because --login, --login-api or --login-website was used
    if (Globals::globalConfig.bLoginAPI || Globals::globalConfig.bLoginHTTP)
        bLoginOK = downloader.login();

    bool bIsLoggedin = downloader.isLoggedIn();

    // Login because we are not logged in
    while (iLoginTries++ < Globals::globalConfig.iRetries && !bIsLoggedin)
    {
        bLoginOK = downloader.login();
        if (bLoginOK)
        {
            bIsLoggedin = downloader.isLoggedIn();
        }
    }

    // Login failed, cleanup
    if (!bLoginOK && !bIsLoggedin)
    {
        curl_global_cleanup();
        ssl_thread_cleanup();
        return 1;
    }

    // Make sure that config file and cookie file are only readable/writable by owner
    if (!Globals::globalConfig.bRespectUmask)
    {
        Util::setFilePermissions(Globals::globalConfig.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
        Util::setFilePermissions(Globals::globalConfig.curlConf.sCookiePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
        Util::setFilePermissions(Globals::galaxyConf.getFilepath(), boost::filesystem::owner_read | boost::filesystem::owner_write);
    }

    if (Globals::globalConfig.bSaveConfig || bLoginOK)
    {
        if (bLoginOK)
        {
            set_vm_value(vm, "token", Globals::globalConfig.apiConf.sToken);
            set_vm_value(vm, "secret", Globals::globalConfig.apiConf.sSecret);
        }
        std::ofstream ofs(Globals::globalConfig.sConfigFilePath.c_str());
        if (ofs)
        {
            std::cerr << "Saving config: " << Globals::globalConfig.sConfigFilePath << std::endl;
            for (bpo::variables_map::iterator it = vm.begin(); it != vm.end(); ++it)
            {
                std::string option = it->first;
                std::string option_value_string;
                const bpo::variable_value& option_value = it->second;

                try
                {
                    if (options_cfg_all.find(option, false).long_name() == option)
                    {
                        if (!option_value.empty())
                        {
                            const std::type_info& type = option_value.value().type() ;
                            if ( type == typeid(std::string) )
                               option_value_string = option_value.as<std::string>();
                            else if ( type == typeid(int) )
                                 option_value_string = std::to_string(option_value.as<int>());
                            else if ( type == typeid(size_t) )
                                option_value_string = std::to_string(option_value.as<size_t>());
                            else if ( type == typeid(unsigned int) )
                                option_value_string = std::to_string(option_value.as<unsigned int>());
                            else if ( type == typeid(long int) )
                                option_value_string = std::to_string(option_value.as<long int>());
                            else if ( type == typeid(bool) )
                            {
                                if (option_value.as<bool>() == true)
                                    option_value_string = "true";
                                else
                                    option_value_string = "false";
                            }
                        }
                    }
                }
                catch (...)
                {
                    continue;
                }

                if (!option_value_string.empty())
                {
                    ofs << option << " = " << option_value_string << std::endl;
                }
            }
            ofs.close();
            if (!Globals::globalConfig.bRespectUmask)
                Util::setFilePermissions(Globals::globalConfig.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
            if (Globals::globalConfig.bSaveConfig)
            {
                curl_global_cleanup();
                ssl_thread_cleanup();
                return 0;
            }
        }
        else
        {
            std::cerr << "Failed to create config: " << Globals::globalConfig.sConfigFilePath << std::endl;
            curl_global_cleanup();
            ssl_thread_cleanup();
            return 1;
        }
    }
    else if (Globals::globalConfig.bResetConfig)
    {
        std::ofstream ofs(Globals::globalConfig.sConfigFilePath.c_str());
        if (ofs)
        {
            if (!Globals::globalConfig.apiConf.sToken.empty() && !Globals::globalConfig.apiConf.sSecret.empty())
            {
                ofs << "token = " << Globals::globalConfig.apiConf.sToken << std::endl;
                ofs << "secret = " << Globals::globalConfig.apiConf.sSecret << std::endl;
            }
            ofs.close();
            if (!Globals::globalConfig.bRespectUmask)
                Util::setFilePermissions(Globals::globalConfig.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);

            curl_global_cleanup();
            ssl_thread_cleanup();
            return 0;
        }
        else
        {
            std::cerr << "Failed to create config: " << Globals::globalConfig.sConfigFilePath << std::endl;
            curl_global_cleanup();
            ssl_thread_cleanup();
            return 1;
        }
    }

    bool bInitOK = downloader.init();
    if (!bInitOK)
    {
        curl_global_cleanup();
        ssl_thread_cleanup();
        return 1;
    }

    int res = 0;

    if (Globals::globalConfig.bShowWishlist)
        downloader.showWishlist();
    else if (Globals::globalConfig.bUpdateCache)
        downloader.updateCache();
    else if (Globals::globalConfig.bUpdateCheck) // Update check has priority over download and list
        downloader.updateCheck();
    else if (!vFileIdStrings.empty())
    {
        for (std::vector<std::string>::iterator it = vFileIdStrings.begin(); it != vFileIdStrings.end(); it++)
        {
            res |= downloader.downloadFileWithId(*it, Globals::globalConfig.sOutputFilename) ? 1 : 0;
        }
    }
    else if (Globals::globalConfig.bRepair) // Repair file
        downloader.repair();
    else if (Globals::globalConfig.bDownload) // Download games
        downloader.download();
    else if (Globals::globalConfig.bListDetails || Globals::globalConfig.bList) // Detailed list of games/extras
        res = downloader.listGames();
    else if (!Globals::globalConfig.sOrphanRegex.empty()) // Check for orphaned files if regex for orphans is set
        downloader.checkOrphans();
    else if (Globals::globalConfig.bCheckStatus)
        downloader.checkStatus();
    else if (!galaxy_product_id_show_builds.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_id_show_builds, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.galaxyShowBuilds(product_id, build_index);
    }
    else if (!galaxy_product_id_install.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_id_install, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.galaxyInstallGame(product_id, build_index, Globals::globalConfig.dlConf.iGalaxyArch);
    }
    else
    {
        if (!(Globals::globalConfig.bLoginAPI || Globals::globalConfig.bLoginHTTP))
        {
            // Show help message
            std::cerr   << Globals::globalConfig.sVersionString << std::endl
                        << options_cli_all << std::endl;
        }
    }

    // Orphan check was called at the same time as download. Perform it after download has finished
    if (!Globals::globalConfig.sOrphanRegex.empty() && Globals::globalConfig.bDownload)
        downloader.checkOrphans();

    curl_global_cleanup();
    ssl_thread_cleanup();

    return res;
}
