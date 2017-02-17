# GOG Downloader

This repository contains the code of unofficial [GOG](http://www.gog.com/) downloader.

## Dependencies

* [libcurl](https://curl.haxx.se/libcurl/) >= 7.32.0
* [liboauth](https://sourceforge.net/projects/liboauth/)
* [librhash](https://github.com/rhash/RHash)
* [jsoncpp](https://github.com/open-source-parsers/jsoncpp)
* [htmlcxx](http://htmlcxx.sourceforge.net/)
* [tinyxml2](https://github.com/leethomason/tinyxml2)
* [boost](http://www.boost.org/) (regex, date-time, system, filesystem, program-options, iostreams)
* [libcrypto](https://www.openssl.org/) if libcurl is built with OpenSSL

## Make dependencies
* [cmake](https://cmake.org/) >= 3.0.0
* [help2man](https://www.gnu.org/software/help2man/help2man.html) (optional, man page generation)
* [grep](https://www.gnu.org/software/grep/)
* [binutils](https://www.gnu.org/software/binutils/) (readelf)

### Debian/Ubuntu

    # apt install build-essential libcurl4-openssl-dev libboost-regex-dev \
    libjsoncpp-dev liboauth-dev librhash-dev libtinyxml2-dev libhtmlcxx-dev \
    libboost-system-dev libboost-filesystem-dev libboost-program-options-dev \
    libboost-date-time-dev libboost-iostreams-dev help2man cmake libssl-dev \
    pkg-config

## Build and install

    $ mkdir build
    $ cd build
    $ cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
    $ make
    # sudo make install

## Use

    man lgogdownloader

## Links
- [LGOGDownloader website](https://sites.google.com/site/gogdownloader/)
- [GOG forum thread](https://www.gog.com/forum/general/lgogdownloader_gogdownloader_for_linux)
- [LGOGDownloader @ AUR](https://aur.archlinux.org/packages/lgogdownloader/)
- [LGOGDownloader @ AUR (git version)](https://aur.archlinux.org/packages/lgogdownloader-git/)
- [LGOGDownloader @ Debian](https://tracker.debian.org/lgogdownloader)
- [LGOGDownloader @ Ubuntu](https://launchpad.net/ubuntu/+source/lgogdownloader)
