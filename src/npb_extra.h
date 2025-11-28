#pragma once

#include "stdint.h"

struct npb_data
{
        uint8_t* data;
        uint32_t size;
};

struct npb_str
{
        char const*     str;
        struct npb_str* next;
};