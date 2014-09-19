#ifndef GAMEDETAILS_H
#define GAMEDETAILS_H

#include "globalconstants.h"
#include "gamefile.h"

#include <iostream>
#include <vector>

class gameDetails
{
    public:
        std::vector<gameFile> extras;
        std::vector<gameFile> installers;
        std::vector<gameFile> patches;
        std::vector<gameFile> languagepacks;
        std::vector<gameDetails> dlcs;
        std::string gamename;
        std::string title;
        std::string icon;;
    protected:
    private:
};

#endif // GAMEDETAILS_H
