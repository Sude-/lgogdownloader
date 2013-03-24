/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef PROGRESSBAR_H
#define PROGRESSBAR_H

#include <iostream>
#include <vector>

class ProgressBar
{
    public:
        ProgressBar(bool bUnicode, bool bColor);
        virtual ~ProgressBar();
        void draw(unsigned int length, double fraction);
    protected:
    private:
        std::vector<std::string> const m_bar_chars;
        std::string const m_left_border;
        std::string const m_right_border;
        std::string const m_simple_left_border;
        std::string const m_simple_right_border;
        std::string const m_simple_empty_fill;
        std::string const m_simple_bar_char;
        std::string const m_bar_color;
        std::string const m_border_color;
        std::string const COLOR_RESET;
        bool m_use_unicode;
        bool m_use_color;
};

#endif // PROGRESSBAR_H
