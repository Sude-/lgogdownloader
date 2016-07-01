/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef DOWNLOADINFO_H
#define DOWNLOADINFO_H

#include <curl/curl.h>
#include <mutex>

const unsigned int DLSTATUS_NOTSTARTED = 0;
const unsigned int DLSTATUS_STARTING   = 1 << 0;
const unsigned int DLSTATUS_RUNNING    = 1 << 1;
const unsigned int DLSTATUS_FINISHED   = 1 << 2;

struct progressInfo
{
    curl_off_t dlnow;
    curl_off_t dltotal;
    double rate;
    double rate_avg;
};

class DownloadInfo
{
    public:
        void setFilename(const std::string& filename_)
        {
            std::unique_lock<std::mutex> lock(m);
            filename = filename_;
        }

        std::string getFilename()
        {
            std::unique_lock<std::mutex> lock(m);
            return filename;
        }

        void setStatus(const unsigned int& status_)
        {
            std::unique_lock<std::mutex> lock(m);
            status = status_;
        }

        unsigned int getStatus()
        {
            std::unique_lock<std::mutex> lock(m);
            return status;
        }

        void setProgressInfo(const progressInfo& info)
        {
            std::unique_lock<std::mutex> lock(m);
            progress_info = info;
        }

        progressInfo getProgressInfo()
        {
            std::unique_lock<std::mutex> lock(m);
            return progress_info;
        }

        DownloadInfo()=default;

        DownloadInfo(const DownloadInfo& other)
        {
            std::lock_guard<std::mutex> guard(other.m);
            filename = other.filename;
            status = other.status;
            progress_info = other.progress_info;
        }

        DownloadInfo& operator= (DownloadInfo& other)
        {
            if(&other == this)
                return *this;

            std::unique_lock<std::mutex> lock1(m, std::defer_lock);
            std::unique_lock<std::mutex> lock2(other.m, std::defer_lock);
            std::lock(lock1, lock2);
            filename = other.filename;
            status = other.status;
            progress_info = other.progress_info;
            return *this;
        }
    private:
        std::string filename;
        unsigned int status;
        progressInfo progress_info;
        mutable std::mutex m;
};

#endif // DOWNLOADINFO_H
