#pragma once 
 
#include <util/system/types.h>
 
// Threadsafe 
ui32 Crc32c(const void* p, size_t size) noexcept;
ui32 Crc32cExtend(ui32 init, const void* data, size_t n) noexcept;

bool HaveFastCrc32c() noexcept;
