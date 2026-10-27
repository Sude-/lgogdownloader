/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef DOWNLOADSTATS_H
#define DOWNLOADSTATS_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

class gameFile;

// Outcome of a single file in the download queue.
// Failed is the default so that any unhandled exit path is counted as a failure
// instead of silently vanishing from the statistics.
enum class FileOutcome
{
    Failed = 0,
    Ok,
    UpToDate,
    Skipped
};

class DownloadStats
{
    public:
        struct Snapshot
        {
            unsigned long long games_total = 0;
            unsigned long long games_ok = 0;
            unsigned long long games_failed = 0;
            unsigned long long games_remaining = 0;
            unsigned long long files_total = 0;
            unsigned long long files_ok = 0;
            unsigned long long files_uptodate = 0;
            unsigned long long files_skipped = 0;
            unsigned long long files_failed = 0;
            unsigned long long files_remaining = 0;
            unsigned long long bytes_total = 0;
            unsigned long long bytes_done = 0;
        };

        // Called while building the download queue, before any thread is started
        void addFile(const gameFile& gf, const unsigned long long& filesize);

        // Called exactly once for every file that was popped from the download queue
        void recordOutcome(const gameFile& gf, const FileOutcome& outcome,
                           const unsigned long long& filesize, const std::string& filename);

        Snapshot get() const;
        std::vector<std::string> getFailedFiles() const;
        bool empty() const;
        void clear();

    private:
        // DLC files are accounted to their base game
        static std::string getGameKey(const gameFile& gf);

        struct gameProgress
        {
            unsigned long long files_total = 0;
            unsigned long long files_remaining = 0;
            unsigned long long files_failed = 0;
            bool done = false;
        };

        mutable std::mutex m;
        std::map<std::string, gameProgress> games;
        std::vector<std::string> failed_files;
        Snapshot stats;
};

extern DownloadStats downloadStats;

#endif // DOWNLOADSTATS_H
