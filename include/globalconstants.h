/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef GLOBALCONSTANTS_H_INCLUDED
#define GLOBALCONSTANTS_H_INCLUDED

#include <iostream>
#include <vector>

namespace GlobalConstants
{
    // API constants
    const std::string CONSUMER_KEY = "1f444d14ea8ec776585524a33f6ecc1c413ed4a5";
    const std::string CONSUMER_SECRET = "20d175147f9db9a10fc0584aa128090217b9cf88";
    const int OAUTH_VERIFIER_LENGTH = 14;
    const int OAUTH_TOKEN_LENGTH = 11;
    const int OAUTH_SECRET_LENGTH = 18;

    // Language constants
    const unsigned int LANGUAGE_EN = 1;
    const unsigned int LANGUAGE_DE = 2;
    const unsigned int LANGUAGE_FR = 4;
    const unsigned int LANGUAGE_PL = 8;
    const unsigned int LANGUAGE_RU = 16;
    const unsigned int LANGUAGE_CN = 32;
    const unsigned int LANGUAGE_CZ = 64;
    const unsigned int LANGUAGE_ES = 128;
    const unsigned int LANGUAGE_HU = 256;
    const unsigned int LANGUAGE_IT = 512;
    const unsigned int LANGUAGE_JP = 1024;
    const unsigned int LANGUAGE_TR = 2048;

    struct languageStruct {const unsigned int languageId; const std::string languageCode; const std::string languageString;};
    const std::vector<languageStruct> LANGUAGES =
    {
        { LANGUAGE_EN, "en", "English"   },
        { LANGUAGE_DE, "de", "German"    },
        { LANGUAGE_FR, "fr", "French"    },
        { LANGUAGE_PL, "pl", "Polish"    },
        { LANGUAGE_RU, "ru", "Russian"   },
        { LANGUAGE_CN, "cn", "Chinese"   },
        { LANGUAGE_CZ, "cz", "Czech"     },
        { LANGUAGE_ES, "es", "Spanish"   },
        { LANGUAGE_HU, "hu", "Hungarian" },
        { LANGUAGE_IT, "it", "Italian"   },
        { LANGUAGE_JP, "jp", "Japanese"  },
        { LANGUAGE_TR, "tr", "Turkish"   }
    };

    // Installer constants
    const unsigned int PLATFORM_WINDOWS = 1;
    const unsigned int PLATFORM_MAC     = 2;

    struct platformStruct {const unsigned int platformId; const std::string platformCode; const std::string platformString;};
    const std::vector<platformStruct> PLATFORMS =
    {
        { PLATFORM_WINDOWS, "win", "Windows" },
        { PLATFORM_MAC,     "mac", "Mac"     }
    };
};

#endif // GLOBALCONSTANTS_H_INCLUDED
