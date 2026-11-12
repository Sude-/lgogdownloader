/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "obsolete.h"

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>

namespace fs = boost::filesystem;

static int failures = 0;

static void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        failures++;
    }
}

static void touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path.string());
    file << "test";
}

int main()
{
    const fs::path root = fs::temp_directory_path()
        / ("lgog-obsolete-tests-" + std::to_string(static_cast<long long>(getpid())));
    fs::remove_all(root);
    fs::create_directories(root);

    try
    {
        const fs::path current_exe = root / "game/windows/setup_game_2.0.exe";
        const fs::path current_bin = root / "game/windows/setup_game_2.0-1.bin";
        const fs::path old_exe = root / "game/windows/setup_game_1.0.exe";
        const fs::path old_bin = root / "game/windows/setup_game_1.0-1.bin";
        const fs::path linux_installer = root / "game/linux/game_2_0.sh";
        const fs::path mac_installer = root / "game/mac/game_2_0.pkg";
        const fs::path patch = root / "game/windows/patch_game_1.0_to_2.0.exe";
        const fs::path extra = root / "game/extras/manual.zip";
        const fs::path x86_installer = root / "game/windows/setup_game_2.0_(32bit).exe";
        const fs::path x64_installer = root / "game/windows/setup_game_2.0_(64bit).exe";
        const fs::path dlc_installer = root / "game/dlc/expansion/setup_expansion_2.0.exe";
        const fs::path language_installer = root / "game/windows/setup_game_2.0_german.exe";
        const fs::path disc_image = root / "game/windows/game_bonus_disc.iso";
        const fs::path metadata = root / "game/game-details.json";

        for (const auto& path : {current_exe, current_bin, old_exe, old_bin, linux_installer,
                                 mac_installer, patch, extra, x86_installer, x64_installer,
                                 dlc_installer, language_installer, disc_image, metadata})
            touch(path);

        const boost::regex expression(".*\\.(zip|exe|bin|dmg|old|deb|tar\\.gz|pkg|sh|mp4|iso)$");
        auto candidates = Obsolete::collectCandidates(root / "game", expression);
        expect(candidates.size() == 13, "candidate scan excludes metadata and includes all installer formats");

        std::set<std::string> selected = {
            Obsolete::pathKey(current_exe),
            Obsolete::pathKey(current_bin),
            Obsolete::pathKey(linux_installer),
            Obsolete::pathKey(extra),
            Obsolete::pathKey(x86_installer),
            Obsolete::pathKey(x64_installer),
            Obsolete::pathKey(dlc_installer),
            Obsolete::pathKey(disc_image)
        };
        // "selected" is what this run would download; "current_catalog" is everything
        // GOG still offers. The patch stands for a type left out by --include: still
        // advertised, therefore protected.
        std::set<std::string> current_catalog = selected;
        current_catalog.insert(Obsolete::pathKey(mac_installer));
        current_catalog.insert(Obsolete::pathKey(language_installer));
        current_catalog.insert(Obsolete::pathKey(patch));
        auto obsolete = Obsolete::findObsolete(candidates, selected, current_catalog);
        std::set<std::string> obsolete_keys;
        for (const auto& path : obsolete)
            obsolete_keys.insert(Obsolete::pathKey(path));

        expect(obsolete.size() == 2, "only the superseded multipart family is obsolete");
        expect(obsolete_keys.count(Obsolete::pathKey(old_exe)) == 1, "old executable is obsolete");
        expect(obsolete_keys.count(Obsolete::pathKey(old_bin)) == 1, "old split payload is obsolete");
        expect(obsolete_keys.count(Obsolete::pathKey(patch)) == 0,
               "a file the current catalog still advertises is retained even when not selected for download");
        expect(obsolete_keys.count(Obsolete::pathKey(current_bin)) == 0, "current split payload is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(x86_installer)) == 0, "current 32-bit alternative is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(x64_installer)) == 0, "current 64-bit alternative is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(dlc_installer)) == 0, "current DLC installer is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(mac_installer)) == 0, "current non-selected platform installer is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(language_installer)) == 0, "current non-selected language installer is retained");
        expect(obsolete_keys.count(Obsolete::pathKey(disc_image)) == 0, "current ISO is retained");
        expect(Obsolete::isWithin(root / "game", current_exe), "game file is inside its scan root");
        const fs::path trailing_root((root / "game").generic_string() + "/");
        expect(Obsolete::isWithin(trailing_root, current_exe), "trailing root separator does not reject a game file");
        expect(Obsolete::pathKey(trailing_root) == Obsolete::pathKey(root / "game"), "path key ignores trailing separators");
        expect(!Obsolete::isWithin(root / "gam", current_exe), "path prefix is not treated as a parent directory");
        expect(!Obsolete::isWithin(root / "other", current_exe), "unrelated root does not own game file");
        expect(Obsolete::relativeKey(root, current_exe) == "game/windows/setup_game_2.0.exe",
               "relative key follows documented directory-relative blacklist format");
        expect(Obsolete::relativeKey(root / "gam", current_exe) == Obsolete::pathKey(current_exe),
               "relative key does not strip a mere path prefix");

        // An empty catalog condemns everything, which is why Downloader::checkObsolete
        // refuses to delete from a root the live catalog described no file for.
        expect(Obsolete::findObsolete(candidates, {}, {}).size() == candidates.size(),
               "an empty catalog marks every candidate obsolete");

        // The scan must never leave the game root.
        expect(Obsolete::collectCandidates(root / "missing", expression).empty(),
               "a root that does not exist yields no candidates");
        expect(Obsolete::collectCandidates(current_exe, expression).empty(),
               "a regular file passed as a root yields no candidates");

        const fs::path outside_dir = root / "outside";
        const fs::path outside_file = outside_dir / "unrelated_setup.exe";
        touch(outside_file);
        boost::system::error_code symlink_ec;
        fs::create_directory_symlink(outside_dir, root / "game" / "linked", symlink_ec);
        if (!symlink_ec)
        {
            // Compare resolved paths: a candidate reached through the symlink is
            // still lexically inside the root, so only canonical form can tell.
            const fs::path outside_canonical = fs::canonical(outside_file);
            for (const auto& candidate : Obsolete::collectCandidates(root / "game", expression))
                expect(fs::canonical(candidate) != outside_canonical,
                       "the scan does not follow a directory symlink out of the game root");
        }
    }
    catch (...)
    {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
    if (failures == 0)
        std::cout << "All obsolete-file tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
