#ifndef GAMEFILE_H
#define GAMEFILE_H

#include "globalconstants.h"

#include <iostream>
#include <vector>

class gameFile
{
    public:
        gameFile(const int& t_updated, const std::string& t_id, const std::string& t_name, const std::string& t_path, const std::string& t_size, const unsigned int& t_language = GlobalConstants::LANGUAGE_EN, const unsigned int& t_platform = GlobalConstants::PLATFORM_WINDOWS, const int& t_silent = 0);
        int updated;
        std::string id;
        std::string name;
        std::string path;
        std::string size;
        unsigned int platform;
        unsigned int language;
        int silent;
        void setFilepath(const std::string& path);
        std::string getFilepath();
        virtual ~gameFile();
    protected:
    private:
        std::string filepath;
};

#endif // GAMEFILE_H
