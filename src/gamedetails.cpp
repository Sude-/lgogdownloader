/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "gamedetails.h"

gameDetails::gameDetails()
{
    //ctor
}

gameDetails::~gameDetails()
{
    //dtor
}

void gameDetails::filterWithPriorities(const gameSpecificConfig& config)
{
    if (config.dlConf.vPlatformPriority.empty() && config.dlConf.vLanguagePriority.empty())
        return;

    filterListWithPriorities(installers, config);
    filterListWithPriorities(patches, config);
    filterListWithPriorities(languagepacks, config);
    for (unsigned int i = 0; i < dlcs.size(); ++i)
    {
        filterListWithPriorities(dlcs[i].installers, config);
        filterListWithPriorities(dlcs[i].patches, config);
        filterListWithPriorities(dlcs[i].languagepacks, config);
    }
}

void gameDetails::filterListWithPriorities(std::vector<gameFile>& list, const gameSpecificConfig& config)
{
    /*
      Compute the score of each item - we use a scoring mechanism and we keep all ties
      Like if someone asked French then English and Linux then Windows, but there are
      only Windows French, Windows English and Linux English versions, we'll get the
      Windows French and Linux English ones.
      Score is inverted: lower is better.
    */
    int bestscore = -1;

    for (std::vector<gameFile>::iterator fileDetails = list.begin(); fileDetails != list.end(); fileDetails++)
        {
            fileDetails->score = 0;
            if (!config.dlConf.vPlatformPriority.empty())
                {
                    for (size_t i = 0; i != config.dlConf.vPlatformPriority.size(); i++)
                        if (fileDetails->platform & config.dlConf.vPlatformPriority[i])
                            {
                                fileDetails->score += i;
                                break;
                            }
                }
            if (!config.dlConf.vLanguagePriority.empty())
                {
                    for (size_t i = 0; i != config.dlConf.vLanguagePriority.size(); i++)
                        if (fileDetails->language & config.dlConf.vLanguagePriority[i])
                            {
                                fileDetails->score += i;
                                break;
                            }
                }
            if ((fileDetails->score < bestscore) or (bestscore < 0))
                bestscore = fileDetails->score;
        }

    for (std::vector<gameFile>::iterator fileDetails = list.begin(); fileDetails != list.end(); )
        {
            if (fileDetails->score > bestscore)
                fileDetails = list.erase(fileDetails);
            else
                fileDetails++;
        }
}

void gameDetails::makeFilepaths(const DirectoryConfig& config)
{
    std::string filepath;
    std::string directory = config.sDirectory + "/" + config.sGameSubdir + "/";
    std::string subdir;
    std::string logo_ext = ".jpg"; // Assume jpg
    std::string icon_ext = ".png"; // Assume png

    if (this->logo.rfind(".") != std::string::npos)
        logo_ext = this->logo.substr(this->logo.rfind("."));

    if (this->icon.rfind(".") != std::string::npos)
        icon_ext = this->icon.substr(this->icon.rfind("."));

    this->serialsFilepath = this->makeCustomFilepath(std::string("serials.txt"), *this, config);
    this->logoFilepath = this->makeCustomFilepath(std::string("logo") + logo_ext, *this, config);
    this->iconFilepath = this->makeCustomFilepath(std::string("icon") + icon_ext, *this, config);
    this->changelogFilepath = this->makeCustomFilepath(std::string("changelog_") + gamename + ".html", *this, config);
    this->gameDetailsJsonFilepath = this->makeCustomFilepath(std::string("game-details.json"), *this, config);
    this->productJsonFilepath = this->makeCustomFilepath(std::string("product.json"), *this, config);

    for (auto &installer : this->installers)
    {
        filepath = this->makeFilepath(installer, config);
        installer.setFilepath(filepath);
    }

    for (auto &extra : this->extras)
    {
        filepath = this->makeFilepath(extra, config);
        extra.setFilepath(filepath);
    }

    for (auto &patch : this->patches)
    {
        filepath = this->makeFilepath(patch, config);
        patch.setFilepath(filepath);
    }

    for (auto &languagepack : this->languagepacks)
    {
        filepath = this->makeFilepath(languagepack, config);
        languagepack.setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->dlcs.size(); ++i)
    {
        this->dlcs[i].serialsFilepath = this->makeCustomFilepath(std::string("serials.txt"), this->dlcs[i], config);
        this->dlcs[i].logoFilepath = this->makeCustomFilepath(std::string("logo") + logo_ext, this->dlcs[i], config);
        this->dlcs[i].iconFilepath = this->makeCustomFilepath(std::string("icon") + icon_ext, this->dlcs[i], config);
        this->dlcs[i].changelogFilepath = this->makeCustomFilepath(std::string("changelog_") + this->dlcs[i].gamename + ".html", this->dlcs[i], config);
        this->dlcs[i].productJsonFilepath = this->makeCustomFilepath(std::string("product.json"), this->dlcs[i], config);

        for (auto &installer : this->dlcs[i].installers)
        {
            filepath = this->makeFilepath(installer, config);
            installer.setFilepath(filepath);
        }

        for (auto &extra : this->dlcs[i].extras)
        {
            filepath = this->makeFilepath(extra, config);
            extra.setFilepath(filepath);
        }

        for (auto &patch : this->dlcs[i].patches)
        {
            filepath = this->makeFilepath(patch, config);
            patch.setFilepath(filepath);
        }

        for (auto &languagepack : this->dlcs[i].languagepacks)
        {
            filepath = this->makeFilepath(languagepack, config);
            languagepack.setFilepath(filepath);
        }
    }
}

Json::Value gameDetails::getDetailsAsJson()
{
    Json::Value json;

    json["gamename"] = this->gamename;
    json["product_id"] = this->product_id;
    json["title"] = this->title;
    json["icon"] = this->icon;
    json["serials"] = this->serials;
    json["changelog"] = this->changelog;

    for (unsigned int i = 0; i < this->extras.size(); ++i)
        json["extras"].append(this->extras[i].getAsJson());
    for (unsigned int i = 0; i < this->installers.size(); ++i)
        json["installers"].append(this->installers[i].getAsJson());
    for (unsigned int i = 0; i < this->patches.size(); ++i)
        json["patches"].append(this->patches[i].getAsJson());
    for (unsigned int i = 0; i < this->languagepacks.size(); ++i)
        json["languagepacks"].append(this->languagepacks[i].getAsJson());

    if (!this->dlcs.empty())
    {
        for (unsigned int i = 0; i < this->dlcs.size(); ++i)
        {
            json["dlcs"].append(this->dlcs[i].getDetailsAsJson());
        }
    }

    return json;
}

std::string gameDetails::getSerialsFilepath()
{
    return this->serialsFilepath;
}

std::string gameDetails::getLogoFilepath()
{
    return this->logoFilepath;
}

std::string gameDetails::getIconFilepath()
{
    return this->iconFilepath;
}

std::string gameDetails::getChangelogFilepath()
{
    return this->changelogFilepath;
}

std::string gameDetails::getGameDetailsJsonFilepath()
{
    return this->gameDetailsJsonFilepath;
}

std::string gameDetails::getProductJsonFilepath()
{
    return this->productJsonFilepath;
}

// Return vector containing all game files
std::vector<gameFile> gameDetails::getGameFileVector()
{
    std::vector<gameFile> vGameFiles;

    vGameFiles.insert(std::end(vGameFiles), std::begin(installers), std::end(installers));
    vGameFiles.insert(std::end(vGameFiles), std::begin(patches), std::end(patches));
    vGameFiles.insert(std::end(vGameFiles), std::begin(extras), std::end(extras));
    vGameFiles.insert(std::end(vGameFiles), std::begin(languagepacks), std::end(languagepacks));

    if (!dlcs.empty())
    {
        for (unsigned int i = 0; i < dlcs.size(); ++i)
        {
            std::vector<gameFile> vGameFilesDLC = dlcs[i].getGameFileVector();
            vGameFiles.insert(std::end(vGameFiles), std::begin(vGameFilesDLC), std::end(vGameFilesDLC));
        }
    }

    return vGameFiles;
}

// Return vector containing all game files matching download filters
std::vector<gameFile> gameDetails::getGameFileVectorFiltered(const unsigned int& iType)
{
    std::vector<gameFile> vGameFiles;

    for (auto gf : this->getGameFileVector())
    {
        if (gf.type & iType)
            vGameFiles.push_back(gf);
    }

    return vGameFiles;
}

void gameDetails::filterWithType(const unsigned int& iType)
{
    filterListWithType(installers, iType);
    filterListWithType(patches, iType);
    filterListWithType(extras, iType);
    filterListWithType(languagepacks, iType);
    for (unsigned int i = 0; i < dlcs.size(); ++i)
    {
        filterListWithType(dlcs[i].installers, iType);
        filterListWithType(dlcs[i].patches, iType);
        filterListWithType(dlcs[i].extras, iType);
        filterListWithType(dlcs[i].languagepacks, iType);
    }
}

void gameDetails::filterListWithType(std::vector<gameFile>& list, const unsigned int& iType)
{
    for (std::vector<gameFile>::iterator gf = list.begin(); gf != list.end();)
    {
        if (!(gf->type & iType))
            gf = list.erase(gf);
        else
            gf++;
    }
}

std::string gameDetails::makeFilepath(const gameFile& gf, const DirectoryConfig& dirConf)
{
    std::map<std::string, std::string> templates;

    std::string path = gf.path;
    std::string filename = path;
    if (path.find_last_of("/") != std::string::npos)
        filename = path.substr(path.find_last_of("/")+1, path.length());

    std::string subdir;
    if (dirConf.bSubDirectories)
    {
        if (gf.type & GlobalConstants::GFTYPE_INSTALLER)
        {
            subdir = dirConf.sInstallersSubdir;
        }
        else if (gf.type & GlobalConstants::GFTYPE_EXTRA)
        {
            subdir = dirConf.sExtrasSubdir;
        }
        else if (gf.type & GlobalConstants::GFTYPE_PATCH)
        {
            subdir = dirConf.sPatchesSubdir;
        }
        else if (gf.type & GlobalConstants::GFTYPE_LANGPACK)
        {
            subdir = dirConf.sLanguagePackSubdir;
        }

        if (gf.type & GlobalConstants::GFTYPE_DLC)
        {
            subdir = dirConf.sDLCSubdir + "/" + subdir;
        }

        if (!dirConf.sGameSubdir.empty())
        {
            subdir = dirConf.sGameSubdir + "/" + subdir;
        }
    }
    std::string gamename = gf.gamename;
    std::string title = gf.title;
    std::string dlc_gamename;
    std::string dlc_title;
    if (gf.type & GlobalConstants::GFTYPE_DLC)
    {
        gamename = gf.gamename_basegame;
        title = gf.title_basegame;
        dlc_gamename = gf.gamename;
        dlc_title = gf.title;
    }

    std::string filepath = dirConf.sDirectory + "/" + subdir + "/" + filename;

    std::string platform;
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        if ((gf.platform & GlobalConstants::PLATFORMS[i].id) == GlobalConstants::PLATFORMS[i].id)
        {
            platform = boost::algorithm::to_lower_copy(GlobalConstants::PLATFORMS[i].str);
            break;
        }
    }
    if (platform.empty())
    {
        if (filepath.find("%gamename%/%platform%") != std::string::npos)
            platform = "";
        else
            platform = "no_platform";
    }

    // Don't save certain files in "no_platform" folder
    if (
           filepath.rfind("/icon.png") != std::string::npos
        || filepath.rfind("/logo.jpg") != std::string::npos
        || filepath.rfind("/product.json") != std::string::npos
    )
        platform = "";

    std::string gamename_firstletter;
    if (!gamename.empty())
    {
        if (std::isdigit(gamename.front()))
            gamename_firstletter = "0";
        else
            gamename_firstletter = gamename.front();
    }

    std::string gamename_transformed;
    std::string gamename_transformed_firstletter;
    if (filepath.find("%gamename_transformed%") != std::string::npos || filepath.find("%gamename_transformed_firstletter%") != std::string::npos)
    {
        gamename_transformed = Util::transformGamename(gamename);
        if (!gamename_transformed.empty())
        {
            if (std::isdigit(gamename_transformed.front()))
                gamename_transformed_firstletter = "0";
            else
                gamename_transformed_firstletter = gamename_transformed.front();
        }
    }

    templates["%gamename%"] = gamename;
    templates["%gamename_firstletter%"] = gamename_firstletter;
    templates["%title%"] = title;
    templates["%title_stripped%"] = Util::getStrippedString(title);
    templates["%dlcname%"] = dlc_gamename;
    templates["%dlc_title%"] = dlc_title;
    templates["%dlc_title_stripped%"] = Util::getStrippedString(dlc_title);
    templates["%platform%"] = platform;
    templates["%gamename_transformed%"] = gamename_transformed;
    templates["%gamename_transformed_firstletter%"] = gamename_transformed_firstletter;

    for (auto t : templates)
        Util::replaceAllString(filepath, t.first, t.second);

    Util::replaceAllString(filepath, "//", "/"); // Replace any double slashes with single slash

    return filepath;
}

std::string gameDetails::makeCustomFilepath(const std::string& filename, const gameDetails& gd, const DirectoryConfig& dirConf)
{
    gameFile gf;
    gf.gamename = gd.gamename;
    gf.path = "/" + filename;
    gf.title = gd.title;
    gf.gamename_basegame = gd.gamename_basegame;
    gf.title_basegame = gd.title_basegame;

    if (gf.gamename_basegame.empty())
        gf.type = GlobalConstants::GFTYPE_CUSTOM_BASE;
    else
        gf.type = GlobalConstants::GFTYPE_CUSTOM_DLC;

    std::string filepath;
    filepath = this->makeFilepath(gf, dirConf);

    return filepath;
}
