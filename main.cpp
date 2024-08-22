/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "config.h"
#include "util.h"
#include "globalconstants.h"
#include "galaxyapi.h"
#include "globals.h"

#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <signal.h>

namespace bpo = boost::program_options;
Config Globals::globalConfig;

template<typename T> void set_vm_value(std::map<std::string, bpo::variable_value>& vm, const std::string& option, const T& value)
{
    vm[option].value() = boost::any(value);
}

void ensure_trailing_slash(std::string &path, const char *default_ = nullptr) {
    if (!path.empty())
    {
        if (path.at(path.length()-1)!='/')
            path += "/";
    }
    else
    {
        path = default_; // Directory wasn't specified, use current directory
    }
}

int main(int argc, char *argv[])
{
    struct sigaction act;
    act.sa_handler = SIG_IGN;
    act.sa_flags = SA_RESTART;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGPIPE, &act, NULL) < 0)
        return 1;

    rhash_library_init();

    Globals::globalConfig.sVersionString = VERSION_STRING;
    Globals::globalConfig.sVersionNumber = VERSION_NUMBER;
    Globals::globalConfig.curlConf.sUserAgent = DEFAULT_USER_AGENT;

    Globals::globalConfig.sCacheDirectory = Util::getCacheHome() + "/lgogdownloader";
    Globals::globalConfig.sXMLDirectory = Globals::globalConfig.sCacheDirectory + "/xml";

    Globals::globalConfig.sConfigDirectory = Util::getConfigHome() + "/lgogdownloader";
    Globals::globalConfig.curlConf.sCookiePath = Globals::globalConfig.sConfigDirectory + "/cookies.txt";
    Globals::globalConfig.sConfigFilePath = Globals::globalConfig.sConfigDirectory + "/config.cfg";
    Globals::globalConfig.sBlacklistFilePath = Globals::globalConfig.sConfigDirectory + "/blacklist.txt";
    Globals::globalConfig.sIgnorelistFilePath = Globals::globalConfig.sConfigDirectory + "/ignorelist.txt";
    Globals::globalConfig.sGameHasDLCListFilePath = Globals::globalConfig.sConfigDirectory + "/game_has_dlc.txt";
    Globals::globalConfig.sTransformConfigFilePath = Globals::globalConfig.sConfigDirectory + "/transformations.json";

    Globals::galaxyConf.setFilepath(Globals::globalConfig.sConfigDirectory + "/galaxy_tokens.json");

    std::string sDefaultBlacklistFilePath = Globals::globalConfig.sConfigDirectory + "/blacklist.txt";
    std::string sDefaultIgnorelistFilePath = Globals::globalConfig.sConfigDirectory + "/ignorelist.txt";

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
        language_text += GlobalConstants::LANGUAGES[i].str + " = " + GlobalConstants::LANGUAGES[i].regexp + "\n";
    }
    language_text += "All = all";
    language_text += "\n\n" + priority_help_text;
    language_text += "\nExample: German if available otherwise English and French: de,en+fr";

    // Create help text for --galaxy-language option
    std::string galaxy_language_text = "Select language\n";
    for (unsigned int i = 0; i < GlobalConstants::LANGUAGES.size(); ++i)
    {
        galaxy_language_text += GlobalConstants::LANGUAGES[i].str + " = " + GlobalConstants::LANGUAGES[i].regexp + "\n";
    }

    // Create help text for --galaxy-arch option
    std::string galaxy_arch_text = "Select architecture\n";
    for (unsigned int i = 0; i < GlobalConstants::GALAXY_ARCHS.size(); ++i)
    {
        galaxy_arch_text += GlobalConstants::GALAXY_ARCHS[i].str + " = " + GlobalConstants::GALAXY_ARCHS[i].regexp + "\n";
    }

    // Create help text for --subdir-galaxy-install option
    std::string galaxy_install_subdir_text = "Set subdirectory for galaxy install\n"
        "\nTemplates:\n"
        "- %install_dir% = Installation directory from Galaxy API response\n"
        "- %gamename% = Game name\n"
        "- %title% = Title of the game\n"
        "- %product_id% = Product id of the game\n"
        "- %install_dir_stripped% = %install_dir% with some characters stripped\n"
        "- %title_stripped% = %title% with some characters stripped\n"
        "\n\"stripped\" means that every character that doesn't match the following list is removed:\n"
        "> alphanumeric\n"
        "> space\n"
        "> - _ . ( ) [ ] { }";

    // Create help text for --galaxy-cdn-priority option
    std::string galaxy_cdn_priority_text = "Set priority for used CDNs\n";
    galaxy_cdn_priority_text += "Use --galaxy-list-cdns to list available CDNs\n";
    galaxy_cdn_priority_text += "Set priority by separating values with \",\"";

    // Create help text for --check-orphans
    std::string orphans_regex_default = ".*\\.(zip|exe|bin|dmg|old|deb|tar\\.gz|pkg|sh|mp4)$"; // Limit to files with these extensions (".old" is for renamed older version files)
    std::string check_orphans_text = "Check for orphaned files (files found on local filesystem that are not found on GOG servers). Sets regular expression filter (Perl syntax) for files to check. If no argument is given then the regex defaults to '" + orphans_regex_default + "'";

    // Help text for subdir options
    std::string subdir_help_text = "\nTemplates:\n- %platform%\n- %gamename%\n- %gamename_firstletter%\n- %dlcname%\n- %gamename_transformed%\n- %gamename_transformed_firstletter%";

    // Help text for include and exclude options
    std::string include_options_text;
    for (unsigned int i = 0; i < GlobalConstants::INCLUDE_OPTIONS.size(); ++i)
    {
        include_options_text += GlobalConstants::INCLUDE_OPTIONS[i].str + " = " + GlobalConstants::INCLUDE_OPTIONS[i].regexp + "\n";
    }
    include_options_text += "All = all\n";
    include_options_text += "Separate with \",\" to use multiple values";

    // Create help text for --list-format option
    std::string list_format_text = "List games/tags\n";
    for (unsigned int i = 0; i < GlobalConstants::LIST_FORMAT.size(); ++i)
    {
        list_format_text += GlobalConstants::LIST_FORMAT[i].str + " = " + GlobalConstants::LIST_FORMAT[i].regexp + "\n";
    }

    std::string galaxy_product_id_install;
    std::string galaxy_product_id_list_cdns;
    std::string galaxy_product_id_show_builds;
    std::string galaxy_product_id_show_cloud_paths;
    std::string galaxy_product_id_show_local_cloud_paths;
    std::string galaxy_product_cloud_saves;
    std::string galaxy_product_cloud_saves_delete;
    std::string galaxy_upload_product_cloud_saves;
    std::string tags;

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
    bool bClearUpdateNotifications = false;
    bool bList = false;
    bool bCheckLoginStatus = false;
    try
    {
        bool bInsecure = false;
        bool bNoColor = false;
        bool bNoUnicode = false;
        bool bNoDuplicateHandler = false;
        bool bNoRemoteXML = false;
        bool bNoSubDirectories = false;
        bool bNoPlatformDetection = false;
        bool bNoGalaxyDependencies = false;
        bool bUseDLCList = false;
        bool bNoFastStatusCheck = false;
        std::string sInstallerPlatform;
        std::string sInstallerLanguage;
        std::string sIncludeOptions;
        std::string sExcludeOptions;
        std::string sGalaxyPlatform;
        std::string sGalaxyLanguage;
        std::string sGalaxyArch;
        std::string sGalaxyCDN;
        std::string sListFormat;
        Globals::globalConfig.bReport = false;
        // Commandline options (no config file)
        options_cli_no_cfg.add_options()
            ("help,h", "Print help message")
            ("version", "Print version information")
            ("login", bpo::value<bool>(&Globals::globalConfig.bLogin)->zero_tokens()->default_value(false), "Login")
#ifdef USE_QT_GUI_LOGIN
            ("gui-login", bpo::value<bool>(&Globals::globalConfig.bForceGUILogin)->zero_tokens()->default_value(false), "Login (force GUI login)\nImplies --enable-login-gui")
#endif
            ("browser-login", bpo::value<bool>(&Globals::globalConfig.bForceBrowserLogin)->zero_tokens()->default_value(false), "Login (force browser login)")
            ("check-login-status", bpo::value<bool>(&bCheckLoginStatus)->zero_tokens()->default_value(false), "Check login status")
            ("list", bpo::value<std::string>(&sListFormat)->implicit_value("games"), list_format_text.c_str())
            ("download", bpo::value<bool>(&Globals::globalConfig.bDownload)->zero_tokens()->default_value(false), "Download")
            ("repair", bpo::value<bool>(&Globals::globalConfig.bRepair)->zero_tokens()->default_value(false), "Repair downloaded files\nUse --repair --download to redownload files when filesizes don't match (possibly different version). Redownload will rename the old file (appends .old to filename)")
            ("game", bpo::value<std::string>(&Globals::globalConfig.sGameRegex)->default_value(""), "Set regular expression filter\nfor download/list/repair (Perl syntax)")
            ("create-xml", bpo::value<std::string>(&Globals::globalConfig.sXMLFile)->implicit_value("automatic"), "Create GOG XML for file\n\"automatic\" to enable automatic XML creation")
            ("notifications", bpo::value<bool>(&Globals::globalConfig.bNotifications)->zero_tokens()->default_value(false), "Check notifications")
            ("updated", bpo::value<bool>(&Globals::globalConfig.bUpdated)->zero_tokens()->default_value(false), "List/download only games with update flag set")
            ("new", bpo::value<bool>(&Globals::globalConfig.bNew)->zero_tokens()->default_value(false), "List/download only games with new flag set")
            ("clear-update-flags", bpo::value<bool>(&bClearUpdateNotifications)->zero_tokens()->default_value(false), "Clear update notification flags")
            ("check-orphans", bpo::value<std::string>(&Globals::globalConfig.sOrphanRegex)->implicit_value(""), check_orphans_text.c_str())
            ("delete-orphans", bpo::value<bool>(&Globals::globalConfig.dlConf.bDeleteOrphans)->zero_tokens()->default_value(false), "Delete orphaned files during --check-orphans and --galaxy-install")
            ("status", bpo::value<bool>(&Globals::globalConfig.bCheckStatus)->zero_tokens()->default_value(false), "Show status of files\n\nOutput format:\nstatuscode gamename filename filesize filehash\n\nStatus codes:\nOK - File is OK\nND - File is not downloaded\nMD5 - MD5 mismatch, different version\nFS - File size mismatch, incomplete download\n\nSee also --no-fast-status-check option")
            ("save-config", bpo::value<bool>(&Globals::globalConfig.bSaveConfig)->zero_tokens()->default_value(false), "Create config file with current settings")
            ("reset-config", bpo::value<bool>(&Globals::globalConfig.bResetConfig)->zero_tokens()->default_value(false), "Reset config settings to default")
            ("report", bpo::value<std::string>(&Globals::globalConfig.sReportFilePath)->implicit_value("lgogdownloader-report.log"), "Save report of downloaded/repaired files to specified file\nDefault filename: lgogdownloader-report.log")
            ("update-cache", bpo::value<bool>(&Globals::globalConfig.bUpdateCache)->zero_tokens()->default_value(false), "Update game details cache")
            ("no-platform-detection", bpo::value<bool>(&bNoPlatformDetection)->zero_tokens()->default_value(false), "Don't try to detect supported platforms from game shelf.\nSkips the initial fast platform detection and detects the supported platforms from game details which is slower but more accurate.\nUseful in case platform identifier is missing for some games in the game shelf.\nUsing --platform with --list doesn't work with this option.")
            ("download-file", bpo::value<std::string>(&Globals::globalConfig.sFileIdString)->default_value(""), "Download files using fileid\n\nFormat:\n\"gamename/fileid\"\n\"gamename/dlc_gamename/fileid\"\n\"gogdownloader://gamename/fileid\"\n\"gogdownloader://gamename/dlc_name/fileid\"\n\nMultiple files:\n\"gamename1/fileid1,gamename2/fileid2,gamename2/dlcname/fileid1\"\n\nThis option ignores all subdir options. The files are downloaded to directory specified with --directory option.")
            ("output-file,o", bpo::value<std::string>(&Globals::globalConfig.sOutputFilename)->default_value(""), "Set filename of file downloaded with --download-file.")
            ("wishlist", bpo::value<bool>(&Globals::globalConfig.bShowWishlist)->zero_tokens()->default_value(false), "Show wishlist")
            ("cacert", bpo::value<std::string>(&Globals::globalConfig.curlConf.sCACertPath)->default_value(""), "Path to CA certificate bundle in PEM format")
            ("respect-umask", bpo::value<bool>(&Globals::globalConfig.bRespectUmask)->zero_tokens()->default_value(false), "Do not adjust permissions of sensitive files")
            ("user-agent", bpo::value<std::string>(&Globals::globalConfig.curlConf.sUserAgent)->default_value(DEFAULT_USER_AGENT), "Set user agent")

            ("wine-prefix", bpo::value<std::string>(&Globals::globalConfig.dirConf.sWinePrefix)->default_value("."), "Set wineprefix directory")
            ("cloud-whitelist", bpo::value<std::vector<std::string>>(&Globals::globalConfig.cloudWhiteList)->multitoken(), "Include this list of cloud saves, by default all cloud saves are included\n Example: --cloud-whitelist saves/AutoSave-0 saves/AutoSave-1/screenshot.png")
            ("cloud-blacklist", bpo::value<std::vector<std::string>>(&Globals::globalConfig.cloudBlackList)->multitoken(), "Exclude this list of cloud saves\n Example: --cloud-blacklist saves/AutoSave-0 saves/AutoSave-1/screenshot.png")
            ("cloud-force", bpo::value<bool>(&Globals::globalConfig.bCloudForce)->zero_tokens()->default_value(false), "Download or Upload cloud saves even if they're up-to-date\nDelete remote cloud saves even if no saves are whitelisted")
#ifdef USE_QT_GUI_LOGIN
            ("enable-login-gui", bpo::value<bool>(&Globals::globalConfig.bEnableLoginGUI)->zero_tokens()->default_value(false), "Enable login GUI when encountering reCAPTCHA on login form")
#endif
            ("tag", bpo::value<std::string>(&tags)->default_value(""), "Filter using tags. Separate with \",\" to use multiple values")
            ("blacklist", bpo::value<std::string>(&Globals::globalConfig.sBlacklistFilePath)->default_value(sDefaultBlacklistFilePath), "Filepath to blacklist")
            ("ignorelist", bpo::value<std::string>(&Globals::globalConfig.sIgnorelistFilePath)->default_value(sDefaultIgnorelistFilePath), "Filepath to ignorelist")
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
            ("curl-verbose", bpo::value<bool>(&Globals::globalConfig.curlConf.bVerbose)->zero_tokens()->default_value(false), "Set libcurl to verbose mode")
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
            ("save-game-details-json", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveGameDetailsJson)->zero_tokens()->default_value(false), "Save game details JSON data as-is to \"game-details.json\"")
            ("save-product-json", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveProductJson)->zero_tokens()->default_value(false), "Save product info JSON data from the API as-is to \"product.json\"")
            ("save-logo", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveLogo)->zero_tokens()->default_value(false), "Save logo when downloading")
            ("save-icon", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveIcon)->zero_tokens()->default_value(false), "Save icon when downloading")
            ("ignore-dlc-count", bpo::value<std::string>(&Globals::globalConfig.sIgnoreDLCCountRegex)->implicit_value(".*"), "Set regular expression filter for games to ignore DLC count information\nIgnoring DLC count information helps in situations where the account page doesn't provide accurate information about DLCs")
            ("include", bpo::value<std::string>(&sIncludeOptions)->default_value("all"), ("Select what to download/list/repair\n" + include_options_text).c_str())
            ("exclude", bpo::value<std::string>(&sExcludeOptions)->default_value(""), ("Select what not to download/list/repair\n" + include_options_text).c_str())
            ("automatic-xml-creation", bpo::value<bool>(&Globals::globalConfig.dlConf.bAutomaticXMLCreation)->zero_tokens()->default_value(false), "Automatically create XML data after download has completed")
            ("save-changelogs", bpo::value<bool>(&Globals::globalConfig.dlConf.bSaveChangelogs)->zero_tokens()->default_value(false), "Save changelogs when downloading")
            ("threads", bpo::value<unsigned int>(&Globals::globalConfig.iThreads)->default_value(4), "Number of download threads")
            ("info-threads", bpo::value<unsigned int>(&Globals::globalConfig.iInfoThreads)->default_value(4), "Number of threads for getting product info")
            ("use-dlc-list", bpo::value<bool>(&bUseDLCList)->zero_tokens()->default_value(false), "Use DLC list specified with --dlc-list")
            ("dlc-list", bpo::value<std::string>(&Globals::globalConfig.sGameHasDLCList)->default_value("https://raw.githubusercontent.com/Sude-/lgogdownloader-lists/master/game_has_dlc.txt"), "Set URL for list of games that have DLC")
            ("progress-interval", bpo::value<int>(&Globals::globalConfig.iProgressInterval)->default_value(100), "Set interval for progress bar update (milliseconds)\nValue must be between 1 and 10000")
            ("lowspeed-timeout", bpo::value<long int>(&Globals::globalConfig.curlConf.iLowSpeedTimeout)->default_value(30), "Set time in number seconds that the transfer speed should be below the rate set with --lowspeed-rate for it to considered too slow and aborted")
            ("lowspeed-rate", bpo::value<long int>(&Globals::globalConfig.curlConf.iLowSpeedTimeoutRate)->default_value(200), "Set average transfer speed in bytes per second that the transfer should be below during time specified with --lowspeed-timeout for it to be considered too slow and aborted")
            ("include-hidden-products", bpo::value<bool>(&Globals::globalConfig.bIncludeHiddenProducts)->zero_tokens()->default_value(false), "Include games that have been set hidden in account page")
            ("size-only", bpo::value<bool>(&Globals::globalConfig.bSizeOnly)->zero_tokens()->default_value(false), "Don't check the hashes of the files whose size matches that on the server")
            ("verbosity", bpo::value<int>(&Globals::globalConfig.iMsgLevel)->default_value(0), "Set message verbosity level\n -1 = Less verbose\n 0 = Default\n 1 = Verbose\n 2 = Debug")
            ("check-free-space", bpo::value<bool>(&Globals::globalConfig.dlConf.bFreeSpaceCheck)->zero_tokens()->default_value(false), "Check for available free space before starting download")
            ("no-fast-status-check", bpo::value<bool>(&bNoFastStatusCheck)->zero_tokens()->default_value(false), "Don't use fast status check.\nMakes --status much slower but able to catch corrupted files by calculating local file hash for all files.")
            ("trust-api-for-extras", bpo::value<bool>(&Globals::globalConfig.bTrustAPIForExtras)->zero_tokens()->default_value(false), "Trust API responses for extras to be correct.")
        ;

        options_cli_no_cfg_hidden.add_options()
            ("login-email", bpo::value<std::string>(&Globals::globalConfig.sEmail)->default_value(""), "login email")
            ("login-password", bpo::value<std::string>(&Globals::globalConfig.sPassword)->default_value(""), "login password")
        ;

        options_cli_experimental.add_options()
            ("galaxy-install", bpo::value<std::string>(&galaxy_product_id_install)->default_value(""), "Install game using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-show-builds", bpo::value<std::string>(&galaxy_product_id_show_builds)->default_value(""), "Show game builds using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build\nLists available builds if build index is not specified\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-download-cloud-saves", bpo::value<std::string>(&galaxy_product_cloud_saves)->default_value(""), "Download cloud saves using product-id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-upload-cloud-saves", bpo::value<std::string>(&galaxy_upload_product_cloud_saves)->default_value(""), "Upload cloud saves using product-id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-show-cloud-saves", bpo::value<std::string>(&galaxy_product_id_show_cloud_paths)->default_value(""), "Show game cloud-saves using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-show-local-cloud-saves", bpo::value<std::string>(&galaxy_product_id_show_local_cloud_paths)->default_value(""), "Show local cloud-saves using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-delete-cloud-saves", bpo::value<std::string>(&galaxy_product_cloud_saves_delete)->default_value(""), "Delete cloud-saves using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
            ("galaxy-platform", bpo::value<std::string>(&sGalaxyPlatform)->default_value("w"), galaxy_platform_text.c_str())
            ("galaxy-language", bpo::value<std::string>(&sGalaxyLanguage)->default_value("en"), galaxy_language_text.c_str())
            ("galaxy-arch", bpo::value<std::string>(&sGalaxyArch)->default_value("x64"), galaxy_arch_text.c_str())
            ("galaxy-no-dependencies", bpo::value<bool>(&bNoGalaxyDependencies)->zero_tokens()->default_value(false), "Don't download dependencies during --galaxy-install")
            ("subdir-galaxy-install", bpo::value<std::string>(&Globals::globalConfig.dirConf.sGalaxyInstallSubdir)->default_value("%install_dir%"), galaxy_install_subdir_text.c_str())
            ("galaxy-cdn-priority", bpo::value<std::string>(&sGalaxyCDN)->default_value("edgecast,akamai_edgecast_proxy,fastly"), galaxy_cdn_priority_text.c_str())
            ("galaxy-list-cdns", bpo::value<std::string>(&galaxy_product_id_list_cdns)->default_value(""), "List available CDNs for game using product id [product_id/build_index] or gamename regex [gamename/build_id]\nBuild index is used to select a build and defaults to 0 if not specified.\n\nExample: 12345/2 selects build 2 for product 12345")
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

        if (vm.count("list"))
        {
            bList = true;
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

        if (boost::filesystem::exists(Globals::globalConfig.sTransformConfigFilePath))
        {
            Globals::globalConfig.transformationsJSON = Util::readJsonFile(Globals::globalConfig.sTransformConfigFilePath);
        }

        if (!bUseDLCList)
            Globals::globalConfig.sGameHasDLCList = "";

        if (Globals::globalConfig.sIgnoreDLCCountRegex.empty())
        {
            if (boost::filesystem::exists(Globals::globalConfig.sGameHasDLCListFilePath) && bUseDLCList)
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

        #ifdef USE_QT_GUI_LOGIN
        if (Globals::globalConfig.bForceGUILogin)
        {
            Globals::globalConfig.bLogin = true;
            Globals::globalConfig.bEnableLoginGUI = true;
        }
        #endif

        if (Globals::globalConfig.bForceBrowserLogin)
        {
            Globals::globalConfig.bLogin = true;
        }

        if (Globals::globalConfig.bLogin)
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

        if (Globals::globalConfig.iMsgLevel < -1)
        {
            Globals::globalConfig.iMsgLevel = -1;
            set_vm_value(vm, "verbosity", Globals::globalConfig.iMsgLevel);
        }

        Globals::globalConfig.curlConf.bVerifyPeer = !bInsecure;
        Globals::globalConfig.bColor = !bNoColor;
        Globals::globalConfig.bUnicode = !bNoUnicode;
        Globals::globalConfig.dlConf.bDuplicateHandler = !bNoDuplicateHandler;
        Globals::globalConfig.dlConf.bRemoteXML = !bNoRemoteXML;
        Globals::globalConfig.dirConf.bSubDirectories = !bNoSubDirectories;
        Globals::globalConfig.bPlatformDetection = !bNoPlatformDetection;
        Globals::globalConfig.dlConf.bGalaxyDependencies = !bNoGalaxyDependencies;
        Globals::globalConfig.bUseFastCheck = !bNoFastStatusCheck;

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

        if (!tags.empty())
            Globals::globalConfig.dlConf.vTags = Util::tokenize(tags, ",");

        if (!Globals::globalConfig.sOutputFilename.empty() && vFileIdStrings.size() > 1)
        {
            std::cerr << "Cannot specify an output file name when downloading multiple files." << std::endl;
            return 1;
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

        Globals::globalConfig.dlConf.vGalaxyCDNPriority = Util::tokenize(sGalaxyCDN, ",");

        unsigned int include_value = 0;
        unsigned int exclude_value = 0;
        std::vector<std::string> vInclude = Util::tokenize(sIncludeOptions, ",");
        std::vector<std::string> vExclude = Util::tokenize(sExcludeOptions, ",");
        for (std::vector<std::string>::iterator it = vInclude.begin(); it != vInclude.end(); it++)
        {
            include_value |= Util::getOptionValue(*it, GlobalConstants::INCLUDE_OPTIONS);
        }
        for (std::vector<std::string>::iterator it = vExclude.begin(); it != vExclude.end(); it++)
        {
            exclude_value |= Util::getOptionValue(*it, GlobalConstants::INCLUDE_OPTIONS);
        }
        Globals::globalConfig.dlConf.iInclude = include_value & ~exclude_value;

        Globals::globalConfig.iListFormat = Util::getOptionValue(sListFormat, GlobalConstants::LIST_FORMAT, false);
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
    ensure_trailing_slash(Globals::globalConfig.dirConf.sDirectory, "./");
    ensure_trailing_slash(Globals::globalConfig.dirConf.sWinePrefix, "./");

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
    curl_global_init(CURL_GLOBAL_ALL);
    struct CurlCleanup { ~CurlCleanup() { curl_global_cleanup(); } };
    CurlCleanup _curl_cleanup;

    Downloader downloader;

    bool bLoginOK = false;

    // Login because --login was used
    if (Globals::globalConfig.bLogin)
    {
        bLoginOK = downloader.login();
    }
    else
    {
        bool bIsLoggedin = downloader.isLoggedIn();
        if (bCheckLoginStatus)
        {
            if (bIsLoggedin)
            {
                std::cout << "Login status: Logged in" << std::endl;
                return 0;
            }
            else
            {
                std::cout << "Login status: Not logged in" << std::endl;
                return 1;
            }
        }
        if (!bIsLoggedin)
        {
            Globals::globalConfig.bLogin = true;
            bLoginOK = downloader.login();
            if (bLoginOK)
            {
                bIsLoggedin = downloader.isLoggedIn();
            }
        }

        // Login failed, cleanup
        if (!bLoginOK && !bIsLoggedin)
        {
            return 1;
        }
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

                ofs << option << " = " << option_value_string << std::endl;
            }
            ofs.close();
            if (!Globals::globalConfig.bRespectUmask)
                Util::setFilePermissions(Globals::globalConfig.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);
            if (Globals::globalConfig.bSaveConfig)
            {
                return 0;
            }
        }
        else
        {
            std::cerr << "Failed to create config: " << Globals::globalConfig.sConfigFilePath << std::endl;
            return 1;
        }
    }
    else if (Globals::globalConfig.bResetConfig)
    {
        std::ofstream ofs(Globals::globalConfig.sConfigFilePath.c_str());
        if (ofs)
        {
            ofs.close();
            if (!Globals::globalConfig.bRespectUmask)
                Util::setFilePermissions(Globals::globalConfig.sConfigFilePath, boost::filesystem::owner_read | boost::filesystem::owner_write);

            return 0;
        }
        else
        {
            std::cerr << "Failed to create config: " << Globals::globalConfig.sConfigFilePath << std::endl;
            return 1;
        }
    }

    bool bInitOK = downloader.init();
    if (!bInitOK)
    {
        return 1;
    }

    int res = 0;

    if (Globals::globalConfig.bShowWishlist)
        downloader.showWishlist();
    else if (Globals::globalConfig.bUpdateCache)
        downloader.updateCache();
    else if (Globals::globalConfig.bNotifications)
        downloader.checkNotifications();
    else if (bClearUpdateNotifications)
        downloader.clearUpdateNotifications();
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
    else if (bList) // List games/extras/tags
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
    else if (!galaxy_product_id_show_cloud_paths.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_id_show_cloud_paths, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.galaxyShowCloudSaves(product_id, build_index);
    }
    else if (!galaxy_product_id_show_local_cloud_paths.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_id_show_local_cloud_paths, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.galaxyShowLocalCloudSaves(product_id, build_index);
    }
    else if (!galaxy_product_cloud_saves_delete.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_cloud_saves_delete, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.deleteCloudSaves(product_id, build_index);
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
    else if (!galaxy_product_id_list_cdns.empty())
    {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_id_list_cdns, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.galaxyListCDNs(product_id, build_index);
    }
    else if (!galaxy_product_cloud_saves.empty()) {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_product_cloud_saves, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.downloadCloudSaves(product_id, build_index);
    }
    else if (!galaxy_upload_product_cloud_saves.empty()) {
        int build_index = -1;
        std::vector<std::string> tokens = Util::tokenize(galaxy_upload_product_cloud_saves, "/");
        std::string product_id = tokens[0];
        if (tokens.size() == 2)
        {
            build_index = std::stoi(tokens[1]);
        }
        downloader.uploadCloudSaves(product_id, build_index);
    }
    else
    {
        if (!Globals::globalConfig.bLogin)
        {
            // Show help message
            std::cerr   << Globals::globalConfig.sVersionString << std::endl
                        << options_cli_all << std::endl;
        }
    }

    // Orphan check was called at the same time as download. Perform it after download has finished
    if (!Globals::globalConfig.sOrphanRegex.empty() && Globals::globalConfig.bDownload)
        downloader.checkOrphans();

    return res;
}
