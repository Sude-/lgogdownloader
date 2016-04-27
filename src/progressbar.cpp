/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "progressbar.h"
#include <cmath>
#include <sstream>

ProgressBar::ProgressBar(bool bUnicode, bool bColor)
:
    // Based on block characters.
    // See https://en.wikipedia.org/wiki/List_of_Unicode_characters#Block_elements
    // u8"\u2591" - you can try using this ("light shade") instead of space, but it looks worse,
    //              since partial bar has no shade behind it.
    m_bar_chars
    {
        " ",        // 0/8
        u8"\u258F", // 1/8
        u8"\u258E", // 2/8
        u8"\u258D", // 3/8
        u8"\u258C", // 4/8
        u8"\u258B", // 5/8
        u8"\u258A", // 6/8
        u8"\u2589", // 7/8
        u8"\u2588"  /* 8/8 */
    },
    m_left_border(u8"\u2595"),  // right 1/8th
    m_right_border(u8"\u258F"), // left  1/8th
    m_simple_left_border("["),
    m_simple_right_border("]"),
    m_simple_empty_fill(" "),
    m_simple_bar_char("="),
    // using vt100 escape sequences for colors... See http://ascii-table.com/ansi-escape-sequences.php
    m_bar_color("\033[1;34m"),
    m_border_color("\033[1;37m"),
    COLOR_RESET("\033[0m"),
    m_use_unicode(bUnicode),
    m_use_color(bColor)
{ }

ProgressBar::~ProgressBar()
{
    //dtor
}

void ProgressBar::draw(unsigned int length, double fraction)
{
    std::cout << createBarString(length, fraction);
}

std::string ProgressBar::createBarString(unsigned int length, double fraction)
{
    std::ostringstream ss;
    // validation
    if (!std::isnormal(fraction) || (fraction < 0.0)) fraction = 0.0;
    else if (fraction > 1.0) fraction = 1.0;

    double bar_part                = fraction * length;
    double whole_bar_chars         = std::floor(bar_part);
    unsigned int whole_bar_chars_i = (unsigned int) whole_bar_chars;
    // The bar uses symbols graded with 1/8
    unsigned int partial_bar_char_index = (unsigned int) std::floor((bar_part - whole_bar_chars) * 8.0);

    // left border
    if (m_use_color) ss << m_border_color;
    ss << (m_use_unicode ? m_left_border : m_simple_left_border);

    // whole completed bars
    if (m_use_color) ss << m_bar_color;
    unsigned int i = 0;
    for (; i < whole_bar_chars_i; i++)
    {
        ss << (m_use_unicode ? m_bar_chars[8] : m_simple_bar_char);
    }

    // partial completed bar
    if (i < length) ss << (m_use_unicode ? m_bar_chars[partial_bar_char_index] : m_simple_empty_fill);

    // whole unfinished bars
    if (m_use_color) ss << COLOR_RESET;
    for (i = whole_bar_chars_i + 1; i < length; i++)
    {  // first entry in m_bar_chars is assumed to be the empty bar
        ss << (m_use_unicode ? m_bar_chars[0] : m_simple_empty_fill);
    }

    // right border
    if (m_use_color) ss << m_border_color;
    ss << (m_use_unicode ? m_right_border : m_simple_right_border);
    if (m_use_color) ss << COLOR_RESET;

    return ss.str();
}
