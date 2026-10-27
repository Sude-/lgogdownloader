/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef OBSOLETE_H
#define OBSOLETE_H

#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/regex.hpp>

namespace Obsolete
{
    std::string pathKey(const boost::filesystem::path& path);
    bool isWithin(const boost::filesystem::path& root, const boost::filesystem::path& path);
    std::string relativeKey(const boost::filesystem::path& root, const boost::filesystem::path& path);
    std::vector<boost::filesystem::path> collectCandidates(
        const boost::filesystem::path& root,
        const boost::regex& expression
    );
    std::vector<boost::filesystem::path> findObsolete(
        const std::vector<boost::filesystem::path>& candidates,
        const std::set<std::string>& selected,
        const std::set<std::string>& current
    );
}

#endif // OBSOLETE_H
