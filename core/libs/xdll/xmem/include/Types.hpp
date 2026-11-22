#pragma once

#include <windows.h>

#include <cstdint>

struct XMemVirtualQueryInfo {
    MEMORY_BASIC_INFORMATION basic;
    std::uint8_t console[16];
};

static_assert(sizeof(XMemVirtualQueryInfo) == 64);
