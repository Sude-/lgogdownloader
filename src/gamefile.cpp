/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "gamefile.h"

gameFile::gameFile()
{
    this->platform = GlobalConstants::PLATFORM_WINDOWS;
    this->language = GlobalConstants::LANGUAGE_EN;
    this->silent = 0;
    this->type = 0;
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
    json["gamename"] = this->gamename;
    json["type"] = this->type;
    json["galaxy_downlink_json_url"] = this->galaxy_downlink_json_url;

    return json;
}
