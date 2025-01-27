/*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*   Product name: redemption, a FLOSS RDP proxy
*   Copyright (C) Wallix 2010-2016
*   Author(s): Jonathan Poelen
*/

#pragma once

#include "rvt/character.hpp"

#include <string_view>
#include <vector>

namespace rvt {

class Screen;

struct RenderingBuffer
{
    using ExtraMemoryAllocator = uint8_t*(void* ctx, std::size_t* extra_capacity_in_out, uint8_t* p, std::size_t used_size);
    using SetFinalBuffer = void(void* ctx, uint8_t* p, std::size_t used_size);

    void* ctx;
    char* buffer;
    std::size_t length;
    ExtraMemoryAllocator* allocate;
    SetFinalBuffer* set_final_buffer;

    static RenderingBuffer from_vector(std::vector<char>& v);
    static RenderingBuffer from_vector(std::vector<uint8_t>& v);
};

void json_rendering(
    ucs4_carray_view title, Screen const & screen,
    ColorTableView palette, RenderingBuffer buffer,
    std::string_view extra_data = {}
);

void ansi_rendering(
    ucs4_carray_view title, Screen const & screen,
    ColorTableView palette, RenderingBuffer buffer,
    std::string_view extra_data = {}
);

struct TranscriptPartialBuffer
{
    char* buffer;
    std::size_t length;
    std::size_t capacity;
};

TranscriptPartialBuffer transcript_partial_rendering(
    Screen const & screen, size_t y, size_t yend,
    RenderingBuffer buffer, std::size_t consumed_buffer
);

}
