#ifndef GAMEDETAILS_H
#define GAMEDETAILS_H

#include "globalconstants.h"
#include "gamefile.h"
#include "config.h"
#include "util.h"

#include <iostream>
#include <vector>
#include <json/json.h>

class gameDetails
{
    public:
        gameDetails();
        std::vector<gameFile> extras;
        std::vector<gameFile> installers;
        std::vector<gameFile> patches;
        std::vector<gameFile> languagepacks;
        std::vector<gameDetails> dlcs;
        std::string gamename;
        std::string title;
        std::string icon;
        std::string serials;
        void filterWithPriorities(const gameSpecificConfig& config);
        void makeFilepaths(const gameSpecificDirectoryConfig& config);
        std::string getSerialsFilepath();
        Json::Value getDetailsAsJson();
        virtual ~gameDetails();
    protected:
        void filterListWithPriorities(std::vector<gameFile>& list, const gameSpecificConfig& config);
    private:
        std::string serialsFilepath;
};

#endif // GAMEDETAILS_H
