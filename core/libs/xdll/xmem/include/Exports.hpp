#pragma once

#include "AbxApi.hpp"
#include "xdll/xmem/include/Types.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

ABX_API void* XMemAlloc(std::size_t size, std::uint64_t attributes);
ABX_API void XMemFree(void* address, std::uint64_t attributes);
ABX_API BOOL XMemVirtualQuery(HANDLE process, const void* address, XMemVirtualQueryInfo* info);
ABX_API BOOL XMemGetWorkingSetStatistics(std::uint32_t request, void* statistics);
