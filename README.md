# GOG Downloader

This repository contains the code of unofficial [GOG](http://www.gog.com/) downloader.

## Dependencies

* [libcurl](https://curl.haxx.se/libcurl/)
* [liboauth](https://sourceforge.net/projects/liboauth/)
* [librhash](https://github.com/rhash/RHash)
* [jsoncpp](https://github.com/open-source-parsers/jsoncpp)
* [htmlcxx](http://htmlcxx.sourceforge.net/)
* [tinyxml2](https://github.com/leethomason/tinyxml2)
* [boost](http://www.boost.org/) (regex, date-time, system, filesystem, program-options)
* [help2man](https://www.gnu.org/software/help2man/help2man.html)

### Debian/Ubuntu

    # apt install build-essential libcurl4-openssl-dev libboost-regex-dev \
    libjsoncpp-dev liboauth-dev librhash-dev libtinyxml2-dev libhtmlcxx-dev\
    libboost-system-dev libboost-filesystem-dev libboost-program-options-dev\
    libboost-date-time-dev help2man cmake

## Build and install

    $ mkdir build
    $ cd build
    $ cmake ..
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
