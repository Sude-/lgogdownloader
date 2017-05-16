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
    const int GAMEDETAILS_CACHE_VERSION = 2;
    const int ZLIB_WINDOW_SIZE = 15;

    struct optionsStruct {const unsigned int id; const std::string code; const std::string str; const std::string regexp;};
    const std::string PROTOCOL_PREFIX = "gogdownloader://";

    // Language constants
    const unsigned int LANGUAGE_EN = 1 << 0;
    const unsigned int LANGUAGE_DE = 1 << 1;
    const unsigned int LANGUAGE_FR = 1 << 2;
    const unsigned int LANGUAGE_PL = 1 << 3;
    const unsigned int LANGUAGE_RU = 1 << 4;
    const unsigned int LANGUAGE_CN = 1 << 5;
    const unsigned int LANGUAGE_CZ = 1 << 6;
    const unsigned int LANGUAGE_ES = 1 << 7;
    const unsigned int LANGUAGE_HU = 1 << 8;
    const unsigned int LANGUAGE_IT = 1 << 9;
    const unsigned int LANGUAGE_JP = 1 << 10;
    const unsigned int LANGUAGE_TR = 1 << 11;
    const unsigned int LANGUAGE_PT = 1 << 12;
    const unsigned int LANGUAGE_KO = 1 << 13;
    const unsigned int LANGUAGE_NL = 1 << 14;
    const unsigned int LANGUAGE_SV = 1 << 15;
    const unsigned int LANGUAGE_NO = 1 << 16;
    const unsigned int LANGUAGE_DA = 1 << 17;
    const unsigned int LANGUAGE_FI = 1 << 18;
    const unsigned int LANGUAGE_PT_BR = 1 << 19;
    const unsigned int LANGUAGE_SK = 1 << 20;
    const unsigned int LANGUAGE_BL = 1 << 21;
    const unsigned int LANGUAGE_UK = 1 << 22;

    const std::vector<optionsStruct> LANGUAGES =
    {
        { LANGUAGE_EN, "en", "English"   , "en|eng|english"        },
        { LANGUAGE_DE, "de", "German"    , "de|deu|ger|german"     },
        { LANGUAGE_FR, "fr", "French"    , "fr|fra|fre|french"     },
        { LANGUAGE_PL, "pl", "Polish"    , "pl|pol|polish"         },
        { LANGUAGE_RU, "ru", "Russian"   , "ru|rus|russian"        },
        { LANGUAGE_CN, "cn", "Chinese"   , "cn|zh|zho|chi|chinese" },
        { LANGUAGE_CZ, "cz", "Czech"     , "cz|cs|ces|cze|czech"   },
        { LANGUAGE_ES, "es", "Spanish"   , "es|spa|spanish"        },
        { LANGUAGE_HU, "hu", "Hungarian" , "hu|hun|hungarian"      },
        { LANGUAGE_IT, "it", "Italian"   , "it|ita|italian"        },
        { LANGUAGE_JP, "jp", "Japanese"  , "jp|ja|jpn|japanese"    },
        { LANGUAGE_TR, "tr", "Turkish"   , "tr|tur|turkish"        },
        { LANGUAGE_PT, "pt", "Portuguese", "pt|por|portuguese"     },
        { LANGUAGE_KO, "ko", "Korean"    , "ko|kor|korean"         },
        { LANGUAGE_NL, "nl", "Dutch"     , "nl|nld|dut|dutch"      },
        { LANGUAGE_SV, "sv", "Swedish"   , "sv|swe|swedish"        },
        { LANGUAGE_NO, "no", "Norwegian" , "no|nor|norwegian"      },
        { LANGUAGE_DA, "da", "Danish"    , "da|dan|danish"         },
        { LANGUAGE_FI, "fi", "Finnish"   , "fi|fin|finnish"        },
        { LANGUAGE_PT_BR, "br", "Brazilian Portuguese", "br|pt_br|pt-br|ptbr|brazilian_portuguese" },
        { LANGUAGE_SK, "sk", "Slovak"    , "sk|slk|slo|slovak"     },
        { LANGUAGE_BL, "bl", "Bulgarian" , "bl|bg|bul|bulgarian"   },
        { LANGUAGE_UK, "uk", "Ukrainian" , "uk|ukr|ukrainian"      }
    };

    // Platform constants
    const unsigned int PLATFORM_WINDOWS = 1 << 0;
    const unsigned int PLATFORM_MAC     = 1 << 1;
    const unsigned int PLATFORM_LINUX   = 1 << 2;

    const std::vector<optionsStruct> PLATFORMS =
    {
        { PLATFORM_WINDOWS, "win",   "Windows" , "w|win|windows" },
        { PLATFORM_MAC,     "mac",   "Mac"     , "m|mac|osx"     },
        { PLATFORM_LINUX,   "linux", "Linux"   , "l|lin|linux"   }
    };
}

#endif // GLOBALCONSTANTS_H_INCLUDED
