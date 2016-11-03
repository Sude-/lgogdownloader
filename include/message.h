/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef MESSAGE_H
#define MESSAGE_H

#include <boost/date_time/posix_time/posix_time.hpp>

const unsigned int MSGTYPE_INFO    = 1 << 0;
const unsigned int MSGTYPE_WARNING = 1 << 1;
const unsigned int MSGTYPE_ERROR   = 1 << 2;
const unsigned int MSGTYPE_SUCCESS = 1 << 3;

class Message
{
    public:
        Message() = default;
        Message(std::string msg, const unsigned int& type = MSGTYPE_INFO, const std::string& prefix = std::string())
        {
            prefix_ = prefix;
            msg_ = msg;
            type_ = type;
            timestamp_ = boost::posix_time::second_clock::local_time();
        }

        void setMessage(const std::string& msg)
        {
            msg_ = msg;
        }

        void setType(const unsigned int& type)
        {
            type_ = type;
        }

        void setTimestamp(const boost::posix_time::ptime& timestamp)
        {
            timestamp_ = timestamp;
        }

        void setPrefix(const std::string& prefix)
        {
            prefix_ = prefix;
        }

        std::string getMessage()
        {
            return msg_;
        }

        unsigned int getType()
        {
            return type_;
        }

        boost::posix_time::ptime getTimestamp()
        {
            return timestamp_;
        }

        std::string getTimestampString()
        {
            return boost::posix_time::to_simple_string(timestamp_);
        }

        std::string getPrefix()
        {
            return prefix_;
        }

        std::string getFormattedString(const bool& bColor = true, const bool& bPrefix = true)
        {
            std::string str;
            std::string color_value = "\033[39m"; // Default foreground color
            std::string color_reset = "\033[0m";

            if (type_ == MSGTYPE_INFO)
                color_value = "\033[39m"; // Default foreground color
            else if (type_ == MSGTYPE_WARNING)
                color_value = "\033[33m"; // Yellow
            else if (type_ == MSGTYPE_ERROR)
                color_value = "\033[31m"; // Red
            else if (type_ == MSGTYPE_SUCCESS)
                color_value = "\033[32m"; // Green

            str = msg_;
            if (!prefix_.empty() && bPrefix)
                str = prefix_ + " " + str;

            str = getTimestampString() + " " + str;

            if (bColor)
                str = color_value + str + color_reset;

            return str;
        }

    private:
        std::string msg_;
        boost::posix_time::ptime timestamp_;
        unsigned int type_;
        std::string prefix_;
};

#endif // MESSAGE_H
