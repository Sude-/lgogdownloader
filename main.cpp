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

#define VERSION_NUMBER "2.20"

#ifndef VERSION_STRING
#   define VERSION_STRING "LGOGDownloader " VERSION_NUMBER
#endif

namespace bpo = boost::program_options;

template<typename T> void set_vm_value(std::map<std::string, bpo::variable_value>& vm, const std::string& option, const T& value)
{
    vm[option].value() = boost::any(value);
}

// Parse the priority string, making it an array of numeric codes, and override the ORed type if required
void handle_priority(const std::string &what, const std::string &priority_string, std::vector<unsigned int> &priority, unsigned int &type)
{
    size_t idx = 0, found;

    while ((found = priority_string.find(',', idx)) != std::string::npos)
	{
	    priority.push_back(std::stoi(priority_string.substr(idx, found - idx)));
	    idx = found + 1;
	}
    priority.push_back(std::stoi(priority_string.substr(idx)));

    unsigned int wanted = 0;
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (handle_priority): for " << what << " found ";
    #endif
    for (std::vector<unsigned int>::iterator it = priority.begin(); it != priority.end(); it++)
	{
	    wanted += *it;
            #ifdef DEBUG
  	      std::cerr << *it << " ";
            #endif
	}
    #ifdef DEBUG
        std::cerr << std::endl;
    #endif

    if (wanted != type)
	{
            type = wanted;
	    std::cout << "Warning: for " << what << " the priority string doesn't match the enabled installers, forcing enabled installers to " << type << std::endl;
	}
}

int main(int argc, char *argv[])
{
    Config config;
    config.sVersionString = VERSION_STRING;
    char *xdgconfig = getenv("XDG_CONFIG_HOME");
    char *xdgcache = getenv("XDG_CACHE_HOME");
    std::string home = (std::string)getenv("HOME");

    if (xdgcache)
        config.sCacheDirectory = (std::string)xdgcache + "/lgogdownloader";
    else
        config.sCacheDirectory = home + "/.cache/lgogdownloader";

    config.sXMLDirectory = config.sCacheDirectory + "/xml";

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

    // Create help text for --check-orphans
    std::string orphans_regex_default = ".*\\.(zip|exe|bin|dmg|old|deb|tar\\.gz|pkg)$"; // Limit to files with these extensions (".old" is for renamed older version files)
    std::string check_orphans_text = "Check for orphaned files (files found on local filesystem that are not found on GOG servers). Sets regular expression filter (Perl syntax) for files to check. If no argument is given then the regex defaults to '" + orphans_regex_default + "'";

    // Help text for subdir options
    std::string subdir_help_text = "\nTemplates:\n- %platform%\n- %gamename%\n- %dlcname%";
    // Help text for priority options
    std::string priority_help_text = "\nIf set, only the first matching one will be downloaded. If unset, all matching combinations will be downloaded.\nSyntax: use a string separated by \",\"";

    std::vector<std::string> unrecognized_options_cfg;
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
        bool bNoInstallers = false;
        bool bNoExtras = false;
        bool bNoPatches = false;
        bool bNoLanguagePacks = false;
        bool bNoDLC = false;
        bool bNoRemoteXML = false;
        bool bNoSubDirectories = false;
        bool bNoDeb = false;
        bool bNoTarGz = false;
        bool bNoCover = false;
        config.bReport = false;
        // Commandline options (no config file)
        options_cli_no_cfg.add_options()
            ("help,h", "Print help message")
            ("version", "Print version information")
            ("login", bpo::value<bool>(&config.bLogin)->zero_tokens()->default_value(false), "Login")
            ("list", bpo::value<bool>(&config.bList)->zero_tokens()->default_value(false), "List games")
            ("list-details", bpo::value<bool>(&config.bListDetails)->zero_tokens()->default_value(false), "List games with detailed info")
            ("download", bpo::value<bool>(&config.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&config.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files\nUse --repair --download to redownload files when filesizes don't match (possibly different version). Redownload will rename the old file (appends .old to filename)")
            ("game", bpo::value<std::string>(&config.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)\nAliases: \"all\", \"free\"\nAlias \"free\" doesn't work with cached details")
            ("create-xml", bpo::value<std::string>(&config.sXMLFile)->default_value(""), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("update-check", bpo::value<bool>(&config.bUpdateCheck)->zero_tokens()->default_value(false), "Check for update notifications")
            ("check-orphans", bpo::value<std::string>(&config.sOrphanRegex)->implicit_value(""), check_orphans_text.c_str())
            ("status", bpo::value<bool>(&config.bCheckStatus)->zero_tokens()->default_value(false), "Show status of files\n\nOutput format:\nstatuscode gamename filename filesize filehash\n\nStatus codes:\nOK - File is OK\nND - File is not downloaded\nMD5 - MD5 mismatch, different version")
            ("save-config", bpo::value<bool>(&config.bSaveConfig)->zero_tokens()->default_value(false), "Create config file with current settings")
            ("reset-config", bpo::value<bool>(&config.bResetConfig)->zero_tokens()->default_value(false), "Reset config settings to default")
            ("report", bpo::value<std::string>(&config.sReportFilePath)->implicit_value("lgogdownloader-report.log"), "Save report of downloaded/repaired files to specified file\nDefault filename: lgogdownloader-report.log")
            ("no-cover", bpo::value<bool>(&bNoCover)->zero_tokens()->default_value(false), "Don't download cover images. Overrides --cover option.\nUseful for making exceptions when \"cover\" is set to true in config file.")
            ("update-cache", bpo::value<bool>(&config.bUpdateCache)->zero_tokens()->default_value(false), "Update game details cache")
        ;
        // Commandline options (config file)
        options_cli_cfg.add_options()
            ("directory", bpo::value<std::string>(&config.sDirectory)->default_value("."), "Set download directory")
            ("limit-rate", bpo::value<curl_off_t>(&config.iDownloadRate)->default_value(0), "Limit download rate to value in kB\n0 = unlimited")
            ("xml-directory", bpo::value<std::string>(&config.sXMLDirectory), "Set directory for GOG XML files")
            ("chunk-size", bpo::value<size_t>(&config.iChunkSize)->default_value(10), "Chunk size (in MB) when creating XML")
            ("platform", bpo::value<unsigned int>(&config.iInstallerType)->default_value(GlobalConstants::PLATFORM_WINDOWS|GlobalConstants::PLATFORM_LINUX), platform_text.c_str())
            ("language", bpo::value<unsigned int>(&config.iInstallerLanguage)->default_value(GlobalConstants::LANGUAGE_EN), language_text.c_str())
            ("no-installers", bpo::value<bool>(&bNoInstallers)->zero_tokens()->default_value(false), "Don't download/list/repair installers")
            ("no-extras", bpo::value<bool>(&bNoExtras)->zero_tokens()->default_value(false), "Don't download/list/repair extras")
            ("no-patches", bpo::value<bool>(&bNoPatches)->zero_tokens()->default_value(false), "Don't download/list/repair patches")
            ("no-language-packs", bpo::value<bool>(&bNoLanguagePacks)->zero_tokens()->default_value(false), "Don't download/list/repair language packs")
            ("no-dlc", bpo::value<bool>(&bNoDLC)->zero_tokens()->default_value(false), "Don't download/list/repair DLCs")
            ("no-deb", bpo::value<bool>(&bNoDeb)->zero_tokens()->default_value(false), "Don't download/list/repair deb packages")
            ("no-targz", bpo::value<bool>(&bNoTarGz)->zero_tokens()->default_value(false), "Don't download/list/repair tarballs")
            ("cover", bpo::value<bool>(&config.bCover)->zero_tokens()->default_value(false), "Download cover images")
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
            ("language-priority", bpo::value<std::string>(&config.sLanguagePriority)->default_value(""), ("Set priority of systems" + priority_help_text + ", like \"4,1\" for French first, then English if no French version").c_str())
            ("platform-priority", bpo::value<std::string>(&config.sPlatformPriority)->default_value(""), ("Set priority of platforms" + priority_help_text + ", like \"4,1\" for Linux first, then Windows if no Linux version").c_str())

        ;
        // Options read from config file
        options_cfg_only.add_options()
            ("token", bpo::value<std::string>(&config.sToken)->default_value(""), "oauth token")
            ("secret", bpo::value<std::string>(&config.sSecret)->default_value(""), "oauth secret")
        ;

        options_cli_all.add(options_cli_no_cfg).add(options_cli_cfg);
        options_cfg_all.add(options_cfg_only).add(options_cli_cfg);

        bpo::store(bpo::parse_command_line(argc, argv, options_cli_all), vm);
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

        if (xdgconfig)
        {
            config.sConfigDirectory = (std::string)xdgconfig + "/lgogdownloader";
        }
        else
        {
            config.sConfigDirectory = home + "/.config/lgogdownloader";
        }

        config.sCookiePath = config.sConfigDirectory + "/cookies.txt";
        config.sConfigFilePath = config.sConfigDirectory + "/config.cfg";
        config.sBlacklistFilePath = config.sConfigDirectory + "/blacklist.txt";

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
                if (bNoDeb)
                    lines.push_back("Rp .*\\.deb$");
                if (bNoTarGz)
                    lines.push_back("Rp .*\\.tar\\.gz$");
                config.blacklist.initialize(lines);
            }
        }
        else if (bNoDeb || bNoTarGz)
        {
            std::vector<std::string> lines;
            if (bNoDeb)
                lines.push_back("Rp .*\\.deb$");
            if (bNoTarGz)
                lines.push_back("Rp .*\\.tar\\.gz$");
            config.blacklist.initialize(lines);
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
        config.bInstallers = !bNoInstallers;
        config.bExtras = !bNoExtras;
        config.bPatches = !bNoPatches;
        config.bLanguagePacks = !bNoLanguagePacks;
        config.bDLC = !bNoDLC;
        config.bRemoteXML = !bNoRemoteXML;
        config.bSubDirectories = !bNoSubDirectories;

        // Override cover option
        if (bNoCover)
            config.bCover = false;

	// Handle priority business
	if (!config.sLanguagePriority.empty())
	    handle_priority("languages", config.sLanguagePriority, config.vLanguagePriority, config.iInstallerLanguage);
	if (!config.sPlatformPriority.empty())
	    handle_priority("platforms", config.sPlatformPriority, config.vPlatformPriority, config.iInstallerType);

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
    if (config.bLogin || initResult == 1)
    {
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
    else if (config.bUpdateCache)
        downloader.updateCache();
    else if (config.bUpdateCheck) // Update check has priority over download and list
        downloader.updateCheck();
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
