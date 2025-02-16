/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef BLACKLIST_H__
#define BLACKLIST_H__

#include <boost/regex.hpp>
#include <string>
#include <vector>

class Config;
class gameFile;

class BlacklistItem {
    public:
        unsigned int linenr; // where the blacklist item is defined in blacklist.txt
        unsigned int flags;
        std::string source; // source representation of the item
        boost::regex regex;
};

class Blacklist
{
    public:
        Blacklist() {};

        void initialize(const std::vector<std::string>& lines);
        bool isBlacklisted(const std::string& path);

        std::vector<BlacklistItem>::size_type size() const { return blacklist_.size(); }
        bool empty() { return blacklist_.empty(); }
    private:
        std::vector<BlacklistItem> blacklist_;
};

#endif // BLACKLIST_H_
