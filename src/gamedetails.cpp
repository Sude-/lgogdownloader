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

    for (unsigned int i = 0; i < this->installers.size(); ++i)
    {
        filepath = Util::makeFilepath(config.sDirectory, this->installers[i].path, this->gamename);
        this->installers[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->extras.size(); ++i)
    {
        filepath = Util::makeFilepath(config.sDirectory, this->extras[i].path, this->gamename, config.bSubDirectories ? "extras" : "");
        this->extras[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->patches.size(); ++i)
    {
        filepath = Util::makeFilepath(config.sDirectory, this->patches[i].path, this->gamename, config.bSubDirectories ? "patches" : "");
        this->patches[i].setFilepath(filepath);
    }

    for (unsigned int i = 0; i < this->languagepacks.size(); ++i)
    {
        filepath = Util::makeFilepath(config.sDirectory, this->languagepacks[i].path, this->gamename, config.bSubDirectories ? "languagepacks" : "");
    }

    for (unsigned int i = 0; i < this->dlcs.size(); ++i)
    {
        for (unsigned int j = 0; j < this->dlcs[i].installers.size(); ++j)
        {
            filepath = Util::makeFilepath(config.sDirectory, this->dlcs[i].installers[j].path, this->gamename, config.bSubDirectories ? "dlc/" + this->dlcs[i].gamename : "");
            this->dlcs[i].installers[j].setFilepath(filepath);
        }

        for (unsigned int j = 0; j < this->dlcs[i].patches.size(); ++j)
        {
            filepath = Util::makeFilepath(config.sDirectory, this->dlcs[i].patches[j].path, this->gamename, config.bSubDirectories ? "dlc/" + this->dlcs[i].gamename + "/patches" : "");
            this->dlcs[i].patches[j].setFilepath(filepath);
        }

        for (unsigned int j = 0; j < this->dlcs[i].extras.size(); ++j)
        {
            filepath = Util::makeFilepath(config.sDirectory, this->dlcs[i].extras[j].path, this->gamename, config.bSubDirectories ? "dlc/" + this->dlcs[i].gamename + "/extras" : "");
            this->dlcs[i].extras[j].setFilepath(filepath);
        }
    }
}
