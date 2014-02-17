/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef UTIL_H
#define UTIL_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <rhash.h>

namespace Util
{
    std::string makeFilepath(const std::string& directory, const std::string& path, const std::string& gamename, std::string subdirectory = "");
    std::string getFileHash(const std::string& filename, unsigned hash_id);
    std::string getChunkHash(unsigned char* chunk, size_t chunk_size, unsigned hash_id);
    int createXML(std::string filepath, size_t chunk_size, std::string xml_dir = std::string());
}

#endif // UTIL_H
