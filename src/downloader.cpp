/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloader.h"
#include "util.h"
#include "globalconstants.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <tinyxml.h>
#include <jsoncpp/json/json.h>
#include <htmlcxx/html/ParserDom.h>

namespace bptime = boost::posix_time;

Downloader::Downloader(Config &conf)
{
    this->config = conf;
}

Downloader::~Downloader()
{
    delete progressbar;
    delete gogAPI;
    curl_easy_cleanup(curlhandle);
    curl_global_cleanup();
}


/* Initialize the downloader
    returns 0 if successful
    returns 1 if failed
*/
int Downloader::init()
{
    this->resume_position = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curlhandle = curl_easy_init();
    curl_easy_setopt(curlhandle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curlhandle, CURLOPT_USERAGENT, config.sVersionString.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(curlhandle, CURLOPT_CONNECTTIMEOUT, 10);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(curlhandle, CURLOPT_FAILONERROR, true);
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEFILE, config.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_COOKIEJAR, config.sCookiePath.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYPEER, config.bVerifyPeer);
    curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, config.bVerbose);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, Downloader::readData);
    curl_easy_setopt(curlhandle, CURLOPT_PROGRESSFUNCTION, Downloader::progressCallback);
    #ifdef ENVIRONMENT64
        curl_easy_setopt(curlhandle, CURLOPT_MAX_RECV_SPEED_LARGE, config.iDownloadRate);
    #endif

    gogAPI = new API(config.sToken, config.sSecret, config.bVerbose, config.bVerifyPeer);
    progressbar = new ProgressBar(!config.bNoUnicode, !config.bNoColor);

    if (config.bLogin || !gogAPI->init())
        return this->login();

    if (!config.bNoCover && config.bDownload && !config.bUpdateCheck)
        coverXML = this->getResponse("https://sites.google.com/site/gogdownloader/GOG_covers_v2.xml");

    if (!config.bUpdateCheck) // updateCheck() calls getGameList() if needed
        this->getGameList();

    return 0;
}

/* Login
    returns 1 if login fails
    returns 0 if successful
*/
int Downloader::login()
{
    char *pwd;
    std::string email;
    std::cout << "Email: ";
    std::getline(std::cin,email);
    pwd = getpass("Password: ");
    std::string password = (std::string)pwd;
    if (email.empty() || password.empty())
    {
        std::cout << "Email and/or password empty" << std::endl;
        return 1;
    }
    else
    {
        // Login to website
        if (!HTTP_Login(email, password))
        {
            std::cout << "HTTP: Login failed" << std::endl;
            return 1;
        }
        else
        {
            std::cout << "HTTP: Login successful" << std::endl;
        }
        // Login to API
        if (!gogAPI->login(email, password))
        {
            std::cout << "API: Login failed" << std::endl;
            return 1;
        }
        else
        {
            std::cout << "API: Login successful" << std::endl;
            // Save token and secret to config file
            std::ofstream ofs(config.sConfigFilePath.c_str());
            if (ofs)
            {
                ofs << "token = " << gogAPI->getToken() << std::endl << "secret = " << gogAPI->getSecret() << std::endl;
                ofs.close();
                return 0;
            }
            else
            {
                std::cout << "Failed to create config: " << config.sConfigFilePath << std::endl;
                return 1;
            }
        }
    }
}

void Downloader::updateCheck()
{
    if (gogAPI->user.notifications_forum)
    {
        std::cout << gogAPI->user.notifications_forum << " new forum replies" << std::endl;
    }
    else
    {
        std::cout << "No new forum replies" << std::endl;
    }
    if (gogAPI->user.notifications_messages)
    {
        std::cout << gogAPI->user.notifications_messages << " new private message(s)" << std::endl;
    }
    else
    {
        std::cout << "No new private messages" << std::endl;
    }
    if (gogAPI->user.notifications_games)
    {
        std::cout << gogAPI->user.notifications_games << " updated game(s)" << std::endl;
    }
    else
    {
        std::cout << "No updated games" << std::endl;
    }

    if (gogAPI->user.notifications_games)
    {
        config.sGameRegex = ".*"; // Always check all games
        if (config.bList || config.bListDetails || config.bDownload)
        {
            if (config.bList)
                config.bListDetails = true; // Always list details
            this->getGameList();
            if (config.bDownload)
                this->download();
            else
                this->listGames();
        }
    }
}

void Downloader::getGameList()
{
    gameNames = this->getGames();

    // Filter the game list
    if (!config.sGameRegex.empty())
    {
        // GameRegex filter aliases
        if (config.sGameRegex == "all")
        {
            config.sGameRegex = ".*";
        }

        if (config.sGameRegex == "free")
        {
            gameNames = this->getFreeGames();
        }
        else
        {
            std::vector<std::string> gameNamesFiltered;
            boost::regex expression(config.sGameRegex);
            boost::match_results<std::string::const_iterator> what;
            for (unsigned int i = 0; i < gameNames.size(); ++i)
            {
                if (boost::regex_search(gameNames[i], what, expression))
                {
                    gameNamesFiltered.push_back(gameNames[i]);
                }
            }
            gameNames = gameNamesFiltered;
        }
    }

    if (config.bListDetails || config.bDownload || config.bRepair)
    {
        this->getGameDetails();
    }
}

/* Get detailed info about the games
    returns 0 if successful
    returns 1 if fails
*/
int Downloader::getGameDetails()
{
    gameDetails game;
    int updated = 0;
    for (unsigned int i = 0; i < gameNames.size(); ++i)
    {
        std::cout << "Getting game info " << i+1 << " / " << gameNames.size() << "\r" << std::flush;
        game = gogAPI->getGameDetails(gameNames[i], config.iInstallerType, config.iInstallerLanguage);
        if (!gogAPI->getError())
        {
            if (!config.bUpdateCheck)
                games.push_back(game);
            else
            { // Update check, only add games that have updated files
                for (unsigned int j = 0; j < game.installers.size(); ++j)
                {
                    if (game.installers[j].updated)
                    {
                        games.push_back(game);
                        updated++;
                        break; // add the game only once
                    }
                }
                if (updated >= gogAPI->user.notifications_games)
                { // Gone through all updated games. No need to go through the rest.
                    std::cout << std::endl << "Got info for all updated games. Moving on..." << std::endl;
                    break;
                }
            }
        }
        else
        {
            std::cout << gogAPI->getErrorMessage() << std::endl;
            return 1;
        }
    }
    std::cout << std::endl;
    return 0;
}

void Downloader::listGames()
{
    if (config.bListDetails) // Detailed list
    {
        for (unsigned int i = 0; i < games.size(); ++i)
        {
            std::cout   << "gamename: " << games[i].gamename << std::endl
                        << "title: " << games[i].title << std::endl
                        << "icon: " << "http://static.gog.com" << games[i].icon << std::endl;
            // List installers
            if (!config.bNoInstallers)
            {
                std::cout << "installers: " << std::endl;
                for (unsigned int j = 0; j < games[i].installers.size(); ++j)
                {
                    if (!config.bUpdateCheck || games[i].installers[j].updated) // Always list updated files
                    {
                        std::cout   << "\tid: " << games[i].installers[j].id << std::endl
                                    << "\tname: " << games[i].installers[j].name << std::endl
                                    << "\tpath: " << games[i].installers[j].path << std::endl
                                    << "\tsize: " << games[i].installers[j].size << std::endl
                                    << "\tupdated: " << (games[i].installers[j].updated ? "True" : "False") << std::endl
                                    << std::endl;
                    }
                }
            }
            // List extras
            if (!config.bNoExtras && !config.bUpdateCheck && !games[i].extras.empty())
            {
                std::cout << "extras: " << std::endl;
                for (unsigned int j = 0; j < games[i].extras.size(); ++j)
                {
                    std::cout   << "\tid: " << games[i].extras[j].id << std::endl
                                << "\tname: " << games[i].extras[j].name << std::endl
                                << "\tpath: " << games[i].extras[j].path << std::endl
                                << "\tsize: " << games[i].extras[j].size << std::endl
                                << std::endl;
                }
            }
            // List patches
            if (!config.bNoPatches && !config.bUpdateCheck && !games[i].patches.empty())
            {
                std::cout << "patches: " << std::endl;
                for (unsigned int j = 0; j < games[i].patches.size(); ++j)
                {
                    std::cout   << "\tid: " << games[i].patches[j].id << std::endl
                                << "\tname: " << games[i].patches[j].name << std::endl
                                << "\tpath: " << games[i].patches[j].path << std::endl
                                << "\tsize: " << games[i].patches[j].size << std::endl
                                << std::endl;
                }
            }
        }
    }
    else
    {
        for (unsigned int i = 0; i < gameNames.size(); ++i)
            std::cout << gameNames[i] << std::endl;
    }

}

void Downloader::repair()
{
    for (unsigned int i = 0; i < games.size(); ++i)
    {
        // Installers (use remote or local file)
        if (!config.bNoInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[j].path, games[i].gamename);

                // Get XML data
                std::string XML = "";
                if (!config.bNoRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                }

                // Repair
                if (!XML.empty() || config.bNoRemoteXML)
                {
                    std::string url = gogAPI->getInstallerLink(games[i].gamename, games[i].installers[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                    std::cout << "Repairing file " << filepath << std::endl;
                    this->repairFile(url, filepath, XML);
                    std::cout << std::endl;
                }
            }
        }

        // Extras (GOG doesn't provide XML data for extras, use local file)
        if (!config.bNoExtras)
        {
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].extras[j].path, games[i].gamename);

                std::string url = gogAPI->getExtraLink(games[i].gamename, games[i].extras[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }
                std::cout << "Repairing file " << filepath << std::endl;
                this->repairFile(url, filepath);
                std::cout << std::endl;
            }
        }

        // Patches (use remote or local file)
        if (!config.bNoPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].patches[j].path, games[i].gamename);

                // Get XML data
                std::string XML = "";
                if (!config.bNoRemoteXML)
                {
                    XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                }

                // Repair
                if (!XML.empty() || config.bNoRemoteXML)
                {
                    std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                    if (gogAPI->getError())
                    {
                        std::cout << gogAPI->getErrorMessage() << std::endl;
                        gogAPI->clearError();
                        continue;
                    }
                    std::cout << "Repairing file " << filepath << std::endl;
                    this->repairFile(url, filepath, XML);
                    std::cout << std::endl;
                }
            }
        }
    }
}

void Downloader::download()
{
    for (unsigned int i = 0; i < games.size(); ++i)
    {
        // Download covers
        if (!config.bNoCover && !config.bUpdateCheck)
        {
            // Take path from installer path because for some games the base directory for installer/extra path is not "gamename"
            std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[0].path, games[i].gamename);

            // Get base directory from filepath
            boost::match_results<std::string::const_iterator> what;
            boost::regex expression("(.*)/.*");
            boost::regex_match(filepath, what, expression);
            std::string directory = what[1];

            this->downloadCovers(games[i].gamename, directory, coverXML);
        }
        // Download installers
        if (!config.bNoInstallers)
        {
            for (unsigned int j = 0; j < games[i].installers.size(); ++j)
            {
                // Not updated, skip to next installer
                if (config.bUpdateCheck && !games[i].installers[j].updated)
                    continue;

                // Get link
                std::string url = gogAPI->getInstallerLink(games[i].gamename, games[i].installers[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].installers[j].path, games[i].gamename);

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (!config.bNoRemoteXML)
                        XML = gogAPI->getXML(games[i].gamename, games[i].installers[j].id);
                    if (!games[i].installers[j].name.empty())
                        std::cout << "Dowloading: " << games[i].installers[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    this->downloadFile(url, filepath, XML);
                    std::cout << std::endl;
                }
            }
        }
        // Download extras
        if (!config.bNoExtras && !config.bUpdateCheck)
        { // Save some time and don't process extras when running update check. Extras don't have updated flag, all of them would be skipped anyway.
            for (unsigned int j = 0; j < games[i].extras.size(); ++j)
            {
                // Get link
                std::string url = gogAPI->getExtraLink(games[i].gamename, games[i].extras[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].extras[j].path, games[i].gamename);

                // Download
                if (!url.empty())
                {
                    if (!games[i].extras[j].name.empty())
                        std::cout << "Dowloading: " << games[i].extras[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    CURLcode result = downloadFile(url, filepath);
                    std::cout << std::endl;
                    if (result==CURLE_OK && config.sXMLFile == "automatic")
                    {
                        std::cout << "Starting automatic XML creation" << std::endl;
                        Util::createXML(filepath, config.iChunkSize, config.sXMLDirectory);
                    }
                }
            }
        }
        // Download patches
        if (!config.bNoPatches)
        {
            for (unsigned int j = 0; j < games[i].patches.size(); ++j)
            {
                // Get link
                std::string url = gogAPI->getPatchLink(games[i].gamename, games[i].patches[j].id);
                if (gogAPI->getError())
                {
                    std::cout << gogAPI->getErrorMessage() << std::endl;
                    gogAPI->clearError();
                    continue;
                }

                std::string filepath = Util::makeFilepath(config.sDirectory, games[i].patches[j].path, games[i].gamename);

                // Download
                if (!url.empty())
                {
                    std::string XML;
                    if (!config.bNoRemoteXML)
                        XML = gogAPI->getXML(games[i].gamename, games[i].patches[j].id);
                    if (!games[i].patches[j].name.empty())
                        std::cout << "Dowloading: " << games[i].gamename << " " << games[i].patches[j].name << std::endl;
                    std::cout << filepath << std::endl;
                    this->downloadFile(url, filepath, XML);
                    std::cout << std::endl;
                }
            }
        }
    }
}

// Download a file, resume if possible
CURLcode Downloader::downloadFile(const std::string& url, const std::string& filepath, const std::string& xml_data)
{
    CURLcode res = CURLE_RECV_ERROR; // assume network error
    bool bResume = false;
    FILE *outfile;
    size_t offset=0;

    // Get directory from filepath
    boost::filesystem::path pathname = filepath;
    std::string directory = pathname.parent_path().string();
    std::string filenameXML = pathname.filename().string() + ".xml";

    // Using local XML data for version check before resuming
    boost::filesystem::path local_xml_file;
    if (config.sXMLDirectory.empty())
        local_xml_file = config.sHome + "/.gogdownloader/xml/" + filenameXML;
    else
        local_xml_file = config.sXMLDirectory + "/" + filenameXML;

    bool bSameVersion = true; // assume same version
    bool bLocalXMLExists = boost::filesystem::exists(local_xml_file);

    if (!xml_data.empty())
    {
        // Do version check if local XML file exists
        if (bLocalXMLExists)
        {
            TiXmlDocument remote_xml, local_xml;
            remote_xml.Parse(xml_data.c_str());
            local_xml.LoadFile(local_xml_file.string());
            TiXmlNode *fileNodeRemote = remote_xml.FirstChild("file");
            TiXmlNode *fileNodeLocal = local_xml.FirstChild("file");
            if (fileNodeRemote && fileNodeLocal)
            {
                TiXmlElement *fileElemRemote = fileNodeRemote->ToElement();
                TiXmlElement *fileElemLocal = fileNodeLocal->ToElement();
                std::string remoteHash = fileElemRemote->Attribute("md5");
                std::string localHash = fileElemLocal->Attribute("md5");
                if (remoteHash != localHash)
                    bSameVersion = false;
            }
        }
    }

    // Check that directory exists and create subdirectories
    boost::filesystem::path path = directory;
    if (boost::filesystem::exists(path))
    {
        if (!boost::filesystem::is_directory(path))
        {
            std::cout << path << " is not directory" << std::endl;
            return res;
        }
    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    // Check if file exists
    if ((outfile=fopen(filepath.c_str(), "r"))!=NULL)
    {
        if (bSameVersion)
        {
            // File exists, resume
            if ((outfile = freopen(filepath.c_str(), "r+", outfile))!=NULL )
            {
                bResume = true;
                fseek(outfile, 0, SEEK_END);
                offset = ftell(outfile);
                curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, offset);
                this->resume_position = offset;
            }
            else
            {
                std::cout << "Failed to reopen " << filepath << std::endl;
                return res;
            }
        }
        else
        {
            fclose(outfile);
            std::cout << "Remote file is different, renaming local file" << std::endl;
            boost::filesystem::path new_name = filepath + ".old";
            if (boost::filesystem::exists(new_name))
            {
                std::cout << "Old renamed file found, deleting old file" << std::endl;
                if (!boost::filesystem::remove(new_name))
                {
                    std::cout << "Failed to delete " << new_name.string() << std::endl;
                    std::cout << "Skipping file" << std::endl;
                    return res;
                }
            }
            boost::system::error_code ec;
            boost::filesystem::rename(pathname, new_name, ec);
            if (ec)
            {
                std::cout << "Failed to rename " << filepath << " to " << new_name.string() << std::endl;
                std::cout << "Skipping file" << std::endl;
                return res;
            }
            else
            {
                // Create new file
                if ((outfile=fopen(filepath.c_str(), "w"))!=NULL)
                {
                    curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, 0); // start downloading from the beginning of file
                    this->resume_position = 0;
                }
                else
                {
                    std::cout << "Failed to create " << filepath << std::endl;
                    return res;
                }
            }
        }
    }
    else
    {
        // File doesn't exist, create new file
        if ((outfile=fopen(filepath.c_str(), "w"))!=NULL)
        {
            curl_easy_setopt(curlhandle, CURLOPT_RESUME_FROM, 0); // start downloading from the beginning of file
            this->resume_position = 0;
        }
        else
        {
            std::cout << "Failed to create " << filepath << std::endl;
            return res;
        }
    }

    // Save remote XML
    if (!xml_data.empty())
    {
        if ((bLocalXMLExists && (!bSameVersion || config.bRepair)) || !bLocalXMLExists)
        {
            std::ofstream ofs(local_xml_file.string().c_str());
            if (ofs)
            {
                ofs << xml_data;
                ofs.close();
            }
            else
            {
                std::cout << "Can't create " << local_xml_file.string() << std::endl;
            }
        }
    }

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
    res = this->beginDownload();

    fclose(outfile);

    // Download failed and was not a resume attempt so delete the file
    if (res != CURLE_OK && !bResume)
    {
        boost::filesystem::path path = filepath;
        if (boost::filesystem::exists(path))
            if (!boost::filesystem::remove(path))
                std::cout << "Failed to delete " << path << std::endl;
    }

    return res;
}

// Repair file
int Downloader::repairFile(const std::string& url, const std::string& filepath, const std::string& xml_data)
{
    int res = 0;
    FILE *outfile;
    size_t offset=0, from_offset, to_offset, filesize;
    std::string filehash;
    int chunks;
    std::vector<size_t> chunk_from, chunk_to;
    std::vector<std::string> chunk_hash;

    // Get filename
    boost::filesystem::path pathname = filepath;
    std::string filename = pathname.filename().string();

    TiXmlDocument xml;
    if (!xml_data.empty()) {
        std::cout << "XML: Using remote file" << std::endl;
        xml.Parse(xml_data.c_str());
    }
    else
    {
        std::string xml_file = config.sXMLDirectory + "/" + filename + ".xml";
        std::cout << "XML: Using local file" << std::endl;
        xml.LoadFile(xml_file);
    }

    TiXmlNode *fileNode = xml.FirstChild("file");
    if (!fileNode)
    {
        std::cout << "XML: Parsing failed / not valid XML" << std::endl;
        return res;
    }
    else
    {
        std::cout << "XML: Valid XML" << std::endl;
        TiXmlElement *fileElem = fileNode->ToElement();
        filename = fileElem->Attribute("name");
        filehash = fileElem->Attribute("md5");
        std::stringstream(fileElem->Attribute("chunks")) >> chunks;
        std::stringstream(fileElem->Attribute("total_size")) >> filesize;

        //Iterate through all chunk nodes
        TiXmlNode *chunkNode = fileNode->FirstChild();
        while (chunkNode)
        {
            TiXmlElement *chunkElem = chunkNode->ToElement();
            std::stringstream(chunkElem->Attribute("from")) >> from_offset;
            std::stringstream(chunkElem->Attribute("to")) >> to_offset;
            chunk_from.push_back(from_offset);
            chunk_to.push_back(to_offset);
            chunk_hash.push_back(chunkElem->GetText());
            chunkNode = fileNode->IterateChildren(chunkNode);
        }
    }

    std::cout   << "XML: Parsing finished" << std::endl << std::endl
                << filename << std::endl
                << "\tMD5:\t" << filehash << std::endl
                << "\tChunks:\t" << chunks << std::endl
                << "\tSize:\t" << filesize << " bytes" << std::endl << std::endl;

    // Check if file exists
    if ((outfile=fopen(filepath.c_str(), "r"))!=NULL)
    {
        // File exists
        if ((outfile = freopen(filepath.c_str(), "r+", outfile))!=NULL )
        {
            fseek(outfile, 0, SEEK_END);
            offset = ftell(outfile);
        }
        else
        {
            std::cout << "Failed to reopen " << filepath << std::endl;
            return res;
        }
    }
    else
    {
        std::cout << "File doesn't exist " << filepath << std::endl;
        return res;
    }

    if (offset != filesize)
    {
        std::cout   << "Filesizes don't match" << std::endl
                    << "Incomplete download or different version" << std::endl;
        fclose(outfile);
        if (this->config.bDownload)
        {
            std::cout << "Redownloading file" << std::endl;
            boost::filesystem::path path = filepath;
            if (!boost::filesystem::remove(path))
            {
                std::cout << "Failed to delete " << path << std::endl;
                res = 0;
            }
            else
            {
                CURLcode result = this->downloadFile(url, filepath, xml_data);
                std::cout << std::endl;
                if (result == CURLE_OK)
                    res = 1;
                else
                    res = 0;
            }
        }
        return res;
    }

    for (int i=0; i<chunks; i++)
    {
        size_t chunk_begin = chunk_from.at(i);
        size_t chunk_end = chunk_to.at(i);
        size_t size=0, chunk_size = chunk_end - chunk_begin + 1;
        std::stringstream ss;
        ss << chunk_begin << "-" << chunk_end;
        std::string range = ss.str();
        ss.str(std::string());

        std::cout << "\033[0K\rChunk " << i << " (" << chunk_size << " bytes): ";
        fseek(outfile, chunk_begin, SEEK_SET);
        unsigned char *chunk = (unsigned char *) malloc(chunk_size * sizeof(unsigned char *));
        if (chunk == NULL)
        {
            std::cout << "Memory error" << std::endl;
            return res;
        }
        size = fread(chunk, 1, chunk_size, outfile);
        if (size != chunk_size)
        {
            std::cout << "Read error" << std::endl;
            free(chunk);
            return res;
        }
        std::string hash = Util::getChunkHash(chunk, chunk_size, RHASH_MD5);
        if (hash != chunk_hash.at(i))
        {
            std::cout << "Failed - downloading chunk" << std::endl;
            fseek(outfile, chunk_begin, SEEK_SET);
            curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, outfile);
            curl_easy_setopt(curlhandle, CURLOPT_RANGE, range.c_str()); //download range
            this->beginDownload(); //begin chunk download
            std::cout << std::endl;
            i--; //verify downloaded chunk
        }
        else
        {
            std::cout << "OK\r" << std::flush;
        }
        free(chunk);
        res = 1;
    }
    std::cout << std::endl;
    fclose(outfile);

    return res;
}

// Download cover images
int Downloader::downloadCovers(const std::string& gamename, const std::string& directory, const std::string& cover_xml_data)
{
    int res = 0;
    TiXmlDocument xml;

    // Check that directory exists and create subdirectories
    boost::filesystem::path path = directory;
    if (boost::filesystem::exists(path))
    {
        if (!boost::filesystem::is_directory(path))
        {
            std::cout << path << " is not directory" << std::endl;
            return res;
        }

    }
    else
    {
        if (!boost::filesystem::create_directories(path))
        {
            std::cout << "Failed to create directory: " << path << std::endl;
            return res;
        }
    }

    xml.Parse(cover_xml_data.c_str());
    TiXmlElement *rootNode = xml.RootElement();
    if (!rootNode)
    {
        std::cout << "Not valid XML" << std::endl;
        return res;
    }
    else
    {
        TiXmlNode *gameNode = rootNode->FirstChild();
        while (gameNode)
        {
            TiXmlElement *gameElem = gameNode->ToElement();
            std::string game_name = gameElem->Attribute("name");

            if (game_name == gamename)
            {
                boost::match_results<std::string::const_iterator> what;
                TiXmlNode *coverNode = gameNode->FirstChild();
                while (coverNode)
                {
                    TiXmlElement *coverElem = coverNode->ToElement();
                    std::string cover_url = coverElem->GetText();
                    // Get file extension for the image
                    boost::regex e1(".*(\\.\\w+)$", boost::regex::perl | boost::regex::icase);
                    boost::regex_search(cover_url, what, e1);
                    std::string file_extension = what[1];
                    std::string cover_name = std::string("cover_") + coverElem->Attribute("id") + file_extension;
                    std::string filepath = directory + "/" + cover_name;

                    std::cout << "Downloading cover " << filepath << std::endl;
                    CURLcode result = this->downloadFile(cover_url, filepath);
                    std::cout << std::endl;
                    if (result == CURLE_OK)
                        res = 1;
                    else
                        res = 0;

                    if (result == CURLE_HTTP_RETURNED_ERROR)
                    {
                        long int response_code = 0;
                        result = curl_easy_getinfo(curlhandle, CURLINFO_RESPONSE_CODE, &response_code);
                        std::cout << "HTTP ERROR: ";
                        if (result == CURLE_OK)
                            std::cout << response_code << std::endl;
                        else
                            std::cout << "failed to get error code: " << curl_easy_strerror(result) << std::endl;
                    }

                    coverNode = gameNode->IterateChildren(coverNode);
                }
                break; // Found cover for game, no need to go through rest of the game nodes
            }
            gameNode = rootNode->IterateChildren(gameNode);
        }
    }

    return res;
}

CURLcode Downloader::beginDownload()
{
    this->timer.reset();
    CURLcode result = curl_easy_perform(curlhandle);
    this->resume_position = 0;
    return result;
}

std::string Downloader::getResponse(const std::string& url)
{
    std::ostringstream memory;

    curl_easy_setopt(curlhandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeMemoryCallback);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    CURLcode result = curl_easy_perform(curlhandle);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, Downloader::writeData);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 0);
    std::string response = memory.str();
    memory.str(std::string());

    if (result != CURLE_OK)
        std::cout << curl_easy_strerror(result) << std::endl;

    return response;
}

int Downloader::progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    // on entry: dltotal - how much remains to download till the end of the file (bytes)
    //           dlnow   - how much was downloaded from the start of the program (bytes)
    unsigned int bar_length  = 26;
    Downloader* downloader = static_cast<Downloader*>(clientp);

    double rate; //  average download speed in B/s
    // trying to get rate and setting to NaN if it fails
    if (CURLE_OK != curl_easy_getinfo(downloader->curlhandle, CURLINFO_SPEED_DOWNLOAD, &rate))
       rate = std::numeric_limits<double>::quiet_NaN();

    // (Shmerl): this flag is needed to catch the case before anything was downloaded on resume,
    // and there is no way to calculate the fraction, so we set to 0 (otherwise it'd be 1).
    // This is to prevent the progress bar from jumping to 100% and then to lower value.
    // It's visually better to jump from 0% to higher one.
    bool starting = ((0.0 == dlnow) && (0.0 == dltotal));

    // (Shmerl): DEBUG: strange thing - when resuming a file which is already downloaded, dlnow is correctly 0.0
    // but dltotal is 389.0! This messes things up in the progress bar not showing the very last bar as full.
    // enable this debug line to test the problem:
    //
    //   printf("\r\033[0K dlnow: %0.2f, dltotal: %0.2f\r", dlnow, dltotal); fflush(stdout); return 0;
    //
    // For now making a quirky workaround and setting dltotal to 0.0 in that case.
    // It's probably better to find a real fix.
    if ((0.0 == dlnow) && (389.0 == dltotal)) dltotal = 0.0;

    // setting full dlwnow and dltotal
    double offset = static_cast<double>(downloader->getResumePosition());
    if (offset>0)
    {
        dlnow   += offset;
        dltotal += offset;
    }

    // Update progress bar every 100ms
    if (downloader->timer.getTimeBetweenUpdates()>=100 || dlnow == dltotal)
    {
        downloader->timer.reset();
        bptime::time_duration eta(bptime::seconds((long)((dltotal - dlnow) / rate)));
        std::stringstream eta_ss;
        if (eta.hours() > 23)
        {
           eta_ss << eta.hours() / 24 << "d " <<
                     std::setfill('0') << std::setw(2) << eta.hours() % 24 << "h " <<
                     std::setfill('0') << std::setw(2) << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else if (eta.hours() > 0)
        {
           eta_ss << eta.hours() << "h " <<
                     std::setfill('0') << std::setw(2) << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else if (eta.minutes() > 0)
        {
           eta_ss << eta.minutes() << "m " <<
                     std::setfill('0') << std::setw(2) << eta.seconds() << "s";
        }
        else
        {
           eta_ss << eta.seconds() << "s";
        }

        // Create progressbar
        double fraction = starting ? 0.0 : dlnow / dltotal;

        // assuming that config is provided.
        printf("\033[0K\r%3.0f%% ", fraction * 100);
        downloader->progressbar->draw(bar_length, fraction);
        printf(" %0.2f/%0.2fMB @ %0.2fkB/s ETA: %s\r", dlnow/1024/1024, dltotal/1024/1024, rate/1024, eta_ss.str().c_str());
        fflush(stdout);
    }

    return 0;
}

size_t Downloader::writeMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) {
    std::ostringstream *stream = (std::ostringstream*)userp;
    size_t count = size * nmemb;
    stream->write(ptr, count);
    return count;
}

size_t Downloader::writeData(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

size_t Downloader::readData(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return fread(ptr, size, nmemb, stream);
}

size_t Downloader::getResumePosition()
{
    return this->resume_position;
}

// Login to GOG website
int Downloader::HTTP_Login(const std::string& email, const std::string& password)
{
    int res = 0;
    std::string postdata;
    std::ostringstream memory;
    std::string buk;

    // Get "buk" for login form
    std::string json = this->getResponse("http://www.gog.com/user/ajax/?a=get");

    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    bool parsingSuccessful = jsonparser->parse(json, root);
    if (!parsingSuccessful)
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        return res = 0;
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << root << std::endl;
    #endif
    buk = root["buk"].asString();

    //Create postdata - escape characters in email/password to support special characters
    postdata = "log_email=" + (std::string)curl_easy_escape(curlhandle, email.c_str(),email.size())
            + "&log_password=" + (std::string)curl_easy_escape(curlhandle, password.c_str(), password.size())
            + "&buk=" + (std::string)curl_easy_escape(curlhandle, buk.c_str(), buk.size());
    curl_easy_setopt(curlhandle, CURLOPT_URL, "https://secure.gog.com/login");
    curl_easy_setopt(curlhandle, CURLOPT_POST, 1);
    curl_easy_setopt(curlhandle, CURLOPT_POSTFIELDS, postdata.c_str());
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &memory);
    curl_easy_setopt(curlhandle, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, 1);
    curl_easy_setopt(curlhandle, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
    CURLcode result = curl_easy_perform(curlhandle);
    memory.str(std::string());

    if (result != CURLE_OK)
    {
        // Expected to hit maximum amount of redirects so don't print error on it
        if (result != CURLE_TOO_MANY_REDIRECTS)
            std::cout << curl_easy_strerror(result) << std::endl;
    }

    curl_easy_setopt(curlhandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curlhandle, CURLOPT_MAXREDIRS, -1);
    json = this->getResponse("http://www.gog.com/user/ajax/?a=get");

    parsingSuccessful = jsonparser->parse(json, root);
    if (!parsingSuccessful)
    {
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << json << std::endl;
        #endif
        std::cout << jsonparser->getFormatedErrorMessages();
        return res = 0;
    }
    #ifdef DEBUG
        std::cerr << "DEBUG INFO (Downloader::HTTP_Login)" << std::endl << root << std::endl;
    #endif

    if (root["user"]["email"].asString() == email && (result == CURLE_OK || result == CURLE_TOO_MANY_REDIRECTS))
        res = 1; // Login successful
    else
        res = 0; // Login failed

    delete jsonparser;

    return res;
}

// Get list of games from account page
std::vector<std::string> Downloader::getGames()
{
    std::vector<std::string> games;
    Json::Value root;
    Json::Reader *jsonparser = new Json::Reader;
    int i = 1;
    std::string html = "";
    std::string page_html = "";

    do
    {
        std::string response = this->getResponse("https://secure.gog.com/en/account/ajax?a=gamesShelfMore&s=title&q=&t=0&p=" + std::to_string(i));

        // Parse JSON
        if (!jsonparser->parse(response, root))
        {
            #ifdef DEBUG
                std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << response << std::endl;
            #endif
            std::cout << jsonparser->getFormatedErrorMessages();
            delete jsonparser;
            exit(1);
        }
        #ifdef DEBUG
            std::cerr << "DEBUG INFO (Downloader::getGames)" << std::endl << root << std::endl;
        #endif
        page_html = root["html"].asString();
        html += page_html;
        i++;
    } while (!page_html.empty());

    delete jsonparser;

    // Parse HTML to get game names
    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="div")
        {
            it->parseAttributes();
            std::string classname = it->attribute("class").second;
            if (classname=="shelf_game")
            {
                // Game name is contained in data-gameindex attribute
                std::string game = it->attribute("data-gameindex").second;
                if (!game.empty())
                    games.push_back(game);
            }
        }
    }

    return games;
}

// Get list of free games
std::vector<std::string> Downloader::getFreeGames()
{
    std::vector<std::string> games;
    std::string html = this->getResponse("https://secure.gog.com/catalogue/ajax?a=getGames&tab=all_genres&genre=all_genres&price=0&order=alph&publisher=&releaseDate=&availability=&gameMode=&rating=&search=&sort=vote&system=&language=&mixPage=1");

    // Parse HTML to get game names
    htmlcxx::HTML::ParserDom parser;
    tree<htmlcxx::HTML::Node> dom = parser.parseTree(html);
    tree<htmlcxx::HTML::Node>::iterator it = dom.begin();
    tree<htmlcxx::HTML::Node>::iterator end = dom.end();
    for (; it != end; ++it)
    {
        if (it->tagName()=="a")
        {
            it->parseAttributes();
            std::string classname = it->attribute("class").second;
            if (classname=="game-title-link")
            {
                std::string game = it->attribute("href").second;
                game.assign(game.begin()+game.find_last_of("/")+1,game.end());
                if (!game.empty())
                    games.push_back(game);
            }
        }
    }

    return games;
}
