/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef GAMEDETAILS_H
#define GAMEDETAILS_H

#include "globalconstants.h"
#include "globals.h"
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
        std::string product_id;
        std::string title;
        std::string icon;
        std::string serials;
        std::string changelog;
        void filterWithPriorities(const gameSpecificConfig& config);
        void makeFilepaths(const DirectoryConfig& config);
        std::string getSerialsFilepath();
        std::string getChangelogFilepath();
        Json::Value getDetailsAsJson();
        std::vector<gameFile> getGameFileVector();
        std::vector<gameFile> getGameFileVectorFiltered(const unsigned int& iType);
        virtual ~gameDetails();
    protected:
        void filterListWithPriorities(std::vector<gameFile>& list, const gameSpecificConfig& config);
    private:
        std::string serialsFilepath;
        std::string changelogFilepath;
};

#endif // GAMEDETAILS_H
