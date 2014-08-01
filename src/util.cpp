/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "util.h"

#include <boost/filesystem.hpp>
#include <tinyxml.h>
#include <jsoncpp/json/json.h>
#include <fstream>

/*
    Create filepath from specified directory and path
    Remove the leading slash from path if needed
    Use gamename as base directory if specified
*/
std::string Util::makeFilepath(const std::string& directory, const std::string& path, const std::string& gamename, std::string subdirectory)
{
    return directory + makeRelativeFilepath(path, gamename, subdirectory);
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
        filepath = gamename + subdirectory + "/" + filename;
    }

    return filepath;
}

std::string Util::getFileHash(const std::string& filename, unsigned hash_id)
{
    unsigned char digest[rhash_get_digest_size(hash_id)];
    char result[rhash_get_hash_length(hash_id)];

    rhash_library_init();
    int i = rhash_file(hash_id, filename.c_str(), digest);
    if (i < 0)
        std::cout << "LibRHash error: " << strerror(errno) << std::endl;
    else
        rhash_print_bytes(result, digest, rhash_get_digest_size(hash_id), RHPR_HEX);

    return result;
}

std::string Util::getChunkHash(unsigned char *chunk, size_t chunk_size, unsigned hash_id)
{
    unsigned char digest[rhash_get_digest_size(hash_id)];
    char result[rhash_get_hash_length(hash_id)];

    rhash_library_init();
    int i = rhash_msg(hash_id, chunk, chunk_size, digest);
    if (i < 0)
        std::cout << "LibRHash error: " << strerror(errno) << std::endl;
    else
        rhash_print_bytes(result, digest, rhash_get_digest_size(hash_id), RHPR_HEX);

    return result;
}

// Create GOG XML
int Util::createXML(std::string filepath, size_t chunk_size, std::string xml_dir)
{
    int res = 0;
    FILE *infile;
    FILE *xmlfile;
    size_t filesize, size;
    int chunks, i;

    if (xml_dir.empty())
    {
        char *xdgcache = getenv("XDG_CACHE_HOME");
        if (xdgcache)
            xml_dir = (std::string)xdgcache + "/lgogdownloader/xml";
        else
        {
            std::string home = (std::string)getenv("HOME");
            xml_dir = home + "/.cache/lgogdownloader/xml";
        }
    }

    // Make sure directory exists
    boost::filesystem::path path = xml_dir;
    if (!boost::filesystem::exists(path)) {
        if (!boost::filesystem::create_directories(path)) {
            std::cout << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    if ((infile=fopen(filepath.c_str(), "r"))!=NULL) {
        //File exists
        fseek(infile, 0, SEEK_END);
        filesize = ftell(infile);
        rewind(infile);
    } else {
        std::cout << filepath << " doesn't exist" << std::endl;
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
                << "Chunk size: " << (chunk_size >> 20) << " MB" << std::endl
                << "MD5: " << std::flush;
    std::string file_md5 = Util::getFileHash(filepath.c_str(), RHASH_MD5);
    std::cout << file_md5 << std::endl;

    TiXmlDocument xml;
    TiXmlElement *fileElem = new TiXmlElement("file");
    fileElem->SetAttribute("name", filename);
    fileElem->SetAttribute("md5", file_md5);
    fileElem->SetAttribute("chunks", chunks);
    fileElem->SetAttribute("total_size", std::to_string(filesize));

    std::cout << "Getting MD5 for chunks" << std::endl;
    for (i = 0; i < chunks; i++) {
        size_t range_begin = i*chunk_size;
        fseek(infile, range_begin, SEEK_SET);
        if ((i == chunks-1) && (remaining != 0))
            chunk_size = remaining;
        size_t range_end = range_begin + chunk_size - 1;
        unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
        if (chunk == NULL)
        {
            std::cout << "Memory error" << std::endl;
            return res;
        }
        size = fread(chunk, 1, chunk_size, infile);
        if (size != chunk_size)
        {
            std::cout << "Read error" << std::endl;
            free(chunk);
            return res;
        }
        std::string hash = Util::getChunkHash(chunk, chunk_size, RHASH_MD5);
        free(chunk);

        TiXmlElement *chunkElem = new TiXmlElement("chunk");
        chunkElem->SetAttribute("id", i);
        chunkElem->SetAttribute("from", std::to_string(range_begin));
        chunkElem->SetAttribute("to", std::to_string(range_end));
        chunkElem->SetAttribute("method", "md5");
        TiXmlText *text = new TiXmlText(hash);
        chunkElem->LinkEndChild(text);
        fileElem->LinkEndChild(chunkElem);

        std::cout << "Chunks hashed " << (i+1) << " / " << chunks << "\r" << std::flush;
    }
    fclose(infile);
    xml.LinkEndChild(fileElem);

    std::cout << std::endl << "Writing XML: " << filenameXML << std::endl;
    if ((xmlfile=fopen(filenameXML.c_str(), "w"))!=NULL) {
        xml.Print(xmlfile);
        fclose(xmlfile);
        res = 1;
    } else {
        std::cout << "Can't create " << filenameXML << std::endl;
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
        char *xdghome = getenv("XDG_CONFIG_HOME");
        if (xdghome)
            directory = (std::string)xdghome + "/lgogdownloader";
        else
        {
            std::string home = (std::string)getenv("HOME");
            directory = home + "/.config/lgogdownloader";
        }
    }

    std::string filepath = directory + "/" + gamename + ".conf";

    // Make sure file exists
    boost::filesystem::path path = filepath;
    if (!boost::filesystem::exists(path)) {
        return res;
    }

    std::ifstream json(filepath, std::ifstream::binary);
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    if (jsonparser->parse(json, root))
    {
        if (root.isMember("language"))
        {
            conf->iInstallerLanguage = root["language"].asUInt();
            res++;
        }
        if (root.isMember("platform"))
        {
            conf->iInstallerType = root["platform"].asUInt();
            res++;
        }
        if (root.isMember("dlc"))
        {
            conf->bDLC = root["dlc"].asBool();
            res++;
        }
    }
    else
    {
        std::cout << "Failed to parse game specific config" << std::endl;
        std::cout << jsonparser->getFormatedErrorMessages() << std::endl;
    }
    delete jsonparser;
    if (json)
        json.close();

    return res;
}
