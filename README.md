# LGOGDownloader

This repository contains the code of LGOGDownloader which is unoffcial open source downloader for [GOG.com](https://www.gog.com/).
It uses the same API as GOG Galaxy which doesn't have Linux support at the moment.

## Dependencies

* [libcurl](https://curl.haxx.se/libcurl/) >= 7.55.0
* [librhash](https://github.com/rhash/RHash)
* [jsoncpp](https://github.com/open-source-parsers/jsoncpp)
* [htmlcxx](http://htmlcxx.sourceforge.net/)
* [tinyxml2](https://github.com/leethomason/tinyxml2)
* [boost](http://www.boost.org/) (regex, date-time, system, filesystem, program-options, iostreams)
* [zlib](https://www.zlib.net/)
* [qtwebengine](https://www.qt.io/) if built with -DUSE_QT_GUI=ON

## Make dependencies
* [cmake](https://cmake.org/) >= 3.0.0
* [ninja](https://github.com/ninja-build/ninja)
* [help2man](https://www.gnu.org/software/help2man/help2man.html) (optional, man page generation)

### Debian/Ubuntu

    # apt install build-essential libcurl4-openssl-dev libboost-regex-dev \
    libjsoncpp-dev librhash-dev libtinyxml2-dev libhtmlcxx-dev \
    libboost-system-dev libboost-filesystem-dev libboost-program-options-dev \
    libboost-date-time-dev libboost-iostreams-dev help2man cmake \
    pkg-config zlib1g-dev qtwebengine5-dev ninja-build

## Build and install

    $ cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DUSE_QT_GUI=ON -GNinja
    $ ninja -Cbuild install

## Usage examples

- **Login**

        lgogdownloader --login

- **Listing games and details for specific games**

        lgogdownloader --list
        lgogdownloader --list details --game witcher


- **Downloading files**

        lgogdownloader --download
        lgogdownloader --download --game stardew_valley --exclude extras
        lgogdownloader --download --threads 6 --platform linux --language en+de,fr
        lgogdownloader --download-file tyrian_2000/9543

- **Repairing files**

        lgogdownloader --repair --game beneath_a_steel_sky
        lgogdownloader --repair --download --game "^a"

- **Using Galaxy API for listing and installing game builds**

        lgogdownloader --galaxy-platform windows --galaxy-show-builds stardew_valley
        lgogdownloader --galaxy-platform windows --galaxy-install stardew_valley/0
        lgogdownloader --galaxy-platform windows --galaxy-install beneath_a_steel_sky/0 --galaxy-no-dependencies

- **See man page or help text for more**

        lgogdownloader --help
        man lgogdownloader

## Links
- [LGOGDownloader website](https://sites.google.com/site/gogdownloader/)
- [GOG forum thread](https://www.gog.com/forum/general/lgogdownloader_gogdownloader_for_linux)
- [LGOGDownloader @ AUR](https://aur.archlinux.org/packages/lgogdownloader/)
- [LGOGDownloader @ AUR (git version)](https://aur.archlinux.org/packages/lgogdownloader-git/)
- [LGOGDownloader @ Debian](https://tracker.debian.org/lgogdownloader)
- [LGOGDownloader @ Ubuntu](https://launchpad.net/ubuntu/+source/lgogdownloader)

[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=PT95NXVLQU6WG&source=url)
