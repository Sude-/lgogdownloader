/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

#include "downloader.h"
#include "config.h"
#include "util.h"

#include <unistd.h> // getpass
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <tinyxml.h>

#if __GNUC__
#   if __x86_64__ || __ppc64__ || __LP64__
#       define ENVIRONMENT64
#   else
#       define ENVIRONMENT32
#   endif
#endif

namespace bpo = boost::program_options;

int main(int argc, char *argv[])
{
    Config config;
    config.sVersionString = "LGOGDownloader 2.2+git";
    config.sHome = (std::string)getenv("HOME");
    config.sCookiePath = config.sHome + "/.gogdownloader/cookies.txt";
    config.sConfigFilePath = config.sHome + "/.gogdownloader/config.cfg";
    config.sXMLDirectory = config.sHome + "/.gogdownloader/xml";

    // Create gogdownloader directories
    boost::filesystem::path path = config.sXMLDirectory;
    if (!boost::filesystem::exists(path))
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return 1;
        }
    }

    // Create help text for --platform option
    std::string platform_text = "Select which installers are downloaded\n"
                                 + std::to_string(INSTALLER_WINDOWS) + " = Windows\n"
                                 + std::to_string(INSTALLER_MAC) + " = Mac\n"
                                 + std::to_string(INSTALLER_WINDOWS | INSTALLER_MAC) + " = Both";
    // Create help text for --language option
    std::string language_text = "Select which language installers are downloaded\n"
                                 + std::to_string(LANGUAGE_EN) + " = English\n"
                                 + std::to_string(LANGUAGE_DE) + " = German\n"
                                 + std::to_string(LANGUAGE_FR) + " = French\n"
                                 + std::to_string(LANGUAGE_PL) + " = Polish\n"
                                 + std::to_string(LANGUAGE_RU) + " = Russian\n"
                                 + "Add the values to download multiple languages\n"
                                 + "All = " + std::to_string(LANGUAGE_EN) + "+" + std::to_string(LANGUAGE_DE) + "+" + std::to_string(LANGUAGE_FR) + "+" + std::to_string(LANGUAGE_PL) + "+" + std::to_string(LANGUAGE_RU) + " = "
                                 + std::to_string(LANGUAGE_EN | LANGUAGE_DE | LANGUAGE_FR | LANGUAGE_PL | LANGUAGE_RU) + "\n"
                                 + "French + Polish = " + std::to_string(LANGUAGE_FR) + "+" + std::to_string(LANGUAGE_PL) + " = " + std::to_string(LANGUAGE_FR | LANGUAGE_PL);

    bpo::variables_map vm;
    bpo::options_description desc("Options");
    bpo::options_description config_file_options("Configuration");
    try
    {
        desc.add_options()
            ("help,h", "Print help message")
            ("login", bpo::value<bool>(&config.bLogin)->zero_tokens()->default_value(false), "Login")
            ("list", bpo::value<bool>(&config.bList)->zero_tokens()->default_value(false), "List games")
            ("list-details", bpo::value<bool>(&config.bListDetails)->zero_tokens()->default_value(false), "List games with detailed info")
            ("download", bpo::value<bool>(&config.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&config.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files")
            ("game", bpo::value<std::string>(&config.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)\nAliases: \"all\", \"free\"")
            ("directory", bpo::value<std::string>(&config.sDirectory)->default_value(""), "Set download directory")
            #ifndef ENVIRONMENT32
                ("limit-rate", bpo::value<curl_off_t>(&config.iDownloadRate)->default_value(0), "Limit download rate to value in kB\n0 = unlimited")
            #endif
            ("create-xml", bpo::value<std::string>(&config.sXMLFile)->default_value(""), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("xml-directory", bpo::value<std::string>(&config.sXMLDirectory), "Set directory for GOG XML files")
            ("chunk-size", bpo::value<size_t>(&config.iChunkSize)->default_value(10), "Chunk size (in MB) when creating XML")
            ("update-check", bpo::value<bool>(&config.bUpdateCheck)->zero_tokens()->default_value(false), "Check for update notifications")
            ("platform", bpo::value<unsigned int>(&config.iInstallerType)->default_value(INSTALLER_WINDOWS), platform_text.c_str())
            ("language", bpo::value<unsigned int>(&config.iInstallerLanguage)->default_value(LANGUAGE_EN), language_text.c_str())
            ("no-installers", bpo::value<bool>(&config.bNoInstallers)->zero_tokens()->default_value(false), "Don't download/list/repair installers")
            ("no-extras", bpo::value<bool>(&config.bNoExtras)->zero_tokens()->default_value(false), "Don't download/list/repair extras")
            ("no-cover", bpo::value<bool>(&config.bNoCover)->zero_tokens()->default_value(false), "Don't download cover images")
            ("no-remote-xml", bpo::value<bool>(&config.bNoRemoteXML)->zero_tokens()->default_value(false), "Don't use remote XML for repair")
            ("no-unicode", bpo::value<bool>(&config.bNoUnicode)->zero_tokens()->default_value(false), "Don't use Unicode in the progress bar")
            ("no-color", bpo::value<bool>(&config.bNoColor)->zero_tokens()->default_value(false), "Don't use coloring in the progress bar")
            ("verbose", bpo::value<bool>(&config.bVerbose)->zero_tokens()->default_value(false), "Print lots of information")
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

    if (config.iInstallerType < INSTALLER_WINDOWS || config.iInstallerType > (INSTALLER_WINDOWS | INSTALLER_MAC))
    {
        std::cout << "Invalid value for --platform" << std::endl;
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
    else if (config.bDownload) // Download games
        downloader.download();
    else if (config.bRepair) // Repair file
        downloader.repair();
    else if (config.bListDetails || config.bList) // Detailed list of games/extras
        downloader.listGames();
    else
    {   // Show help message
        std::cout   << config.sVersionString << std::endl
                    << desc << std::endl;
    }

    return 0;
}
