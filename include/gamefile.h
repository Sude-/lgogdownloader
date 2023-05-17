/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef GAMEFILE_H
#define GAMEFILE_H

#include "globalconstants.h"
#include "globals.h"

#include <iostream>
#include <vector>
#include <json/json.h>

class gameFile
{
    public:
        gameFile();
        int updated;
        std::string gamename;
        std::string id;
        std::string name;
        std::string path;
        std::string size;
        std::string galaxy_downlink_json_url;
        std::string version;
        unsigned int platform;
        unsigned int language;
        unsigned int type;
        int score;
        int silent;
        void setFilepath(const std::string& path);
        std::string getFilepath();
        Json::Value getAsJson();
        virtual ~gameFile();
    protected:
    private:
        std::string filepath;
};

#endif // GAMEFILE_H
