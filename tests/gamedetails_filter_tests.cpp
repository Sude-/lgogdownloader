/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "gamedetails.h"
#include "globals.h"

#include <iostream>
#include <string>

// Defined in main.cpp, which this target does not link.
namespace Globals
{
    GalaxyConfig galaxyConf;
    Config globalConfig;
    std::vector<std::string> vOwnedGamesIds;
    std::atomic<bool> bWindowProgress (false);
}

static int failures = 0;

static void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        failures++;
    }
}

static gameFile installer(
    const std::string& id,
    const std::string& path,
    const unsigned int platform,
    const unsigned int language
)
{
    gameFile file;
    file.id = id;
    file.path = path;
    file.platform = platform;
    file.language = language;
    file.type = GlobalConstants::GFTYPE_BASE_INSTALLER;
    return file;
}

int main()
{
    gameDetails game;
    game.installers = {
        installer("german", "shared/setup.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_DE),
        installer("mac", "mac/setup.pkg", GlobalConstants::PLATFORM_MAC, GlobalConstants::LANGUAGE_EN),
        installer("english", "shared/setup.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_EN),
        installer("english-duplicate", "shared/setup.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_EN)
    };

    gameFile extra;
    extra.id = "manual";
    extra.path = "manual.zip";
    extra.platform = GlobalConstants::PLATFORM_MAC;
    extra.language = GlobalConstants::LANGUAGE_DE;
    extra.type = GlobalConstants::GFTYPE_BASE_EXTRA;
    game.extras.push_back(extra);

    gameDetails dlc;
    dlc.installers = {
        installer("dlc-german", "shared/dlc.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_DE),
        installer("dlc-english", "shared/dlc.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_EN)
    };
    game.dlcs.push_back(dlc);

    DownloadConfig config = {};
    config.iInstallerPlatform = GlobalConstants::PLATFORM_LINUX;
    config.iInstallerLanguage = GlobalConstants::LANGUAGE_EN;

    game.filterWithPlatformLanguage(config);
    game.filterDuplicates();

    expect(game.installers.size() == 1, "only one selected base installer remains");
    if (!game.installers.empty())
        expect(game.installers[0].id == "english", "selected language donates the retained downlink");
    expect(game.extras.size() == 1, "platform and language filters do not remove extras");
    expect(game.dlcs.size() == 1, "DLC with a selected file remains");
    if (!game.dlcs.empty())
    {
        expect(game.dlcs[0].installers.size() == 1, "DLC filtering and deduplication are recursive");
        if (!game.dlcs[0].installers.empty())
            expect(game.dlcs[0].installers[0].id == "dlc-english", "DLC retains the selected language downlink");
    }

    // A multi-language selection must keep language attribution on the file that
    // survives deduplication, or the retained copy loses the languages it covers.
    gameDetails multilang;
    multilang.installers = {
        installer("shared-en", "shared/setup.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_EN),
        installer("shared-de", "shared/setup.sh", GlobalConstants::PLATFORM_LINUX, GlobalConstants::LANGUAGE_DE)
    };
    gameFile shared_extra_en = extra;
    shared_extra_en.id = "extra-en";
    shared_extra_en.path = "manual.zip";
    shared_extra_en.platform = GlobalConstants::PLATFORM_LINUX;
    shared_extra_en.language = GlobalConstants::LANGUAGE_EN;
    gameFile shared_extra_de = shared_extra_en;
    shared_extra_de.id = "extra-de";
    shared_extra_de.language = GlobalConstants::LANGUAGE_DE;
    multilang.extras = {shared_extra_en, shared_extra_de};

    DownloadConfig multilang_config = {};
    multilang_config.iInstallerPlatform = GlobalConstants::PLATFORM_LINUX;
    multilang_config.iInstallerLanguage = GlobalConstants::LANGUAGE_EN | GlobalConstants::LANGUAGE_DE;
    multilang.filterWithPlatformLanguage(multilang_config);
    multilang.filterDuplicates();

    expect(multilang.installers.size() == 1, "duplicate paths collapse to one installer");
    if (!multilang.installers.empty())
        expect(multilang.installers[0].language
                   == (GlobalConstants::LANGUAGE_EN | GlobalConstants::LANGUAGE_DE),
               "the retained installer keeps every language that shared its path");
    expect(multilang.extras.size() == 1, "duplicate paths collapse to one extra");
    if (!multilang.extras.empty())
        expect(multilang.extras[0].language == GlobalConstants::LANGUAGE_EN,
               "extras do not accumulate language bits when deduplicated");

    // filterDlcsWithInclude must reproduce exactly the two conditions galaxyAPI
    // applies when it admits a DLC, so that details built from the whole catalog
    // still yield the selection the user asked for.
    gameDetails with_dlc;
    gameDetails installer_only_dlc;
    gameFile dlc_installer = installer("dlc-only", "dlc/setup.exe",
        GlobalConstants::PLATFORM_WINDOWS, GlobalConstants::LANGUAGE_EN);
    dlc_installer.type = GlobalConstants::GFTYPE_DLC_INSTALLER;
    installer_only_dlc.installers = {dlc_installer};
    with_dlc.dlcs.push_back(installer_only_dlc);

    gameDetails no_dlc_selected = with_dlc;
    no_dlc_selected.filterDlcsWithInclude(GlobalConstants::GFTYPE_BASE);
    expect(no_dlc_selected.dlcs.empty(), "no DLC survives a selection that asks for no DLC type");

    gameDetails wrong_section = with_dlc;
    wrong_section.filterDlcsWithInclude(GlobalConstants::GFTYPE_DLC_EXTRA);
    expect(wrong_section.dlcs.empty(), "a DLC with no file in a selected section is dropped");

    gameDetails right_section = with_dlc;
    right_section.filterDlcsWithInclude(GlobalConstants::GFTYPE_DLC_INSTALLER);
    expect(right_section.dlcs.size() == 1, "a DLC with a file in a selected section is kept");

    // makeCustomFilepath must keep resolving %platform% the way it did before it
    // gained a platform parameter, or --check-orphans scans the wrong directory.
    DirectoryConfig dirConf = {};
    dirConf.sDirectory = "/games";
    dirConf.sGameSubdir = "%platform%/%gamename%";
    gameDetails custom;
    custom.gamename = "somegame";
    expect(custom.makeCustomFilepath("serials.txt", custom, dirConf) == "/games/windows/somegame/serials.txt",
           "the default platform keeps %platform% resolving to windows");
    expect(custom.makeCustomFilepath("serials.txt", custom, dirConf, GlobalConstants::PLATFORM_LINUX)
               == "/games/linux/somegame/serials.txt",
           "an explicit platform is honoured");

    if (failures == 0)
        std::cout << "All game-details filter tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
