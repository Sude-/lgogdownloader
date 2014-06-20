/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "blacklist.h"
#include "config.h"
#include "api.h"
#include "util.h"

#include <iostream>
#include <utility>

enum {
    BLFLAG_RX = 1 << 0,
    BLFLAG_PERL = 1 << 1
};

void Blacklist::initialize(const std::vector<std::string>& lines) {
    int linenr = 1;
    for (auto it = lines.begin(); it != lines.end(); ++it, ++linenr) {
        BlacklistItem item;
        const std::string& s = *it;

        if (s.length() == 0 || s[0] == '#')
          continue;

        std::size_t i;
        for (i = 0; i < s.length() && s[i] != '\x20'; ++i) {
            switch (s[i]) {
                case 'R':
                    item.flags |= BLFLAG_RX;
                    break;
                case 'p':
                    item.flags |= BLFLAG_PERL;
                    break;
                default:
                    std::cout << "unknown flag '" << s[i] << "' in blacklist line " << linenr << std::endl;
                    break;
            }
        }
        ++i;
        if (i == s.length()) {
            std::cout << "empty expression in blacklist line " << linenr << std::endl;
            continue;
        }
        if (item.flags & BLFLAG_RX) {
            boost::regex::flag_type rx_flags = boost::regex::normal;

            // we only support perl-like syntax for now, which is boost default (normal). Add further flag processing
            // here if that changes.

            rx_flags |= boost::regex::nosubs;

            item.linenr = linenr;
            item.source.assign(s.substr(i).c_str());
            item.regex.assign(item.source, rx_flags);
            blacklist_.push_back(std::move(item));
        } else {
            std::cout << "unknown expression type in blacklist line " << linenr << std::endl;
        }
    }
}

bool Blacklist::isBlacklisted(const std::string& path) {
    for (auto it = blacklist_.begin(); it != blacklist_.end(); ++it) {
        const BlacklistItem& item = *it;
        if (item.flags & BLFLAG_RX && boost::regex_search(path, item.regex))
            return true;
    }
    return false;
}

bool Blacklist::isBlacklisted(const std::string& path, const std::string& gamename, std::string subdirectory)
{
    std::string filepath = Util::makeRelativeFilepath(path, gamename, subdirectory);
    return isBlacklisted(filepath);
}

