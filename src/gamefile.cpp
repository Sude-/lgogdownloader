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

gameFile::gameFile()
{
    //ctor
}

gameFile::~gameFile()
{
    //dtor
}

void gameFile::setFilepath(const std::string& path)
{
    this->filepath = path;
}

std::string gameFile::getFilepath()
{
    return this->filepath;
}

Json::Value gameFile::getAsJson()
{
    Json::Value json;

    json["updated"] = this->updated;
    json["id"] = this->id;
    json["name"] = this->name;
    json["path"] = this->path;
    json["size"] = this->size;
    json["platform"] = this->platform;
    json["language"] = this->language;
    json["silent"] = this->silent;

    return json;
}
