# GOG Downloader

This repository contains the code of unofficial [GOG](http://www.gog.com/) downloader.

## Dependencies

* libcurl
* liboauth
* librhash
* jsoncpp
* htmlcxx
* tinyxml
* boost (regex, date-time, system, filesystem, program-options)
* help2man

### Debian/Ubuntu

    # apt install build-essential libcurl4-openssl-dev libboost-regex-dev \
    libjsoncpp-dev liboauth-dev librhash-dev libtinyxml-dev libhtmlcxx-dev\
    libboost-system-dev libboost-filesystem-dev libboost-program-options-dev\
    libboost-date-time-dev help2man

## Build and install

    $ make release
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
