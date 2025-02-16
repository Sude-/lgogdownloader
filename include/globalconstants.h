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
    const int GAMEDETAILS_CACHE_VERSION = 6;
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
    const unsigned int LANGUAGE_AR = 1 << 24;
    const unsigned int LANGUAGE_RO = 1 << 25;
    const unsigned int LANGUAGE_HE = 1 << 26;
    const unsigned int LANGUAGE_TH = 1 << 27;

    const std::vector<optionsStruct> LANGUAGES =
    {
        { LANGUAGE_EN, "en", "English",    "en|eng|english|en[_-]US"        },
        { LANGUAGE_DE, "de", "German",     "de|deu|ger|german|de[_-]DE"     },
        { LANGUAGE_FR, "fr", "French",     "fr|fra|fre|french|fr[_-]FR"     },
        { LANGUAGE_PL, "pl", "Polish",     "pl|pol|polish|pl[_-]PL"         },
        { LANGUAGE_RU, "ru", "Russian",    "ru|rus|russian|ru[_-]RU"        },
        { LANGUAGE_CN, "cn", "Chinese",    "cn|zh|zho|chi|chinese|zh[_-](CN|Hans)" },
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
        { LANGUAGE_ES_419, "es_mx", "Spanish (Latin American)", "es_mx|es-mx|esmx|es-419|spanish_latin_american" },
        { LANGUAGE_AR, "ar", "Arabic",  "ar|ara|arabic|ar[_-][A-Z]{2}"    },
        { LANGUAGE_RO, "ro", "Romanian",  "ro|ron|rum|romanian|ro[_-][RM]O" },
        { LANGUAGE_HE, "he", "Hebrew",  "he|heb|hebrew|he[_-]IL" },
        { LANGUAGE_TH, "th", "Thai",  "th|tha|thai|th[_-]TH" }
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

    const unsigned int LIST_FORMAT_GAMES           = 1 << 0;
    const unsigned int LIST_FORMAT_DETAILS_TEXT    = 1 << 1;
    const unsigned int LIST_FORMAT_DETAILS_JSON    = 1 << 2;
    const unsigned int LIST_FORMAT_TAGS            = 1 << 3;
    const unsigned int LIST_FORMAT_TRANSFORMATIONS = 1 << 4;
    const unsigned int LIST_FORMAT_USERDATA        = 1 << 5;
    const unsigned int LIST_FORMAT_WISHLIST        = 1 << 6;

    const std::vector<optionsStruct> LIST_FORMAT =
    {
        { LIST_FORMAT_GAMES,        "games",   "Games",   "g|games"   },
        { LIST_FORMAT_DETAILS_TEXT, "details", "Details", "d|details" },
        { LIST_FORMAT_DETAILS_JSON, "json",    "JSON",    "j|json"    },
        { LIST_FORMAT_TAGS,         "tags",    "Tags",    "t|tags"    },
        { LIST_FORMAT_TRANSFORMATIONS, "transform", "Transformations", "tr|transform|transformations" },
        { LIST_FORMAT_USERDATA,     "userdata", "User data", "ud|userdata" },
        { LIST_FORMAT_WISHLIST,     "wishlist", "Wishlist", "w|wishlist" }
    };

    const unsigned int GFTYPE_BASE_INSTALLER = 1 << 0;
    const unsigned int GFTYPE_BASE_EXTRA     = 1 << 1;
    const unsigned int GFTYPE_BASE_PATCH     = 1 << 2;
    const unsigned int GFTYPE_BASE_LANGPACK  = 1 << 3;
    const unsigned int GFTYPE_DLC_INSTALLER  = 1 << 4;
    const unsigned int GFTYPE_DLC_EXTRA      = 1 << 5;
    const unsigned int GFTYPE_DLC_PATCH      = 1 << 6;
    const unsigned int GFTYPE_DLC_LANGPACK   = 1 << 7;
    const unsigned int GFTYPE_CUSTOM_BASE    = 1 << 8;
    const unsigned int GFTYPE_CUSTOM_DLC     = 1 << 9;
    const unsigned int GFTYPE_DLC = GFTYPE_DLC_INSTALLER | GFTYPE_DLC_EXTRA |
                                    GFTYPE_DLC_PATCH | GFTYPE_DLC_LANGPACK |
                                    GFTYPE_CUSTOM_DLC;
    const unsigned int GFTYPE_BASE = GFTYPE_BASE_INSTALLER | GFTYPE_BASE_EXTRA |
                                     GFTYPE_BASE_PATCH | GFTYPE_BASE_LANGPACK |
                                     GFTYPE_CUSTOM_BASE;
    const unsigned int GFTYPE_INSTALLER = GFTYPE_BASE_INSTALLER | GFTYPE_DLC_INSTALLER;
    const unsigned int GFTYPE_EXTRA     = GFTYPE_BASE_EXTRA | GFTYPE_DLC_EXTRA;
    const unsigned int GFTYPE_PATCH     = GFTYPE_BASE_PATCH | GFTYPE_DLC_PATCH;
    const unsigned int GFTYPE_LANGPACK  = GFTYPE_BASE_LANGPACK | GFTYPE_DLC_LANGPACK;
    const unsigned int GFTYPE_CUSTOM    = GFTYPE_CUSTOM_BASE | GFTYPE_CUSTOM_DLC;

    const std::vector<GlobalConstants::optionsStruct> INCLUDE_OPTIONS =
    {
        { GFTYPE_BASE_INSTALLER, "bi", "Base game installers",     "bi|basegame_installers" },
        { GFTYPE_BASE_EXTRA,     "be", "Base game extras",         "be|basegame_extras"     },
        { GFTYPE_BASE_PATCH,     "bp", "Base game patches",        "bp|basegame_patches"    },
        { GFTYPE_BASE_LANGPACK,  "bl", "Base game language packs", "bl|basegame_languagepacks|basegame_langpacks" },
        { GFTYPE_DLC_INSTALLER,  "di", "DLC installers",           "di|dlc_installers"      },
        { GFTYPE_DLC_EXTRA,      "de", "DLC extras",               "de|dlc_extras"          },
        { GFTYPE_DLC_PATCH,      "dp", "DLC patches",              "dp|dlc_patches"         },
        { GFTYPE_DLC_LANGPACK,   "dl", "DLC language packs",       "dl|dlc_languagepacks|dlc_langpacks" },
        { GFTYPE_DLC,            "d",  "DLCs",                     "d|dlc|dlcs"             },
        { GFTYPE_BASE,           "b",  "Basegame",                 "b|bg|basegame"          },
        { GFTYPE_INSTALLER,      "i",  "All installers",           "i|installers"           },
        { GFTYPE_EXTRA,          "e",  "All extras",               "e|extras"               },
        { GFTYPE_PATCH,          "p",  "All patches",              "p|patches"              },
        { GFTYPE_LANGPACK,       "l",  "All language packs",       "l|languagepacks|langpacks" }
    };
}

#endif // GLOBALCONSTANTS_H_INCLUDED
