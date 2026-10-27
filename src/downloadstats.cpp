/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "downloadstats.h"
#include "gamefile.h"

std::string DownloadStats::getGameKey(const gameFile& gf)
{
    return gf.gamename_basegame.empty() ? gf.gamename : gf.gamename_basegame;
}

void DownloadStats::addFile(const gameFile& gf, const unsigned long long& filesize)
{
    std::unique_lock<std::mutex> lock(m);

    gameProgress& gp = games[getGameKey(gf)];
    if (gp.files_total == 0)
        stats.games_total++;

    gp.files_total++;
    gp.files_remaining++;

    stats.files_total++;
    stats.bytes_total += filesize;
}

void DownloadStats::recordOutcome(const gameFile& gf, const FileOutcome& outcome,
                                  const unsigned long long& filesize, const std::string& filename)
{
    std::unique_lock<std::mutex> lock(m);

    switch (outcome)
    {
        case FileOutcome::Ok:
            stats.files_ok++;
            stats.bytes_done += filesize;
            break;
        case FileOutcome::UpToDate:
            stats.files_uptodate++;
            stats.bytes_done += filesize;
            break;
        case FileOutcome::Skipped:
            stats.files_skipped++;
            stats.bytes_done += filesize;
            break;
        case FileOutcome::Failed:
        default:
            stats.files_failed++;
            failed_files.push_back(filename.empty() ? gf.path : filename);
            break;
    }

    auto it = games.find(getGameKey(gf));
    if (it != games.end())
    {
        gameProgress& gp = it->second;

        if (outcome == FileOutcome::Failed)
            gp.files_failed++;

        if (gp.files_remaining > 0)
            gp.files_remaining--;

        if (!gp.done && gp.files_remaining == 0)
        {
            gp.done = true;
            if (gp.files_failed > 0)
                stats.games_failed++;
            else
                stats.games_ok++;
        }
    }
}

DownloadStats::Snapshot DownloadStats::get() const
{
    std::unique_lock<std::mutex> lock(m);

    Snapshot snapshot = stats;

    unsigned long long files_accounted = snapshot.files_ok + snapshot.files_uptodate
                                       + snapshot.files_skipped + snapshot.files_failed;
    snapshot.files_remaining = (snapshot.files_total > files_accounted)
                             ? (snapshot.files_total - files_accounted) : 0;

    unsigned long long games_done = snapshot.games_ok + snapshot.games_failed;
    snapshot.games_remaining = (snapshot.games_total > games_done)
                             ? (snapshot.games_total - games_done) : 0;

    return snapshot;
}

std::vector<std::string> DownloadStats::getFailedFiles() const
{
    std::unique_lock<std::mutex> lock(m);
    return failed_files;
}

bool DownloadStats::empty() const
{
    std::unique_lock<std::mutex> lock(m);
    return (stats.files_total == 0);
}

void DownloadStats::clear()
{
    std::unique_lock<std::mutex> lock(m);
    games.clear();
    failed_files.clear();
    stats = Snapshot();
}
