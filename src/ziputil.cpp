/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "ziputil.h"

#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/regex.hpp>

off_t ZipUtil::getMojoSetupScriptSize(std::stringstream *stream)
{
    off_t mojosetup_script_size = -1;
    int script_lines = 0;
    boost::regex re("offset=`head -n (\\d+?) \"\\$0\"", boost::regex::perl | boost::regex::icase);
    boost::match_results<std::string::const_iterator> what;

    if (boost::regex_search(stream->str(), what, re))
    {
        script_lines = std::stoi(what[1]);
    }

    std::string script;
    for (int i = 0; i < script_lines; ++i)
    {
        std::string line;
        std::getline(*stream, line);
        script += line + "\n";
    }
    mojosetup_script_size = script.size();

    return mojosetup_script_size;
}

off_t ZipUtil::getMojoSetupInstallerSize(std::stringstream *stream)
{
    off_t mojosetup_installer_size = -1;
    boost::regex re("filesizes=\"(\\d+?)\"", boost::regex::perl | boost::regex::icase);
    boost::match_results<std::string::const_iterator> what;

    if (boost::regex_search(stream->str(), what, re))
    {
        mojosetup_installer_size = std::stoll(what[1]);
    }

    return mojosetup_installer_size;
}

struct tm ZipUtil::date_time_to_tm(uint64_t date, uint64_t time)
{
    /* DOS date time format
     * Y|Y|Y|Y|Y|Y|Y|M| |M|M|M|D|D|D|D|D| |h|h|h|h|h|m|m|m| |m|m|m|s|s|s|s|s
     *
     * second is divided by 2
     * month starts at 1
     * https://msdn.microsoft.com/en-us/library/windows/desktop/ms724247%28v=vs.85%29.aspx */

    uint64_t local_time_base_year = 1900;
    uint64_t dos_time_base_year = 1980;

    struct tm timeinfo;
    timeinfo.tm_year = (uint16_t)(((date & 0xFE00) >> 9) - local_time_base_year + dos_time_base_year);
    timeinfo.tm_mon = (uint16_t)(((date & 0x1E0) >> 5) - 1);
    timeinfo.tm_mday = (uint16_t)(date & 0x1F);
    timeinfo.tm_hour = (uint16_t)((time & 0xF800) >> 11);
    timeinfo.tm_min = (uint16_t)((time & 0x7E0) >> 5);
    timeinfo.tm_sec = (uint16_t)(2 * (time & 0x1F));
    timeinfo.tm_isdst = -1;

    return timeinfo;
}

bool ZipUtil::isValidDate(struct tm timeinfo)
{
    if (!(timeinfo.tm_year >= 0 && timeinfo.tm_year <= 207))
        return false;
    if (!(timeinfo.tm_mon >= 0 && timeinfo.tm_mon <= 11))
        return false;
    if (!(timeinfo.tm_mday >= 1 && timeinfo.tm_mday <= 31))
        return false;
    if (!(timeinfo.tm_hour >= 0 && timeinfo.tm_hour <= 23))
        return false;
    if (!(timeinfo.tm_min >= 0 && timeinfo.tm_min <= 59))
        return false;
    if (!(timeinfo.tm_sec >= 0 && timeinfo.tm_sec <= 59))
        return false;

    return true;
}

uint64_t ZipUtil::readValue(std::istream *stream, uint32_t len)
{
    uint64_t value = 0;

    for (uint32_t i = 0; i < len; i++)
    {
        value |= ((uint64_t)(stream->get() & 0xFF)) << (i * 8);
    }

    return value;
}

uint64_t ZipUtil::readUInt64(std::istream *stream)
{
    uint64_t value = (uint64_t)readValue(stream, sizeof(uint64_t));
    return value;
}

uint32_t ZipUtil::readUInt32(std::istream *stream)
{
    uint32_t value = (uint32_t)readValue(stream, sizeof(uint32_t));
    return value;
}

uint16_t ZipUtil::readUInt16(std::istream *stream)
{
    uint16_t value = (uint16_t)readValue(stream, sizeof(uint16_t));
    return value;
}

uint8_t ZipUtil::readUInt8(std::istream *stream)
{
    uint8_t value = (uint8_t)readValue(stream, sizeof(uint8_t));
    return value;
}

off_t ZipUtil::getZipEOCDOffsetSignature(std::istream *stream, const uint32_t& signature)
{
    off_t offset = -1;
    stream->seekg(0, stream->end);
    off_t stream_length = stream->tellg();

    for (off_t i = 4; i <= stream_length; i++)
    {
        off_t pos = stream_length - i;
        stream->seekg(pos, stream->beg);
        if (readUInt32(stream) == signature)
        {
            offset = stream->tellg();
            offset -= 4;
            break;
        }
    }

    return offset;
}

off_t ZipUtil::getZipEOCDOffset(std::istream *stream)
{
    return getZipEOCDOffsetSignature(stream, ZIP_EOCD_HEADER_SIGNATURE);
}

off_t ZipUtil::getZip64EOCDOffset(std::istream *stream)
{
    return getZipEOCDOffsetSignature(stream, ZIP_EOCD_HEADER_SIGNATURE64);
}

zipEOCD ZipUtil::readZipEOCDStruct(std::istream *stream, const off_t& eocd_start_pos)
{
	zipEOCD eocd;

	stream->seekg(eocd_start_pos, stream->beg);

	// end of central dir signature <4 bytes>
	eocd.header = readUInt32(stream);

	// number of this disk <2 bytes>
	eocd.disk = readUInt16(stream); // Number of this disk

	// number of the disk with the start of the central directory <2 bytes>
	eocd.cd_start_disk = readUInt16(stream);

	// total number of entries in the central directory on this disk <2 bytes>
	eocd.cd_records = readUInt16(stream);

	// total number of entries in the central directory <2 bytes>
	eocd.total_cd_records = readUInt16(stream);

	// size of the central directory <4 bytes>
	eocd.cd_size = readUInt32(stream);

	// offset of start of central directory with respect to the starting disk number <4 bytes>
	eocd.cd_start_offset = readUInt32(stream);

	// .ZIP file comment length <2 bytes>
	eocd.comment_length = readUInt16(stream);

	// .ZIP file comment <variable size>
	if (eocd.comment_length > 0)
	{
	    char *buf = new char[eocd.comment_length + 1];
		stream->read(buf, eocd.comment_length);
		eocd.comment = std::string(buf, eocd.comment_length);
		delete[] buf;
	}

	return eocd;
}

zip64EOCD ZipUtil::readZip64EOCDStruct(std::istream *stream, const off_t& eocd_start_pos)
{
    zip64EOCD eocd;

    stream->seekg(eocd_start_pos, stream->beg);

    // zip64 end of central dir signature <4 bytes>
    eocd.header = readUInt32(stream);

    // size of zip64 end of central directory record <8 bytes>
    eocd.directory_record_size = readUInt64(stream);
    /* The value stored into the "size of zip64 end of central
     * directory record" should be the size of the remaining
     * record and should not include the leading 12 bytes.
     *
     * Size = SizeOfFixedFields + SizeOfVariableData - 12 */

    // version made by <2 bytes>
    eocd.version_made_by = readUInt16(stream);

    // version needed to extract <2 bytes>
    eocd.version_needed = readUInt16(stream);

    // number of this disk <4 bytes>
    eocd.cd = readUInt32(stream);

    // number of the disk with the start of the central directory <8 bytes>
    eocd.cd_start = readUInt32(stream);

    // total number of entries in the central directory on this disk <8 bytes>
    eocd.cd_total_disk = readUInt64(stream);

    // total number of entries in the central directory <8 bytes>
    eocd.cd_total = readUInt64(stream);

    // size of the central directory <8 bytes>
    eocd.cd_size = readUInt64(stream);

    // offset of start of central directory with respect to the starting disk number <8 bytes>
    eocd.cd_offset = readUInt64(stream);

    // zip64 extensible data sector <variable size>
    // This is data is not needed for our purposes so we just ignore this data

    return eocd;
}

zipCDEntry ZipUtil::readZipCDEntry(std::istream *stream)
{
    zipCDEntry cd;
    char *buf;

    // file header signature <4 bytes>
    cd.header = readUInt32(stream);

    cd.isLocalCDEntry = (cd.header == ZIP_LOCAL_HEADER_SIGNATURE);

    if (!cd.isLocalCDEntry)
    {
        // version made by <2 bytes>
        cd.version_made_by = readUInt16(stream);
    }
    // version needed to extract <2 bytes>
    cd.version_needed = readUInt16(stream);

    // general purpose bit flag <2 bytes>
    cd.flag = readUInt16(stream);

    // compression method <2 bytes>
    cd.compression_method = readUInt16(stream);

    // last mod file time <2 bytes>
    cd.mod_time = readUInt16(stream);

    // last mod file date <2 bytes>
    cd.mod_date = readUInt16(stream);

    // crc-32 <4 bytes>
    cd.crc32 = readUInt32(stream);

    // compressed size <4 bytes>
    cd.comp_size = readUInt32(stream);

    // uncompressed size <4 bytes>
    cd.uncomp_size = readUInt32(stream);

    // file name length <2 bytes>
    cd.filename_length = readUInt16(stream);

    // extra field length <2 bytes>
    cd.extra_length = readUInt16(stream);

    if (!cd.isLocalCDEntry)
    {
        // file comment length <2 bytes>
        cd.comment_length = readUInt16(stream);

        // disk number start <2 bytes>
        cd.disk_num = readUInt16(stream);

        // internal file attributes <2 bytes>
        cd.internal_file_attr = readUInt16(stream);

        // external file attributes <4 bytes>
        cd.external_file_attr = readUInt32(stream);

        // relative offset of local header <4 bytes>
        cd.disk_offset = readUInt32(stream);
    }

    // file name <variable size>
    buf = new char[cd.filename_length + 1];
    stream->read(buf, cd.filename_length);
    cd.filename = std::string(buf, cd.filename_length);
    delete[] buf;

    // extra field <variable size>
    buf = new char[cd.extra_length + 1];
    stream->read(buf, cd.extra_length);
    cd.extra = std::string(buf, cd.extra_length);
    delete[] buf;
    std::stringstream extra_stream(cd.extra);

    cd.timestamp = 0;
    struct tm timeinfo = date_time_to_tm(cd.mod_date, cd.mod_time);
    if (isValidDate(timeinfo))
        cd.timestamp = mktime(&timeinfo);

    // Read extra fields
    off_t i = 0;
    while (i < cd.extra_length)
    {
        /* Extra field
         * <2 bytes> signature
         * <2 bytes> size of extra field data
         * <variable> extra field data */

        uint16_t header_id = readUInt16(&extra_stream);
        uint16_t extra_data_size = readUInt16(&extra_stream);

        if (header_id == ZIP_EXTENSION_ZIP64)
        {
            /* Zip64 Extended Information Extra Field
             * <8 bytes> size of uncompressed file
             * <8 bytes> size of compressed data
             * <8 bytes> offset of local header record
             * <4 bytes> number of the disk on which this file starts
             *
             * The fields only appear if the corresponding Local or Central
             * directory record field is set to UINT16_MAX or UINT32_MAX */

            if (cd.uncomp_size == UINT32_MAX)
                cd.uncomp_size = readUInt64(&extra_stream);
            if (cd.comp_size == UINT32_MAX)
                cd.comp_size = readUInt64(&extra_stream);
            if (cd.disk_offset == UINT32_MAX)
                cd.disk_offset = readUInt64(&extra_stream);
            if (cd.disk_num == UINT16_MAX)
                cd.disk_num = readUInt32(&extra_stream);
        }
        else if (header_id == ZIP_EXTENDED_TIMESTAMP)
        {
            /* Extended Timestamp Extra Field
             *
             * Local header version
             * <1 byte> info bits
             * <4 bytes> modification time
             * <4 bytes> access time
             * <4 bytes> creation time
             *
             * Central header version
             * <1 byte> info bits
             * <4 bytes> modification time
             *
             * The lower three info bits in both headers indicate
             * which timestamps are present in the local extra field
             * bit 0 if set, modification time is present
             * bit 1 if set, access time is present
             * bit 2 if set, creation time is present
             * bits 3-7 reserved for additional timestamps; not set
             *
             * If info bits indicate that modification time is present
             * in the local header field, it must be present in the
             * central header field.
             * Those times that are present will appear in the order
             * indicated, but any combination of times may be omitted. */

            uint32_t modification_time = 0;
            uint32_t access_time = 0;
            uint32_t creation_time = 0;

            uint8_t flags = readUInt8(&extra_stream);

            if (flags & 0x1) // modification time is present
            {
                modification_time = readUInt32(&extra_stream);
                cd.timestamp = modification_time;
            }

            if (cd.isLocalCDEntry)
            {
                if (flags & 0x2) // access time is present
                {
                    access_time = readUInt32(&extra_stream);
                }

                if (flags & 0x4) // creation time is present
                {
                    creation_time = readUInt32(&extra_stream);
                }
            }

            // access time and creation time are unused currently
            // suppress -Wunused-but-set-variable messages by casting these variables to void
            (void) access_time;
            (void) creation_time;
        }
        else if (header_id == ZIP_INFOZIP_UNIX_NEW)
        {
            /* Info-ZIP New Unix Extra Field
             * <1 byte> version
             * <1 byte> size of uid
             * <variable> uid
             * <1 byte> size of gid
             * <variable> gid
             *
             * Currently Version is set to the number 1. If there is a need
             * to change this field, the version will be incremented.
             * UID and GID entries are stored in standard little endian format */

            uint8_t version = readUInt8(&extra_stream);
            if (version == 1)
            {
                uint64_t uid = 0;
                uint64_t gid = 0;

                uint8_t uid_size = readUInt8(&extra_stream);
                for (uint8_t i = 0; i < uid_size; i++)
                {
                    uid |= ((uint64_t)extra_stream.get()) << (i * 8);
                }

                uint8_t gid_size = readUInt8(&extra_stream);
                for (uint8_t i = 0; i < gid_size; i++)
                {
                    gid |= ((uint64_t)extra_stream.get()) << (i * 8);
                }
            }
            else
            {
                // Unknown version
                // Skip the rest of this field
                extra_stream.seekg(extra_data_size - 1, extra_stream.cur);
            }
        }
        else
        {
            // Skip over unknown/unimplemented extra field
            extra_stream.seekg(extra_data_size, extra_stream.cur);
        }
        i += 4 + extra_data_size;
    }

    // file comment <variable size>
    buf = new char[cd.comment_length + 1];
    stream->read(buf, cd.comment_length);
    cd.comment = std::string(buf, cd.comment_length);
    delete[] buf;

    return cd;
}

/* Extract file
    returns 0 if successful
    returns 1 if input file could not be opened
    returns 2 if compression method is unsupported
    returns 3 if output file could not be created
    returns 4 if zlib error
*/
int ZipUtil::extractFile(const std::string& input_file_path, const std::string& output_file_path)
{
    std::ifstream input_file(input_file_path, std::ifstream::in | std::ifstream::binary);

    if (!input_file)
    {
        // Could not open input file
        return 1;
    }

    // Read header
    zipCDEntry cd = readZipCDEntry(&input_file);

    if (!(cd.compression_method == boost::iostreams::zlib::deflated || cd.compression_method == boost::iostreams::zlib::no_compression))
    {
        // Unsupported compression method
        return 2;
    }

    boost::iostreams::zlib_params p;
    p.window_bits = 15;
    p.noheader = true; // zlib header and trailing adler-32 checksum is omitted

    std::ofstream output_file(output_file_path, std::ofstream::out | std::ofstream::binary);
    if (!output_file)
    {
        // Failed to create output file
        return 3;
    }

    // Uncompress
    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;

    if (cd.compression_method == boost::iostreams::zlib::deflated)
        in.push(boost::iostreams::zlib_decompressor(p));

    in.push(input_file);
    try
    {
        boost::iostreams::copy(in, output_file);
    }
    catch(boost::iostreams::zlib_error & e)
    {
        // zlib error
        return 4;
    }

    input_file.close();
    output_file.close();

    if (cd.timestamp > 0)
        boost::filesystem::last_write_time(output_file_path, cd.timestamp);

    return 0;
}

/* Extract stream to stream
    returns 0 if successful
    returns 1 if input stream is not valid
    returns 2 if compression method is unsupported
    returns 3 if output stream is not valid
    returns 4 if zlib error
*/
int ZipUtil::extractStream(std::istream* input_stream, std::ostream* output_stream)
{
    if (!input_stream)
    {
        // Input stream not valid
        return 1;
    }

    // Read header
    zipCDEntry cd = readZipCDEntry(input_stream);

    if (!(cd.compression_method == boost::iostreams::zlib::deflated || cd.compression_method == boost::iostreams::zlib::no_compression))
    {
        // Unsupported compression method
        return 2;
    }

    boost::iostreams::zlib_params p;
    p.window_bits = 15;
    p.noheader = true; // zlib header and trailing adler-32 checksum is omitted

    if (!output_stream)
    {
        // Output stream not valid
        return 3;
    }

    // Uncompress
    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;

    if (cd.compression_method == boost::iostreams::zlib::deflated)
        in.push(boost::iostreams::zlib_decompressor(p));

    in.push(*input_stream);
    try
    {
        boost::iostreams::copy(in, *output_stream);
    }
    catch(boost::iostreams::zlib_error & e)
    {
        // zlib error
        return 4;
    }

    return 0;
}

boost::filesystem::perms ZipUtil::getBoostFilePermission(const uint16_t& attributes)
{
    boost::filesystem::perms perms = boost::filesystem::no_perms;
    if (attributes & S_IRUSR)
        perms |= boost::filesystem::owner_read;
    if (attributes & S_IWUSR)
        perms |= boost::filesystem::owner_write;
    if (attributes & S_IXUSR)
        perms |= boost::filesystem::owner_exe;

    if (attributes & S_IRGRP)
        perms |= boost::filesystem::group_read;
    if (attributes & S_IWGRP)
        perms |= boost::filesystem::group_write;
    if (attributes & S_IXGRP)
        perms |= boost::filesystem::group_exe;

    if (attributes & S_IROTH)
        perms |= boost::filesystem::others_read;
    if (attributes & S_IWOTH)
        perms |= boost::filesystem::others_write;
    if (attributes & S_IXOTH)
        perms |= boost::filesystem::others_exe;

    return perms;
}

bool ZipUtil::isSymlink(const uint16_t& attributes)
{
    bool bSymlink = ((attributes & S_IFMT) == S_IFLNK);
    return bSymlink;
}
