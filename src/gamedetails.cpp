#include "gamedetails.h"
#include "util.h"

gameDetails::gameDetails()
{
    //ctor
}

gameDetails::~gameDetails()
{
    //dtor
}

void gameDetails::makeFilepaths(const Config& config)
{
    std::string filepath;
    std::string directory = config.sDirectory + "/" + config.sGameSubdir + "/";
    std::string subdir;

    for (unsigned int i = 0; i < this->installers.size(); ++i)
    {
        subdir = config.bSubDirectories ? config.sInstallersSubdir : "";
        filepath = Util::makeFilepath(directory, this->installers[i].path, this->gamename, subdir, this->installers[i].platform);
        this->installers[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->extras.size(); ++i)
    {
        subdir = config.bSubDirectories ? config.sExtrasSubdir : "";
        filepath = Util::makeFilepath(directory, this->extras[i].path, this->gamename, subdir, 0);
        this->extras[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->patches.size(); ++i)
    {
        subdir = config.bSubDirectories ? config.sPatchesSubdir : "";
        filepath = Util::makeFilepath(directory, this->patches[i].path, this->gamename, subdir, this->patches[i].platform);
        this->patches[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->languagepacks.size(); ++i)
    {
        subdir = config.bSubDirectories ? config.sLanguagePackSubdir : "";
        filepath = Util::makeFilepath(directory, this->languagepacks[i].path, this->gamename, subdir, 0);
        this->languagepacks[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->dlcs.size(); ++i)
    {
        for (unsigned int j = 0; j < this->dlcs[i].installers.size(); ++j)
        {
            subdir = config.bSubDirectories ? config.sDLCSubdir + "/" + config.sInstallersSubdir : "";
            filepath = Util::makeFilepath(directory, this->dlcs[i].installers[j].path, this->gamename, subdir, this->dlcs[i].installers[j].platform, this->dlcs[i].gamename);
            this->dlcs[i].installers[j].setFilepath(filepath);
        }

        for (unsigned int j = 0; j < this->dlcs[i].patches.size(); ++j)
        {
            subdir = config.bSubDirectories ? config.sDLCSubdir + "/" + config.sPatchesSubdir : "";
            filepath = Util::makeFilepath(directory, this->dlcs[i].patches[j].path, this->gamename, subdir, this->dlcs[i].patches[j].platform, this->dlcs[i].gamename);
            this->dlcs[i].patches[j].setFilepath(filepath);
        }

        for (unsigned int j = 0; j < this->dlcs[i].extras.size(); ++j)
        {
            subdir = config.bSubDirectories ? config.sDLCSubdir + "/" + config.sExtrasSubdir : "";
            filepath = Util::makeFilepath(directory, this->dlcs[i].extras[j].path, this->gamename, subdir, 0, this->dlcs[i].gamename);
            this->dlcs[i].extras[j].setFilepath(filepath);
        }
    }
}

Json::Value gameDetails::getDetailsAsJson()
{
    Json::Value json;

    json["gamename"] = this->gamename;
    json["title"] = this->title;
    json["icon"] = this->icon;

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
