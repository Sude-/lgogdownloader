/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef FILELIST_H
#define FILELIST_H

#include <boost/regex.hpp>
#include <string>
#include <vector>

class Config;
class gameFile;

class FilelistItem {
    public:
        unsigned int linenr; // where the blacklist item is defined in black/white list.txt
        unsigned int flags;
        std::string source; // source representation of the item
        boost::regex regex;
};

class Filelist
{
    public:
        Filelist(){};

        void initialize(const std::vector<std::string>& lines);
        bool Matches(const std::string& path);
        bool Matches(const std::string& path, const std::string& gamename, std::string subdirectory = "");

        std::vector<FilelistItem>::size_type size() const { return files.size(); }
        bool empty() { return files.empty(); }
    private:
        std::vector<FilelistItem> files;
};

#endif  // FILELIST_H
