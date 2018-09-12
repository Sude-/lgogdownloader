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
    const int GAMEDETAILS_CACHE_VERSION = 3;
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
    const unsigned int LANGUAGE_ES_419 = 1 << 23;

    const std::vector<optionsStruct> LANGUAGES =
    {
        { LANGUAGE_EN, "en", "English",    "en|eng|english|en[_-]US"        },
        { LANGUAGE_DE, "de", "German",     "de|deu|ger|german|de[_-]DE"     },
        { LANGUAGE_FR, "fr", "French",     "fr|fra|fre|french|fr[_-]FR"     },
        { LANGUAGE_PL, "pl", "Polish",     "pl|pol|polish|pl[_-]PL"         },
        { LANGUAGE_RU, "ru", "Russian",    "ru|rus|russian|ru[_-]RU"        },
        { LANGUAGE_CN, "cn", "Chinese",    "cn|zh|zho|chi|chinese|zh[_-]CN" },
        { LANGUAGE_CZ, "cz", "Czech",      "cz|cs|ces|cze|czech|cs[_-]CZ"   },
        { LANGUAGE_ES, "es", "Spanish",    "es|spa|spanish|es[_-]ES"        },
        { LANGUAGE_HU, "hu", "Hungarian",  "hu|hun|hungarian|hu[_-]HU"      },
        { LANGUAGE_IT, "it", "Italian",    "it|ita|italian|it[_-]IT"        },
        { LANGUAGE_JP, "jp", "Japanese",   "jp|ja|jpn|japanese|ja[_-]JP"    },
        { LANGUAGE_TR, "tr", "Turkish",    "tr|tur|turkish|tr[_-]TR"        },
        { LANGUAGE_PT, "pt", "Portuguese", "pt|por|portuguese|pt[_-]PT"     },
        { LANGUAGE_KO, "ko", "Korean",     "ko|kor|korean|ko[_-]KR"         },
        { LANGUAGE_NL, "nl", "Dutch",      "nl|nld|dut|dutch|nl[_-]NL"      },
        { LANGUAGE_SV, "sv", "Swedish",    "sv|swe|swedish|sv[_-]SE"        },
        { LANGUAGE_NO, "no", "Norwegian",  "no|nor|norwegian|nb[_-]no|nn[_-]NO" },
        { LANGUAGE_DA, "da", "Danish",     "da|dan|danish|da[_-]DK"         },
        { LANGUAGE_FI, "fi", "Finnish",    "fi|fin|finnish|fi[_-]FI"        },
        { LANGUAGE_PT_BR, "br", "Brazilian Portuguese", "br|pt_br|pt-br|ptbr|brazilian_portuguese" },
        { LANGUAGE_SK, "sk", "Slovak",     "sk|slk|slo|slovak|sk[_-]SK"     },
        { LANGUAGE_BL, "bl", "Bulgarian",  "bl|bg|bul|bulgarian|bg[_-]BG"   },
        { LANGUAGE_UK, "uk", "Ukrainian",  "uk|ukr|ukrainian|uk[_-]UA"      },
        { LANGUAGE_ES_419, "es_mx", "Spanish (Latin American)", "es_mx|es-mx|esmx|es-419|spanish_latin_american" }
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

    // Galaxy platform arch
    const unsigned int ARCH_X86 = 1 << 0;
    const unsigned int ARCH_X64 = 1 << 1;

    const std::vector<optionsStruct> GALAXY_ARCHS =
    {
        { ARCH_X86, "32", "32-bit", "32|x86|32bit|32-bit" },
        { ARCH_X64, "64", "64-bit", "64|x64|64bit|64-bit" }
    };

    // Galaxy CDNs
    const unsigned int CDN_EDGECAST  = 1 << 0;
    const unsigned int CDN_HIGHWINDS = 1 << 1;
    const unsigned int CDN_GOG       = 1 << 2;

    const std::vector<optionsStruct> GALAXY_CDNS =
    {
        { CDN_EDGECAST,  "edgecast",   "Edgecast",  "ec|edgecast"             },
        { CDN_HIGHWINDS, "high_winds", "Highwinds", "hw|highwinds|high_winds" },
        { CDN_GOG,       "gog_cdn",    "GOG",       "gog|gog_cdn"             }
    };
}

#endif // GLOBALCONSTANTS_H_INCLUDED
