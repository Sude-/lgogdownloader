#include "gamefile.h"

gameFile::gameFile(const int& t_updated, const std::string& t_id, const std::string& t_name, const std::string& t_path, const std::string& t_size, const unsigned int& t_language, const unsigned int& t_platform, const int& t_silent)
{
    this->updated = t_updated;
    this->id = t_id;
    this->name = t_name;
    this->path = t_path;
    this->size = t_size;
    this->platform = t_platform;
    this->language = t_language;
    this->silent = t_silent;
}

gameFile::~gameFile()
{
    //dtor
}
