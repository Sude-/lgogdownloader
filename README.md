# LGOGDownloader

This repository contains the code of LGOGDownloader which is unofficial open source downloader for [GOG.com](https://www.gog.com/).
It uses the same API as GOG Galaxy which doesn't have Linux support at the moment.

## Dependencies

* [libcurl](https://curl.haxx.se/libcurl/) >= 7.55.0
* [librhash](https://github.com/rhash/RHash)
* [jsoncpp](https://github.com/open-source-parsers/jsoncpp)
* [libtidy](https://www.html-tidy.org/)
* [tinyxml2](https://github.com/leethomason/tinyxml2)
* [boost](http://www.boost.org/) (regex, date-time, system, filesystem, program-options, iostreams)
* [zlib](https://www.zlib.net/)
* [qtwebengine](https://www.qt.io/) if built with -DUSE_QT_GUI=ON

## Make dependencies
* [cmake](https://cmake.org/) >= 3.18.0
* [ninja](https://github.com/ninja-build/ninja)

## Debian/Ubuntu

    # apt install build-essential libcurl4-openssl-dev libboost-regex-dev \
    libjsoncpp-dev librhash-dev libtinyxml2-dev libtidy-dev \
    libboost-system-dev libboost-filesystem-dev libboost-program-options-dev \
    libboost-date-time-dev libboost-iostreams-dev cmake \
    pkg-config zlib1g-dev qtwebengine5-dev ninja-build

### Build and install

    $ cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DUSE_QT_GUI=ON -GNinja
    $ ninja -Cbuild install

## Fedora
```
sudo dnf install cmake make gcc gcc-c++ glibc tinyxml2-devel rhash-devel \
libtidy-devel tinyxml-devel jsoncpp-devel libcurl-devel \
boost-devel
```
### Build and Install
```
cmake ..
make
```

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

- **Removing superseded installers after an update**

        # Report local files that GOG's current catalog no longer offers
        lgogdownloader --check-obsolete

        # Download the selected current files, verify them, then delete superseded files
        lgogdownloader --download --check-obsolete --delete-obsolete \
            --platform windows+linux --language en+de

  A file is obsolete only when GOG's *current catalog no longer offers it*. The
  protected set is always the whole catalog: every platform, every language and
  every file type, regardless of what `--platform`, `--language`, `--include` or
  `--exclude` selected for this run. Those options still control what gets
  downloaded and verified, but a file GOG still offers is never deleted just
  because this run was not asked to download it. Resolving the full catalog is
  what makes the check safe, and also what makes it slow: even a no-download
  obsolete scan can take several minutes on a large library.
  It compares exact paths from GOG metadata, so split `.exe`/`.bin` installers,
  DLCs and legitimate architecture or edition alternatives do not rely on
  filename version parsing.

  `--delete-obsolete` deletes from a game directory only after every selected
  current file in that directory exists and matches its exact current download
  size and, for installers, patches and language packs, its freshly computed
  checksum; extras are verified by size alone. Deletion from a game directory is
  refused if size or checksum metadata cannot be retrieved, if the current
  catalog listed no file at all for that directory, if the run has no selected
  current file there to verify against, or if that game's catalog metadata came
  back incomplete. An incomplete response is never treated as evidence that a
  local file is obsolete: the affected game is reported but never pruned, while
  the rest of the library is still checked.
  Verification is performed only for directories that actually have obsolete
  candidates. Blacklisted and ignorelisted files are never reported or deleted.
  `--check-obsolete` always uses the live catalog and never the `--use-cache`
  copy.

  Like `--check-orphans`, the scan treats every file under a game directory as
  something lgogdownloader manages. If you point `--subdir-galaxy-install` at the
  same directory as `--subdir-game`, a Galaxy-installed game tree will be seen as
  obsolete; keep the two apart.

  Two things the current catalog cannot describe, so a local copy of them is
  reported as obsolete: files GOG lists for information only (`count` and
  `total_size` of zero, skipped since #200), and files belonging to a DLC the
  account no longer owns. Neither has a resolvable download path, so there is
  nothing to compare a local file against. Blacklist them if you keep such files.

  A game is skipped when `%version%` appears in one of its directory templates.
  The game root has no version of its own, so it would span every version
  directory while the catalog paths inside it are version specific, and anything
  left at a previous version's path would look obsolete.

  `--check-obsolete` exits non-zero if any game was skipped or any game directory
  was refused, so `exit 0` means the whole selection was checked. With
  `--delete-obsolete` it also exits non-zero if a deletion failed.

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
