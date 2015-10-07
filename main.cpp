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

#define VERSION_NUMBER "2.25"

#ifndef VERSION_STRING
#   define VERSION_STRING "LGOGDownloader " VERSION_NUMBER
#endif

namespace bpo = boost::program_options;

template<typename T> void set_vm_value(std::map<std::string, bpo::variable_value>& vm, const std::string& option, const T& value)
{
    vm[option].value() = boost::any(value);
}

// Parse the options string
void parseOptionString(const std::string &option_string, std::vector<unsigned int> &priority, unsigned int &type, const std::vector<GlobalConstants::optionsStruct>& options)
{
    type = 0;
    std::vector<std::string> tokens_priority = Util::tokenize(option_string, ",");
    for (std::vector<std::string>::iterator it_priority = tokens_priority.begin(); it_priority != tokens_priority.end(); it_priority++)
    {
        unsigned int value = 0;
        std::vector<std::string> tokens_value = Util::tokenize(*it_priority, "+");
        for (std::vector<std::string>::iterator it_value = tokens_value.begin(); it_value != tokens_value.end(); it_value++)
        {
            value |= Util::getOptionValue(*it_value, options);
        }
        priority.push_back(value);
        type |= value;
    }
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
    const unsigned int OPTION_COVERS     = 1 << 4;
    const unsigned int OPTION_DLCS       = 1 << 5;

    const std::vector<GlobalConstants::optionsStruct> INCLUDE_OPTIONS =
    {
        { OPTION_INSTALLERS, "i", "Installers",     "i|installers"              },
        { OPTION_EXTRAS,     "e", "Extras",         "e|extras"                  },
        { OPTION_PATCHES,    "p", "Patches",        "p|patches"                 },
        { OPTION_LANGPACKS,  "l", "Language packs", "l|languagepacks|langpacks" },
        { OPTION_COVERS,     "c", "Covers",         "c|cover|covers"            },
        { OPTION_DLCS,       "d", "DLCs",           "d|dlc|dlcs"                }
    };

    Config config;
    config.sVersionString = VERSION_STRING;
    config.sVersionNumber = VERSION_NUMBER;

    config.sCacheDirectory = Util::getCacheHome() + "/lgogdownloader";
    config.sXMLDirectory = config.sCacheDirectory + "/xml";

    config.sConfigDirectory = Util::getConfigHome() + "/lgogdownloader";
    config.sCookiePath = config.sConfigDirectory + "/cookies.txt";
    config.sConfigFilePath = config.sConfigDirectory + "/config.cfg";
    config.sBlacklistFilePath = config.sConfigDirectory + "/blacklist.txt";

    std::string priority_help_text = "Set priority by separating values with \",\"\nCombine values by separating with \"+\"";
    // Create help text for --platform option
    std::string platform_text = "Select which installers are downloaded\n";
    unsigned int platform_all = Util::getOptionValue("all", GlobalConstants::PLATFORMS);
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        platform_text += GlobalConstants::PLATFORMS[i].str + " = " + GlobalConstants::PLATFORMS[i].regexp + "|" + std::to_string(GlobalConstants::PLATFORMS[i].id) + "\n";
    }
    platform_text += "All = all|" + std::to_string(platform_all);
    platform_text += "\n\n" + priority_help_text;
    platform_text += "\nExample: Linux if available otherwise Windows and Mac: l,w+m";

    // Create help text for --language option
    std::string language_text = "Select which language installers are downloaded\n";
    unsigned int language_all = Util::getOptionValue("all", GlobalConstants::LANGUAGES);
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        language_text +=  GlobalConstants::LANGUAGES[i].str + " = " + GlobalConstants::LANGUAGES[i].regexp + "|" + std::to_string(GlobalConstants::LANGUAGES[i].id) + "\n";
    }
    language_text += "Add the values to download multiple languages\nAll = all|" + std::to_string(language_all) + "\n"
                    + "French + Polish = \"fr+pl\"|" + std::to_string(GlobalConstants::LANGUAGE_FR | GlobalConstants::LANGUAGE_PL) + " (" + std::to_string(GlobalConstants::LANGUAGE_FR) + "+" + std::to_string(GlobalConstants::LANGUAGE_PL) + "=" + std::to_string(GlobalConstants::LANGUAGE_FR | GlobalConstants::LANGUAGE_PL) + ")";
    language_text += "\n\n" + priority_help_text;
    language_text += "\nExample: German if available otherwise English and French: de,en+fr";

    // Create help text for --check-orphans
    std::string orphans_regex_default = ".*\\.(zip|exe|bin|dmg|old|deb|tar\\.gz|pkg|sh)$"; // Limit to files with these extensions (".old" is for renamed older version files)
    std::string check_orphans_text = "Check for orphaned files (files found on local filesystem that are not found on GOG servers). Sets regular expression filter (Perl syntax) for files to check. If no argument is given then the regex defaults to '" + orphans_regex_default + "'";

    // Help text for subdir options
    std::string subdir_help_text = "\nTemplates:\n- %platform%\n- %gamename%\n- %dlcname%";

    // Help text for include and exclude options
    std::string include_options_text;
    for (unsigned int i = 0; i < INCLUDE_OPTIONS.size(); ++i)
    {
        include_options_text +=  INCLUDE_OPTIONS[i].str + " = " + INCLUDE_OPTIONS[i].regexp + "|" + std::to_string(INCLUDE_OPTIONS[i].id) + "\n";
    }
    include_options_text += "Separate with \",\" to use multiple values";

    std::vector<std::string> vFileIdStrings;
    std::vector<std::string> unrecognized_options_cfg;
    std::vector<std::string> unrecognized_options_cli;
    bpo::variables_map vm;
    bpo::options_description options_cli_all("Options");
    bpo::options_description options_cli_no_cfg;
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
        config.bReport = false;
        // Commandline options (no config file)
        options_cli_no_cfg.add_options()
            ("help,h", "Print help message")
            ("version", "Print version information")
            ("login", bpo::value<bool>(&bLogin)->zero_tokens()->default_value(false), "Login")
            ("list", bpo::value<bool>(&config.bList)->zero_tokens()->default_value(false), "List games")
            ("list-details", bpo::value<bool>(&config.bListDetails)->zero_tokens()->default_value(false), "List games with detailed info")
            ("download", bpo::value<bool>(&config.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&config.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files\nUse --repair --download to redownload files when filesizes don't match (possibly different version). Redownload will rename the old file (appends .old to filename)")
            ("game", bpo::value<std::string>(&config.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)\nAliases: \"all\", \"free\"\nAlias \"free\" doesn't work with cached details")
            ("create-xml", bpo::value<std::string>(&config.sXMLFile)->implicit_value("automatic"), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("update-check", bpo::value<bool>(&config.bUpdateCheck)->zero_tokens()->default_value(false), "Check for update notifications")
            ("check-orphans", bpo::value<std::string>(&config.sOrphanRegex)->implicit_value(""), check_orphans_text.c_str())
            ("status", bpo::value<bool>(&config.bCheckStatus)->zero_tokens()->default_value(false), "Show status of files\n\nOutput format:\nstatuscode gamename filename filesize filehash\n\nStatus codes:\nOK - File is OK\nND - File is not downloaded\nMD5 - MD5 mismatch, different version\nFS - File size mismatch, incomplete download")
            ("save-config", bpo::value<bool>(&config.bSaveConfig)->zero_tokens()->default_value(false), "Create config file with current settings")
            ("reset-config", bpo::value<bool>(&config.bResetConfig)->zero_tokens()->default_value(false), "Reset config settings to default")
            ("report", bpo::value<std::string>(&config.sReportFilePath)->implicit_value("lgogdownloader-report.log"), "Save report of downloaded/repaired files to specified file\nDefault filename: lgogdownloader-report.log")
            ("update-cache", bpo::value<bool>(&config.bUpdateCache)->zero_tokens()->default_value(false), "Update game details cache")
            ("no-platform-detection", bpo::value<bool>(&bNoPlatformDetection)->zero_tokens()->default_value(false), "Don't try to detect supported platforms from game shelf.\nSkips the initial fast platform detection and detects the supported platforms from game details which is slower but more accurate.\nUseful in case platform identifier is missing for some games in the game shelf.\nUsing --platform with --list doesn't work with this option.")
            ("download-file", bpo::value<std::string>(&config.sFileIdString)->default_value(""), "Download files using fileid\n\nFormat:\n\"gamename/fileid\"\nor: \"gogdownloader://gamename/fileid\"\n\nMultiple files:\n\"gamename1/fileid1,gamename2/fileid2\"\nor: \"gogdownloader://gamename1/fileid1,gamename2/fileid2\"\n\nThis option ignores all subdir options. The files are downloaded to directory specified with --directory option.")
            ("output-file,o", bpo::value<std::string>(&config.sOutputFilename)->default_value(""), "Set filename of file downloaded with --download-file.")
            ("wishlist", bpo::value<bool>(&config.bShowWishlist)->zero_tokens()->default_value(false), "Show wishlist")
            ("login-api", bpo::value<bool>(&config.bLoginAPI)->zero_tokens()->default_value(false), "Login (API only)")
            ("login-website", bpo::value<bool>(&config.bLoginHTTP)->zero_tokens()->default_value(false), "Login (website only)")
        ;
        // Commandline options (config file)
        options_cli_cfg.add_options()
            ("directory", bpo::value<std::string>(&config.sDirectory)->default_value("."), "Set download directory")
            ("limit-rate", bpo::value<curl_off_t>(&config.iDownloadRate)->default_value(0), "Limit download rate to value in kB\n0 = unlimited")
            ("xml-directory", bpo::value<std::string>(&config.sXMLDirectory), "Set directory for GOG XML files")
            ("chunk-size", bpo::value<size_t>(&config.iChunkSize)->default_value(10), "Chunk size (in MB) when creating XML")
            ("platform", bpo::value<std::string>(&sInstallerPlatform)->default_value("w+l"), platform_text.c_str())
            ("language", bpo::value<std::string>(&sInstallerLanguage)->default_value("en"), language_text.c_str())
            ("no-remote-xml", bpo::value<bool>(&bNoRemoteXML)->zero_tokens()->default_value(false), "Don't use remote XML for repair")
            ("no-unicode", bpo::value<bool>(&bNoUnicode)->zero_tokens()->default_value(false), "Don't use Unicode in the progress bar")
            ("no-color", bpo::value<bool>(&bNoColor)->zero_tokens()->default_value(false), "Don't use coloring in the progress bar")
            ("no-duplicate-handling", bpo::value<bool>(&bNoDuplicateHandler)->zero_tokens()->default_value(false), "Don't use duplicate handler for installers\nDuplicate installers from different languages are handled separately")
            ("no-subdirectories", bpo::value<bool>(&bNoSubDirectories)->zero_tokens()->default_value(false), "Don't create subdirectories for extras, patches and language packs")
            ("verbose", bpo::value<bool>(&config.bVerbose)->zero_tokens()->default_value(false), "Print lots of information")
            ("insecure", bpo::value<bool>(&bInsecure)->zero_tokens()->default_value(false), "Don't verify authenticity of SSL certificates")
            ("timeout", bpo::value<long int>(&config.iTimeout)->default_value(10), "Set timeout for connection\nMaximum time in seconds that connection phase is allowed to take")
            ("retries", bpo::value<int>(&config.iRetries)->default_value(3), "Set maximum number of retries on failed download")
            ("wait", bpo::value<int>(&config.iWait)->default_value(0), "Time to wait between requests (milliseconds)")
            ("cover-list", bpo::value<std::string>(&config.sCoverList)->default_value("https://sites.google.com/site/gogdownloader/covers.xml"), "Set URL for cover list")
            ("subdir-installers", bpo::value<std::string>(&config.sInstallersSubdir)->default_value(""), ("Set subdirectory for extras" + subdir_help_text).c_str())
            ("subdir-extras", bpo::value<std::string>(&config.sExtrasSubdir)->default_value("extras"), ("Set subdirectory for extras" + subdir_help_text).c_str())
            ("subdir-patches", bpo::value<std::string>(&config.sPatchesSubdir)->default_value("patches"), ("Set subdirectory for patches" + subdir_help_text).c_str())
            ("subdir-language-packs", bpo::value<std::string>(&config.sLanguagePackSubdir)->default_value("languagepacks"), ("Set subdirectory for language packs" + subdir_help_text).c_str())
            ("subdir-dlc", bpo::value<std::string>(&config.sDLCSubdir)->default_value("dlc/%dlcname%"), ("Set subdirectory for dlc" + subdir_help_text).c_str())
            ("subdir-game", bpo::value<std::string>(&config.sGameSubdir)->default_value("%gamename%"), ("Set subdirectory for game" + subdir_help_text).c_str())
            ("use-cache", bpo::value<bool>(&config.bUseCache)->zero_tokens()->default_value(false), ("Use game details cache"))
            ("cache-valid", bpo::value<int>(&config.iCacheValid)->default_value(2880), ("Set how long cached game details are valid (in minutes)\nDefault: 2880 minutes (48 hours)"))
            ("save-serials", bpo::value<bool>(&config.bSaveSerials)->zero_tokens()->default_value(false), "Save serial numbers when downloading")
            ("ignore-dlc-count", bpo::value<std::string>(&config.sIgnoreDLCCountRegex)->implicit_value(".*"), "Set regular expression filter for games to ignore DLC count information\nIgnoring DLC count information helps in situations where the account page doesn't provide accurate information about DLCs")
            ("include", bpo::value<std::string>(&sIncludeOptions)->default_value("all"), ("Select what to download/list/repair\n" + include_options_text).c_str())
            ("exclude", bpo::value<std::string>(&sExcludeOptions)->default_value("covers"), ("Select what not to download/list/repair\n" + include_options_text).c_str())
        ;
        // Options read from config file
        options_cfg_only.add_options()
            ("token", bpo::value<std::string>(&config.sToken)->default_value(""), "oauth token")
            ("secret", bpo::value<std::string>(&config.sSecret)->default_value(""), "oauth secret")
        ;

        options_cli_all.add(options_cli_no_cfg).add(options_cli_cfg);
        options_cfg_all.add(options_cfg_only).add(options_cli_cfg);

        bpo::parsed_options parsed = bpo::parse_command_line(argc, argv, options_cli_all);
        bpo::store(parsed, vm);
        unrecognized_options_cli = bpo::collect_unrecognized(parsed.options, bpo::include_positional);
        bpo::notify(vm);

        if (vm.count("help"))
        {
            std::cout   << config.sVersionString << std::endl
                        << options_cli_all << std::endl;
            return 0;
        }

        if (vm.count("version"))
        {
            std::cout << VERSION_STRING << std::endl;
            return 0;
        }

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

        path = config.sCacheDirectory;
        if (!boost::filesystem::exists(path))
        {
            if (!boost::filesystem::create_directories(path))
            {
                std::cout << "Failed to create directory: " << path << std::endl;
                return 1;
            }
        }

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
                bpo::parsed_options parsed = bpo::parse_config_file(ifs, options_cfg_all, true);
                bpo::store(parsed, vm);
                bpo::notify(vm);
                ifs.close();
                unrecognized_options_cfg = bpo::collect_unrecognized(parsed.options, bpo::include_positional);
            }
        }
        if (boost::filesystem::exists(config.sBlacklistFilePath))
        {
            std::ifstream ifs(config.sBlacklistFilePath.c_str());
            if (!ifs)
            {
                std::cout << "Could not open blacklist file: " << config.sBlacklistFilePath << std::endl;
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
                config.blacklist.initialize(lines);
            }
        }

        if (vm.count("chunk-size"))
            config.iChunkSize <<= 20; // Convert chunk size from bytes to megabytes

        if (vm.count("limit-rate"))
            config.iDownloadRate <<= 10; // Convert download rate from bytes to kilobytes

        if (vm.count("check-orphans"))
            if (config.sOrphanRegex.empty())
                config.sOrphanRegex = orphans_regex_default;

        if (vm.count("report"))
            config.bReport = true;

        if (config.iWait > 0)
            config.iWait *= 1000;

        config.bVerifyPeer = !bInsecure;
        config.bColor = !bNoColor;
        config.bUnicode = !bNoUnicode;
        config.bDuplicateHandler = !bNoDuplicateHandler;
        config.bRemoteXML = !bNoRemoteXML;
        config.bSubDirectories = !bNoSubDirectories;
        config.bPlatformDetection = !bNoPlatformDetection;

        for (auto i = unrecognized_options_cli.begin(); i != unrecognized_options_cli.end(); ++i)
            if (i->compare(0, GlobalConstants::PROTOCOL_PREFIX.length(), GlobalConstants::PROTOCOL_PREFIX) == 0)
                config.sFileIdString = *i;

        if (!config.sFileIdString.empty())
        {
            if (config.sFileIdString.compare(0, GlobalConstants::PROTOCOL_PREFIX.length(), GlobalConstants::PROTOCOL_PREFIX) == 0)
            {
                config.sFileIdString.replace(0, GlobalConstants::PROTOCOL_PREFIX.length(), "");
            }
            vFileIdStrings = Util::tokenize(config.sFileIdString, ",");
        }

        if (!config.sOutputFilename.empty() && vFileIdStrings.size() > 1)
        {
            std::cerr << "Cannot specify an output file name when downloading multiple files." << std::endl;
            return 1;
        }

        if (bLogin)
        {
            config.bLoginAPI = true;
            config.bLoginHTTP = true;
        }

        parseOptionString(sInstallerLanguage, config.vLanguagePriority, config.iInstallerLanguage, GlobalConstants::LANGUAGES);
        parseOptionString(sInstallerPlatform, config.vPlatformPriority, config.iInstallerPlatform, GlobalConstants::PLATFORMS);

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
        config.iInclude = include_value & ~exclude_value;

        // Assign values
        // TODO: Use config.iInclude in Downloader class directly and get rid of this value assignment
        config.bCover = (config.iInclude & OPTION_COVERS);
        config.bInstallers = (config.iInclude & OPTION_INSTALLERS);
        config.bExtras = (config.iInclude & OPTION_EXTRAS);
        config.bPatches = (config.iInclude & OPTION_PATCHES);
        config.bLanguagePacks = (config.iInclude & OPTION_LANGPACKS);
        config.bDLC = (config.iInclude & OPTION_DLCS);
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

    if (config.iInstallerPlatform < GlobalConstants::PLATFORMS[0].id || config.iInstallerPlatform > platform_all)
    {
        std::cout << "Invalid value for --platform" << std::endl;
        return 1;
    }

    if (config.iInstallerLanguage < GlobalConstants::LANGUAGES[0].id || config.iInstallerLanguage > language_all)
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
    {
        if (config.sDirectory.at(config.sDirectory.length()-1)!='/')
            config.sDirectory += "/";
    }
    else
    {
        config.sDirectory = "./"; // Directory wasn't specified, use current directory
    }

    if (!unrecognized_options_cfg.empty() && (!config.bSaveConfig || !config.bResetConfig))
    {
        std::cerr << "Unrecognized options in " << config.sConfigFilePath << std::endl;
        for (unsigned int i = 0; i < unrecognized_options_cfg.size(); i+=2)
        {
            std::cerr << unrecognized_options_cfg[i] << " = " << unrecognized_options_cfg[i+1] << std::endl;
        }
        std::cerr << std::endl;
    }

    Downloader downloader(config);
    int initResult = downloader.init();

    int iLoginResult = 0;
    if (config.bLoginAPI || config.bLoginHTTP || initResult == 1)
    {
        if (!config.bLoginAPI && !config.bLoginHTTP)
            downloader.config.bLoginAPI = true;
        iLoginResult = downloader.login();
        if (iLoginResult == 0)
            return 1;
    }

    // Make sure that config file and cookie file are only readable/writable by owner
    Util::setFilePermissions(config.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
    Util::setFilePermissions(config.sCookiePath, boost::filesystem::owner_read | boost::filesystem::owner_write);

    if (config.bSaveConfig || iLoginResult == 1)
    {
        if (iLoginResult == 1)
        {
            set_vm_value(vm, "token", downloader.config.sToken);
            set_vm_value(vm, "secret", downloader.config.sSecret);
            bpo::notify(vm);
        }
        std::ofstream ofs(config.sConfigFilePath.c_str());
        if (ofs)
        {
            std::cout << "Saving config: " << config.sConfigFilePath << std::endl;
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
            Util::setFilePermissions(config.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
            if (config.bSaveConfig)
                return 0;
        }
        else
        {
            std::cout << "Failed to create config: " << config.sConfigFilePath << std::endl;
            return 1;
        }
    }
    else if (config.bResetConfig)
    {
        std::ofstream ofs(config.sConfigFilePath.c_str());
        if (ofs)
        {
            if (!config.sToken.empty() && !config.sSecret.empty())
            {
                ofs << "token = " << config.sToken << std::endl;
                ofs << "secret = " << config.sSecret << std::endl;
            }
            ofs.close();
            Util::setFilePermissions(config.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
            return 0;
        }
        else
        {
            std::cout << "Failed to create config: " << config.sConfigFilePath << std::endl;
            return 1;
        }
    }
    if (config.bShowWishlist)
        downloader.showWishlist();
    else if (config.bUpdateCache)
        downloader.updateCache();
    else if (config.bUpdateCheck) // Update check has priority over download and list
        downloader.updateCheck();
    else if (!vFileIdStrings.empty())
    {
        for (std::vector<std::string>::iterator it = vFileIdStrings.begin(); it != vFileIdStrings.end(); it++)
        {
            downloader.downloadFileWithId(*it, config.sOutputFilename);
        }
    }
    else if (config.bRepair) // Repair file
        downloader.repair();
    else if (config.bDownload) // Download games
        downloader.download();
    else if (config.bListDetails || config.bList) // Detailed list of games/extras
        downloader.listGames();
    else if (!config.sOrphanRegex.empty()) // Check for orphaned files if regex for orphans is set
        downloader.checkOrphans();
    else if (config.bCheckStatus)
        downloader.checkStatus();
    else
    {   // Show help message
        std::cout   << config.sVersionString << std::endl
                    << options_cli_all << std::endl;
    }

    // Orphan check was called at the same time as download. Perform it after download has finished
    if (!config.sOrphanRegex.empty() && config.bDownload)
        downloader.checkOrphans();

    return 0;
}
