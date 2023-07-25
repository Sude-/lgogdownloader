/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "util.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <tinyxml2.h>
#include <json/json.h>
#include <fstream>
#include <sys/ioctl.h>

/*
    Create filepath from specified directory and path
    Remove the leading slash from path if needed
    Use gamename as base directory if specified
*/
std::string Util::makeFilepath(const std::string& directory, const std::string& path, const std::string& gamename, std::string subdirectory, const unsigned int& platformId, const std::string& dlcname)
{
    std::string dir = directory + makeRelativeFilepath(path, gamename, subdirectory);
    Util::filepathReplaceReservedStrings(dir, gamename, platformId, dlcname);
    return dir;
}

/* Create filepath relative to download base directory specified in config.
 */
std::string Util::makeRelativeFilepath(const std::string& path, const std::string& gamename, std::string subdirectory)
{
    std::string filepath;

    if (gamename.empty())
    {
        if (path.at(0)=='/')
        {
            std::string tmp_path = path.substr(1,path.length());
            filepath = tmp_path;
        }
        else
        {
            filepath = path;
        }
    }
    else
    {
        std::string filename = path.substr(path.find_last_of("/")+1, path.length());
        if (!subdirectory.empty())
        {
            subdirectory = "/" + subdirectory;
        }
        filepath = subdirectory + "/" + filename;
    }

    return filepath;
}

std::string Util::getFileHash(const std::string& filename, unsigned hash_id)
{
    unsigned char digest[rhash_get_digest_size(hash_id)];
    char result[rhash_get_hash_length(hash_id) + 1];

    int i = rhash_file(hash_id, filename.c_str(), digest);
    if (i < 0)
        std::cerr << "LibRHash error: " << strerror(errno) << std::endl;
    else
        rhash_print_bytes(result, digest, rhash_get_digest_size(hash_id), RHPR_HEX);

    return result;
}

std::string Util::getFileHashRange(const std::string& filepath, unsigned hash_id, off_t range_start, off_t range_end)
{
    char result[rhash_get_hash_length(hash_id) + 1];

    if (!boost::filesystem::exists(filepath))
        return result;

    off_t filesize = boost::filesystem::file_size(filepath);

    if (range_end == 0 || range_end > filesize)
        range_end = filesize;

    if (range_end < range_start)
    {
        off_t tmp = range_start;
        range_start = range_end;
        range_end = tmp;
    }

    off_t chunk_size = 10 << 20; // 10MB
    off_t rangesize = range_end - range_start;
    off_t remaining = rangesize % chunk_size;
    int chunks = (remaining == 0) ? rangesize/chunk_size : (rangesize/chunk_size)+1;

    rhash rhash_context;
    rhash_context = rhash_init(hash_id);

    FILE *infile = fopen(filepath.c_str(), "r");

    for (int i = 0; i < chunks; i++)
    {
        off_t chunk_begin = range_start + i*chunk_size;
        fseek(infile, chunk_begin, SEEK_SET);
        if ((i == chunks-1) && (remaining != 0))
            chunk_size = remaining;

        unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
        if (chunk == NULL)
        {
            std::cerr << "Memory error" << std::endl;
            fclose(infile);
            return result;
        }
        off_t size = fread(chunk, 1, chunk_size, infile);
        if (size != chunk_size)
        {
            std::cerr << "Read error" << std::endl;
            free(chunk);
            fclose(infile);
            return result;
        }

        rhash_update(rhash_context, chunk, chunk_size);
        free(chunk);
    }
    fclose(infile);

    rhash_final(rhash_context, NULL);
    rhash_print(result, rhash_context, hash_id, RHPR_HEX);
    rhash_free(rhash_context);

    return result;
}

std::string Util::getChunkHash(unsigned char *chunk, uintmax_t chunk_size, unsigned hash_id)
{
    unsigned char digest[rhash_get_digest_size(hash_id)];
    char result[rhash_get_hash_length(hash_id) + 1];

    int i = rhash_msg(hash_id, chunk, chunk_size, digest);
    if (i < 0)
        std::cerr << "LibRHash error: " << strerror(errno) << std::endl;
    else
        rhash_print_bytes(result, digest, rhash_get_digest_size(hash_id), RHPR_HEX);

    return result;
}

// Create GOG XML
int Util::createXML(std::string filepath, uintmax_t chunk_size, std::string xml_dir)
{
    int res = 0;
    FILE *infile;
    FILE *xmlfile;
    uintmax_t filesize, size;
    int chunks, i;

    if (xml_dir.empty())
    {
        xml_dir = Util::getCacheHome() + "/lgogdownloader/xml";
    }

    // Make sure directory exists
    boost::filesystem::path path = xml_dir;
    if (!boost::filesystem::exists(path)) {
        if (!boost::filesystem::create_directories(path)) {
            std::cerr << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    if ((infile=fopen(filepath.c_str(), "r"))!=NULL) {
        //File exists
        fseek(infile, 0, SEEK_END);
        filesize = ftell(infile);
        rewind(infile);
    } else {
        std::cerr << filepath << " doesn't exist" << std::endl;
        return res;
    }

    // Get filename
    boost::filesystem::path pathname = filepath;
    std::string filename = pathname.filename().string();
    std::string filenameXML = xml_dir + "/" + filename + ".xml";

    std::cout << filename << std::endl;
    //Determine number of chunks
    int remaining = filesize % chunk_size;
    chunks = (remaining == 0) ? filesize/chunk_size : (filesize/chunk_size)+1;
    std::cout   << "Filesize: " << filesize << " bytes" << std::endl
                << "Chunks: " << chunks << std::endl
                << "Chunk size: " << (chunk_size >> 20) << " MB" << std::endl;

    tinyxml2::XMLDocument xml;
    tinyxml2::XMLElement *fileElem = xml.NewElement("file");
    fileElem->SetAttribute("name", filename.c_str());
    fileElem->SetAttribute("chunks", chunks);
    fileElem->SetAttribute("total_size", std::to_string(filesize).c_str());

    std::cout << "Getting MD5 for chunks" << std::endl;

    rhash rhash_context;
    rhash_context = rhash_init(RHASH_MD5);
    if(!rhash_context)
    {
        std::cerr << "error: couldn't initialize rhash context" << std::endl;
        fclose(infile);
        return res;
    }
    char rhash_result[rhash_get_hash_length(RHASH_MD5) + 1];

    for (i = 0; i < chunks; i++) {
        uintmax_t range_begin = i*chunk_size;
        fseek(infile, range_begin, SEEK_SET);
        if ((i == chunks-1) && (remaining != 0))
            chunk_size = remaining;
        uintmax_t range_end = range_begin + chunk_size - 1;
        unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
        if (chunk == NULL)
        {
            std::cerr << "Memory error" << std::endl;
            fclose(infile);
            return res;
        }
        size = fread(chunk, 1, chunk_size, infile);
        if (size != chunk_size)
        {
            std::cerr << "Read error" << std::endl;
            free(chunk);
            fclose(infile);
            return res;
        }

        std::string hash = Util::getChunkHash(chunk, chunk_size, RHASH_MD5);
        rhash_update(rhash_context, chunk, chunk_size); // Update hash for the whole file

        free(chunk);

        tinyxml2::XMLElement *chunkElem = xml.NewElement("chunk");
        chunkElem->SetAttribute("id", i);
        chunkElem->SetAttribute("from", std::to_string(range_begin).c_str());
        chunkElem->SetAttribute("to", std::to_string(range_end).c_str());
        chunkElem->SetAttribute("method", "md5");
        tinyxml2::XMLText *text = xml.NewText(hash.c_str());
        chunkElem->LinkEndChild(text);
        fileElem->LinkEndChild(chunkElem);

        std::cout << "Chunks hashed " << (i+1) << " / " << chunks << "\r" << std::flush;
    }
    fclose(infile);

    rhash_final(rhash_context, NULL);
    rhash_print(rhash_result, rhash_context, RHASH_MD5, RHPR_HEX);
    rhash_free(rhash_context);

    std::cout << std::endl << "MD5: " << rhash_result << std::endl;
    fileElem->SetAttribute("md5", rhash_result);

    xml.LinkEndChild(fileElem);

    std::cout << "Writing XML: " << filenameXML << std::endl;
    if ((xmlfile=fopen(filenameXML.c_str(), "w"))!=NULL) {
        tinyxml2::XMLPrinter printer(xmlfile);
        xml.Print(&printer);
        fclose(xmlfile);
        res = 1;
    } else {
        std::cerr << "Can't create " << filenameXML << std::endl;
        return res;
    }

    return res;
}

/*
    Overrides global settings with game specific settings
    returns 0 if fails
    returns number of changed settings if succesful
*/
int Util::getGameSpecificConfig(std::string gamename, gameSpecificConfig* conf, std::string directory)
{
    int res = 0;

    if (directory.empty())
    {
        directory = Util::getConfigHome() + "/lgogdownloader/gamespecific";
    }

    std::string filepath = directory + "/" + gamename + ".conf";

    // Make sure file exists
    boost::filesystem::path path = filepath;
    if (!boost::filesystem::exists(path)) {
        return res;
    }

    std::ifstream json(filepath, std::ifstream::binary);
    Json::Value root;
    try {
        json >> root;
    } catch (const Json::Exception& exc) {
        std::cerr << "Failed to parse game specific config " << filepath << std::endl;
        std::cerr << exc.what() << std::endl;
        return res;
    }

    if (root.isMember("language"))
    {
        if (root["language"].isInt())
            conf->dlConf.iInstallerLanguage = root["language"].asUInt();
        else
        {
            Util::parseOptionString(root["language"].asString(), conf->dlConf.vLanguagePriority, conf->dlConf.iInstallerLanguage, GlobalConstants::LANGUAGES);
        }
        res++;
    }
    if (root.isMember("platform"))
    {
        if (root["platform"].isInt())
            conf->dlConf.iInstallerPlatform = root["platform"].asUInt();
        else
        {
            Util::parseOptionString(root["platform"].asString(), conf->dlConf.vPlatformPriority, conf->dlConf.iInstallerPlatform, GlobalConstants::PLATFORMS);
        }
        res++;
    }
    // Warn about deprecated option
    if (root.isMember("dlc"))
    {
        std::cerr << filepath << " contains deprecated option \"dlc\" which will be ignored, use \"include\" instead" << std::endl;
    }
    if (root.isMember("include"))
    {
        conf->dlConf.iInclude = 0;
        std::vector<std::string> vInclude = Util::tokenize(root["include"].asString(), ",");
        for (std::vector<std::string>::iterator it = vInclude.begin(); it != vInclude.end(); it++)
        {
            conf->dlConf.iInclude |= Util::getOptionValue(*it, GlobalConstants::INCLUDE_OPTIONS);
        }
        res++;
    }
    if (root.isMember("ignore-dlc-count"))
    {
        conf->dlConf.bIgnoreDLCCount = root["ignore-dlc-count"].asBool();
        res++;
    }
    if (root.isMember("subdirectories"))
    {
        conf->dirConf.bSubDirectories = root["subdirectories"].asBool();
        res++;
    }
    if (root.isMember("directory"))
    {
        conf->dirConf.sDirectory = root["directory"].asString();
        res++;
    }
    if (root.isMember("subdir-game"))
    {
        conf->dirConf.sGameSubdir = root["subdir-game"].asString();
        res++;
    }
    if (root.isMember("subdir-installers"))
    {
        conf->dirConf.sInstallersSubdir = root["subdir-installers"].asString();
        res++;
    }
    if (root.isMember("subdir-extras"))
    {
        conf->dirConf.sExtrasSubdir = root["subdir-extras"].asString();
        res++;
    }
    if (root.isMember("subdir-patches"))
    {
        conf->dirConf.sPatchesSubdir = root["subdir-patches"].asString();
        res++;
    }
    if (root.isMember("subdir-language-packs"))
    {
        conf->dirConf.sLanguagePackSubdir = root["subdir-language-packs"].asString();
        res++;
    }
    if (root.isMember("subdir-dlc"))
    {
        conf->dirConf.sDLCSubdir = root["subdir-dlc"].asString();
        res++;
    }
    return res;
}

int Util::replaceString(std::string& str, const std::string& to_replace, const std::string& replace_with)
{
    size_t pos = str.find(to_replace);
    if (pos == std::string::npos)
    {
        return 0;
    }
    str.replace(str.begin()+pos, str.begin()+pos+to_replace.length(), replace_with);
    return 1;
}

int Util::replaceAllString(std::string& str, const std::string& to_replace, const std::string& replace_with) {
    size_t pos = str.find(to_replace);
    if (pos == std::string::npos)
    {
        return 0;
    }

    do {
        str.replace(str.begin()+pos, str.begin()+pos+to_replace.length(), replace_with);

        pos = str.find(to_replace, pos + to_replace.length());
    } while(pos != std::string::npos);

    return 1;
}

void Util::filepathReplaceReservedStrings(std::string& str, const std::string& gamename, const unsigned int& platformId, const std::string& dlcname)
{
    std::string platform;
    for (unsigned int i = 0; i < GlobalConstants::PLATFORMS.size(); ++i)
    {
        if ((platformId & GlobalConstants::PLATFORMS[i].id) == GlobalConstants::PLATFORMS[i].id)
        {
            platform = boost::algorithm::to_lower_copy(GlobalConstants::PLATFORMS[i].str);
            break;
        }
    }
    if (platform.empty())
    {
        if (str.find("%gamename%/%platform%") != std::string::npos)
            platform = "";
        else
            platform = "no_platform";
    }

    std::string gamename_firstletter;
    if (!gamename.empty())
    {
        if (std::isdigit(gamename.front()))
            gamename_firstletter = "0";
        else
            gamename_firstletter = gamename.front();
    }

    if (str.find("%gamename_transformed%") != std::string::npos || str.find("%gamename_transformed_firstletter%") != std::string::npos)
    {
        std::string gamename_transformed = transformGamename(gamename);
        std::string gamename_transformed_firstletter;
        if (!gamename_transformed.empty())
        {
            if (std::isdigit(gamename_transformed.front()))
                gamename_transformed_firstletter = "0";
            else
                gamename_transformed_firstletter = gamename_transformed.front();
        }

        while (Util::replaceString(str, "%gamename_transformed%", gamename_transformed));
        while (Util::replaceString(str, "%gamename_transformed_firstletter%", gamename_transformed_firstletter));
    }

    while (Util::replaceString(str, "%gamename_firstletter%", gamename_firstletter));
    while (Util::replaceString(str, "%gamename%", gamename));
    while (Util::replaceString(str, "%dlcname%", dlcname));
    while (Util::replaceString(str, "%platform%", platform));
    while (Util::replaceString(str, "//", "/")); // Replace any double slashes with single slash
}

void Util::setFilePermissions(const boost::filesystem::path& path, const boost::filesystem::perms& permissions)
{
    if (boost::filesystem::exists(path))
    {
        if (boost::filesystem::is_regular_file(path))
        {
            boost::filesystem::file_status s = boost::filesystem::status(path);
            if (s.permissions() != permissions)
            {
                boost::system::error_code ec;
                boost::filesystem::permissions(path, permissions, ec);
                if (ec)
                {
                    std::cerr << "Failed to set file permissions for " << path.string() << std::endl;
                }
            }
        }
    }
}

int Util::getTerminalWidth()
{
    int width;
    if(isatty(STDOUT_FILENO))
    {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        width = static_cast<int>(w.ws_col);
    }
    else
        width = 10000;//Something sufficiently big
    return width;
}


void Util::getManualUrlsFromJSON(const Json::Value &root, std::vector<std::string> &urls)
{
    if(root.size() > 0) {
        for(Json::ValueConstIterator it = root.begin() ; it != root.end() ; ++it)
        {
            if (it.key() == "manualUrl")
            {
                Json::Value url = *it;
                urls.push_back(url.asString());
            }
            else
                getManualUrlsFromJSON(*it, urls);
        }
    }
    return;
}

std::vector<std::string> Util::getDLCNamesFromJSON(const Json::Value &root)
{
    std::vector<std::string> urls, dlcnames;
    getManualUrlsFromJSON(root, urls);

    for (unsigned int i = 0; i < urls.size(); ++i)
    {
        std::string gamename;
        std::string url_prefix = "/downloads/";

        gamename.assign(urls[i].begin()+urls[i].find(url_prefix)+url_prefix.length(), urls[i].begin()+urls[i].find_last_of("/"));
        bool bDuplicate = false;
        for (unsigned int j = 0; j < dlcnames.size(); ++j)
        {
            if (gamename == dlcnames[j])
            {
                bDuplicate = true;
                break;
            }
        }
        if (!bDuplicate)
            dlcnames.push_back(gamename);
    }
    return dlcnames;
}

std::string Util::getHomeDir()
{
    return (std::string)getenv("HOME");
}

std::string Util::getConfigHome()
{
    std::string configHome;
    char *xdgconfig = getenv("XDG_CONFIG_HOME");
    if (xdgconfig)
        configHome = (std::string)xdgconfig;
    else
        configHome = Util::getHomeDir() + "/.config";
    return configHome;
}

std::string Util::getCacheHome()
{
    std::string cacheHome;
    char *xdgcache = getenv("XDG_CACHE_HOME");
    if (xdgcache)
        cacheHome = (std::string)xdgcache;
    else
        cacheHome = Util::getHomeDir() + "/.cache";
    return cacheHome;
}

std::vector<std::string> Util::tokenize(const std::string& str, const std::string& separator)
{
    std::vector<std::string> tokens;
    std::string token;
    size_t idx = 0, found;
    while ((found = str.find(separator, idx)) != std::string::npos)
    {
        token = str.substr(idx, found - idx);
        if (!token.empty())
            tokens.push_back(token);
        idx = found + separator.length();
    }
    token = str.substr(idx);
    if (!token.empty())
        tokens.push_back(token);

    return tokens;
}

unsigned int Util::getOptionValue(const std::string& str, const std::vector<GlobalConstants::optionsStruct>& options, const bool& bAllowStringToIntConversion)
{
    unsigned int value = 0;
    boost::regex expression("^[+-]?\\d+$", boost::regex::perl);
    boost::match_results<std::string::const_iterator> what;
    if (str == "all")
    {
        for (unsigned int i = 0; i < options.size(); ++i)
            value |= options[i].id;
    }
    else if (boost::regex_search(str, what, expression) && bAllowStringToIntConversion)
    {
        value = std::stoi(str);
    }
    else
    {
        for (unsigned int i = 0; i < options.size(); ++i)
        {
            if (!options[i].regexp.empty())
            {
                boost::regex expr("^(" + options[i].regexp + ")$", boost::regex::perl | boost::regex::icase);
                if (boost::regex_search(str, what, expr))
                {
                    value = options[i].id;
                    break;
                }
            }

            if (str == options[i].code)
            {
                value = options[i].id;
                break;
            }
        }
    }
    return value;
}

std::string Util::getOptionNameString(const unsigned int& value, const std::vector<GlobalConstants::optionsStruct>& options)
{
    std::string str;
    for (unsigned int i = 0; i < options.size(); ++i)
    {
        if ((value & options[i].id) == options[i].id)
            str += (str.empty() ? "" : ", ")+options[i].str;
    }
    return str;
}

// Parse the options string
void Util::parseOptionString(const std::string &option_string, std::vector<unsigned int> &priority, unsigned int &type, const std::vector<GlobalConstants::optionsStruct>& options)
{
    type = 0;
    priority.clear();
    std::vector<std::string> tokens_priority = Util::tokenize(option_string, ",");
    for (std::vector<std::string>::iterator it_priority = tokens_priority.begin(); it_priority != tokens_priority.end(); it_priority++)
    {
        unsigned int value = 0;
        std::vector<std::string> tokens_value = Util::tokenize(*it_priority, "+");
        for (std::vector<std::string>::iterator it_value = tokens_value.begin(); it_value != tokens_value.end(); it_value++)
        {
            value |= Util::getOptionValue(*it_value, options);
        }
        priority.push_back(value);
        type |= value;
    }
}

std::string Util::getLocalFileHash(const std::string& xml_dir, const std::string& filepath, const std::string& gamename, const bool& useFastCheck)
{
    std::string localHash;
    boost::filesystem::path path = filepath;
    boost::filesystem::path local_xml_file;
    if (!gamename.empty())
        local_xml_file = xml_dir + "/" + gamename + "/" + path.filename().string() + ".xml";
    else
        local_xml_file = xml_dir + "/" + path.filename().string() + ".xml";

    if (boost::filesystem::exists(local_xml_file) && useFastCheck)
    {
        tinyxml2::XMLDocument local_xml;
        local_xml.LoadFile(local_xml_file.string().c_str());
        tinyxml2::XMLElement *fileElem = local_xml.FirstChildElement("file");

        if (fileElem)
        {
            localHash = fileElem->Attribute("md5");
        }
    }
    else if (boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path))
    {
        localHash = Util::getFileHash(path.string(), RHASH_MD5);
    }

    return localHash;
}

void Util::shortenStringToTerminalWidth(std::string& str)
{
    int iStrLen = static_cast<int>(str.length());
    int iTermWidth = Util::getTerminalWidth();
    if (iStrLen >= iTermWidth)
    {
        size_t chars_to_remove = (iStrLen - iTermWidth) + 4;
        size_t middle = iStrLen / 2;
        size_t pos1 = middle - (chars_to_remove / 2);
        size_t pos2 = middle + (chars_to_remove / 2);
        str.replace(str.begin()+pos1, str.begin()+pos2, "...");
    }
}

std::string Util::getJsonUIntValueAsString(const Json::Value& json_value)
{
    std::string value;
    try
    {
        value = json_value.asString();
    }
    catch (...)
    {
        try
        {
            uintmax_t value_uint = json_value.asLargestUInt();
            value = std::to_string(value_uint);
        }
        catch (...)
        {
            value = "";
        }
    }

    return value;
}

std::string Util::getStrippedString(std::string str)
{
    str.erase(
        std::remove_if(str.begin(), str.end(),
            [](unsigned char c)
            {
                bool bIsValid = false;
                bIsValid = (std::isspace(c) && std::isprint(c)) || std::isalnum(c);
                std::vector<unsigned char> validChars = { '-', '_', '.', '(', ')', '[', ']', '{', '}' };
                if (std::any_of(validChars.begin(), validChars.end(), [c](unsigned char x){return x == c;}))
                {
                    bIsValid = true;
                }
                return !bIsValid;
            }
        ),
        str.end()
    );
    return str;
}

std::string Util::makeEtaString(const unsigned long long& iBytesRemaining, const double& dlRate)
{
    boost::posix_time::time_duration duration(boost::posix_time::seconds((long)(iBytesRemaining / dlRate)));

    return Util::makeEtaString(duration);
}

std::string Util::makeEtaString(const boost::posix_time::time_duration& duration)
{
    std::string etastr;
    std::stringstream eta_ss;

    if (duration.hours() > 23)
    {
       eta_ss << duration.hours() / 24 << "d " <<
                 std::setfill('0') << std::setw(2) << duration.hours() % 24 << "h " <<
                 std::setfill('0') << std::setw(2) << duration.minutes() << "m " <<
                 std::setfill('0') << std::setw(2) << duration.seconds() << "s";
    }
    else if (duration.hours() > 0)
    {
       eta_ss << duration.hours() << "h " <<
                 std::setfill('0') << std::setw(2) << duration.minutes() << "m " <<
                 std::setfill('0') << std::setw(2) << duration.seconds() << "s";
    }
    else if (duration.minutes() > 0)
    {
       eta_ss << duration.minutes() << "m " <<
                 std::setfill('0') << std::setw(2) << duration.seconds() << "s";
    }
    else
    {
       eta_ss << duration.seconds() << "s";
    }
    etastr = eta_ss.str();

    return etastr;
}

void Util::CurlHandleSetDefaultOptions(CURL* curlhandle, const CurlConfig& conf)
{
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, conf.sUserAgent.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, conf.iTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEFILE, conf.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, conf.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, conf.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, conf.iDownloadRate);

    // Assume that we have connection error and abort transfer with CURLE_OPERATION_TIMEDOUT if download speed is less than 200 B/s for 30 seconds
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_TIME, conf.iLowSpeedTimeout);
    curl_easy_setopt(curlhandle, CURLOPT_LOW_SPEED_LIMIT, conf.iLowSpeedTimeoutRate);

    if (!conf.sCACertPath.empty())
        curl_easy_setopt(curlhandle, CURLOPT_CAINFO, conf.sCACertPath.c_str());
}

std::string Util::CurlHandleGetInfoString(CURL* curlhandle, CURLINFO info)
{
    char* str;
    return (curl_easy_getinfo(curlhandle, info, &str) == CURLE_OK) ? str : "";
}

CURLcode Util::CurlGetResponse(const std::string& url, std::string& response, int max_retries)
{
    CURLcode result;
    CURL *handle = curl_easy_init();
    Util::CurlHandleSetDefaultOptions(handle, Globals::globalConfig.curlConf);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());

    result = Util::CurlHandleGetResponse(handle, response, max_retries);

    curl_easy_cleanup(handle);

    return result;
}

CURLcode Util::CurlHandleGetResponse(CURL* curlhandle, std::string& response, int max_retries)
{
    CURLcode result;
    int retries = 0;
    std::ostringstream memory;
    bool bShouldRetry = false;
    long int response_code = 0;
    if (max_retries < 0)
        max_retries = Globals::globalConfig.iRetries;

    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Util::CurlWriteMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);

    do
    {
        if (bShouldRetry)
            retries++;

        if (Globals::globalConfig.iWait > 0)
            usleep(Globals::globalConfig.iWait); // Delay the request by specified time
        result = curl_easy_perform(curlhandle);
        response = memory.str();
        memory.str(std::string());

        switch (result)
        {
            // Retry on these errors
            case CURLE_PARTIAL_FILE:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_RECV_ERROR:
                bShouldRetry = true;
                break;
            // Retry on CURLE_HTTP_RETURNED_ERROR if response code is not "404 Not Found"
            case CURLE_HTTP_RETURNED_ERROR:
                curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                if (response_code == 404 || response_code == 403) {
                    bShouldRetry = false;
                }
                else
                    bShouldRetry = true;
                break;
            default:
                bShouldRetry = false;
                break;
        }
        if (retries >= max_retries)
            bShouldRetry = false;
    } while (bShouldRetry);

    return result;
}

curl_off_t Util::CurlWriteMemoryCallback(char *ptr, curl_off_t size, curl_off_t nmemb, void *userp)
{
    std::ostringstream *stream = (std::ostringstream*)userp;
    std::streamsize count = (std::streamsize) size * nmemb;
    stream->write(ptr, count);
    return count;
}

curl_off_t Util::CurlWriteChunkMemoryCallback(void *contents, curl_off_t size, curl_off_t nmemb, void *userp)
{
    curl_off_t realsize = size * nmemb;
    ChunkMemoryStruct *mem = (ChunkMemoryStruct *)userp;

    mem->memory = (char *) realloc(mem->memory, mem->size + realsize + 1);
    if(mem->memory == NULL)
    {
        std::cout << "Not enough memory (realloc returned NULL)" << std::endl;
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

curl_off_t Util::CurlReadChunkMemoryCallback(void *contents, curl_off_t size, curl_off_t nmemb, ChunkMemoryStruct *mem) {
    curl_off_t realsize = std::min(size * nmemb, mem->size);

    std::copy(mem->memory, mem->memory + realsize, (char*)contents);

    mem->size -= realsize;
    mem->memory += realsize;

    return realsize;
}

std::string Util::makeSizeString(const unsigned long long& iSizeInBytes)
{
    auto units = { "B", "kB", "MB", "GB", "TB", "PB" };
    std::string size_unit = "B";
    double iSize = static_cast<double>(iSizeInBytes);
    for (auto unit : units)
    {
        size_unit = unit;
        if (iSize < 1024)
            break;

        iSize /= 1024;
    }
    return formattedString("%0.2f %s", iSize, size_unit.c_str());
}

Json::Value Util::readJsonFile(const std::string& path)
{
    Json::Value json;
    std::ifstream ifs(path, std::ifstream::binary);

    if (ifs)
    {
        try
        {
            ifs >> json;
        }
        catch (const Json::Exception& exc)
        {
            std::cerr << "Failed to parse " << path << std::endl;
            std::cerr << exc.what() << std::endl;
        }
        ifs.close();
    }
    else
    {
        std::cerr << "Failed to open " << path << std::endl;
    }

    return json;
}

std::string Util::transformGamename(const std::string& gamename)
{
    std::string gamename_transformed = gamename;
    for (auto transformMatch : Globals::globalConfig.transformationsJSON.getMemberNames())
    {
        boost::regex expression(transformMatch);
        boost::match_results<std::string::const_iterator> what;
        if (boost::regex_search(gamename_transformed, what, expression))
        {
            // Get list of exceptions
            std::vector<std::string> vExceptions;
            if (Globals::globalConfig.transformationsJSON[transformMatch].isMember("exceptions"))
            {
                if (Globals::globalConfig.transformationsJSON[transformMatch]["exceptions"].isArray())
                {
                    for (auto exception : Globals::globalConfig.transformationsJSON[transformMatch]["exceptions"])
                        vExceptions.push_back(exception.asString());
                }
                else
                {
                    vExceptions.push_back(Globals::globalConfig.transformationsJSON[transformMatch]["exceptions"].asString());
                }
            }

            // Skip if gamename matches exception
            if (std::any_of(vExceptions.begin(), vExceptions.end(), [gamename](std::string exception){return exception == gamename;}))
                continue;

            boost::regex transformRegex(Globals::globalConfig.transformationsJSON[transformMatch]["regex"].asString());
            std::string transformReplacement = Globals::globalConfig.transformationsJSON[transformMatch]["replacement"].asString();
            gamename_transformed = boost::regex_replace(gamename_transformed, transformRegex, transformReplacement);
        }
    }

    return gamename_transformed;
}
