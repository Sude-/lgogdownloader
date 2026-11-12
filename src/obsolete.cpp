/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "obsolete.h"

#include <algorithm>
#include <system_error>

#include <boost/filesystem.hpp>

std::string Obsolete::pathKey(const boost::filesystem::path& path)
{
    boost::filesystem::path normalized = boost::filesystem::absolute(path).lexically_normal();
    while (normalized != normalized.root_path() && normalized.filename() == ".")
        normalized = normalized.parent_path();
    std::string key = normalized.generic_string();
    const std::size_t root_length = normalized.root_path().generic_string().size();
    while (key.size() > root_length && key.back() == '/')
        key.pop_back();
    return key;
}

bool Obsolete::isWithin(const boost::filesystem::path& root, const boost::filesystem::path& path)
{
    const std::string root_key = pathKey(root);
    const std::string path_key = pathKey(path);
    if (root_key == path_key)
        return true;
    return path_key.size() > root_key.size()
        && path_key.compare(0, root_key.size(), root_key) == 0
        && path_key[root_key.size()] == '/';
}

std::string Obsolete::relativeKey(const boost::filesystem::path& root, const boost::filesystem::path& path)
{
    const std::string root_key = pathKey(root);
    const std::string path_key = pathKey(path);
    if (!isWithin(root, path))
        return path_key;
    if (root_key == path_key)
        return std::string();
    return path_key.substr(root_key.size() + 1);
}

std::vector<boost::filesystem::path> Obsolete::collectCandidates(
    const boost::filesystem::path& root,
    const boost::regex& expression
)
{
    std::vector<boost::filesystem::path> candidates;
    if (!boost::filesystem::exists(root) || !boost::filesystem::is_directory(root))
        return candidates;

    boost::filesystem::recursive_directory_iterator end;
    for (boost::filesystem::recursive_directory_iterator it(root); it != end; ++it)
    {
        boost::system::error_code ec;
        if (!boost::filesystem::is_regular_file(it->path(), ec) || ec)
            continue;

        const std::string filepath = it->path().generic_string();
        if (boost::regex_search(filepath, expression))
            candidates.push_back(it->path());
    }

    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

std::vector<boost::filesystem::path> Obsolete::findObsolete(
    const std::vector<boost::filesystem::path>& candidates,
    const std::set<std::string>& selected,
    const std::set<std::string>& current
)
{
    std::vector<boost::filesystem::path> obsolete;
    for (const auto& candidate : candidates)
    {
        const std::string key = pathKey(candidate);
        if (selected.find(key) == selected.end() && current.find(key) == current.end())
            obsolete.push_back(candidate);
    }
    return obsolete;
}
