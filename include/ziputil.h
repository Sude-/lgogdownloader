/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef ZIPUTIL_H
#define ZIPUTIL_H

#include <cstdint>
#include <ctime>
#include <string>
#include <iostream>
#include <boost/filesystem.hpp>

#define ZIP_LOCAL_HEADER_SIGNATURE  0x04034b50
#define ZIP_CD_HEADER_SIGNATURE     0x02014b50
#define ZIP_EOCD_HEADER_SIGNATURE   0x06054b50
#define ZIP_EOCD_HEADER_SIGNATURE64 0x06064b50
#define ZIP_EXTENSION_ZIP64         0x0001
#define ZIP_EXTENDED_TIMESTAMP      0x5455
#define ZIP_INFOZIP_UNIX_NEW        0x7875

typedef struct
{
    uint32_t header = 0;
    uint16_t disk = 0;
    uint16_t cd_start_disk = 0;
    uint16_t cd_records = 0;
    uint16_t total_cd_records = 0;
    uint32_t cd_size = 0;
    uint32_t cd_start_offset = 0;
    uint16_t comment_length = 0;

    std::string comment;
} zipEOCD;

typedef struct
{
    uint32_t header = 0;
    uint64_t directory_record_size = 0;
    uint16_t version_made_by = 0;
    uint16_t version_needed = 0;
    uint32_t cd = 0;
    uint32_t cd_start = 0;
    uint64_t cd_total_disk = 0;
    uint64_t cd_total = 0;
    uint64_t cd_size = 0;
    uint64_t cd_offset = 0;

    std::string comment;
} zip64EOCD;

typedef struct
{
    uint32_t header = 0;
    uint16_t version_made_by = 0;
    uint16_t version_needed = 0;
    uint16_t flag = 0;
    uint16_t compression_method = 0;
    uint16_t mod_date = 0;
    uint16_t mod_time = 0;
    uint32_t crc32 = 0;
    uint64_t comp_size = 0;
    uint64_t uncomp_size = 0;
    uint16_t filename_length = 0;
    uint16_t extra_length = 0;
    uint16_t comment_length = 0;
    uint32_t disk_num = 0;
    uint16_t internal_file_attr = 0;
    uint32_t external_file_attr = 0;
    uint64_t disk_offset = 0;

    std::string filename;
    std::string extra;
    std::string comment;
    time_t timestamp = 0;
    bool isLocalCDEntry = false;
} zipCDEntry;

namespace ZipUtil
{
    off_t getMojoSetupScriptSize(std::stringstream *stream);
    off_t getMojoSetupInstallerSize(std::stringstream *stream);

    struct tm date_time_to_tm(uint64_t date, uint64_t time);
    bool isValidDate(struct tm timeinfo);

    uint64_t readValue(std::istream *stream, uint32_t len);
    uint64_t readUInt64(std::istream *stream);
    uint32_t readUInt32(std::istream *stream);
    uint16_t readUInt16(std::istream *stream);
    uint8_t readUInt8(std::istream *stream);

    off_t getZipEOCDOffsetSignature(std::istream *stream, const uint32_t& signature);
    off_t getZipEOCDOffset(std::istream *stream);
    off_t getZip64EOCDOffset(std::istream *stream);

    zipEOCD readZipEOCDStruct(std::istream *stream, const off_t& eocd_start_pos = 0);
    zip64EOCD readZip64EOCDStruct(std::istream *stream, const off_t& eocd_start_pos = 0);
    zipCDEntry readZipCDEntry(std::istream *stream);

    int extractFile(const std::string& input_file_path, const std::string& output_file_path);
    int extractStream(std::istream* input_stream, std::ostream* output_stream);
    boost::filesystem::perms getBoostFilePermission(const uint16_t& attributes);
    bool isSymlink(const uint16_t& attributes);
}

#endif // ZIPUTIL_H
