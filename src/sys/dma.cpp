#include <cstddef>
#include <cstdint>
#include "sys/dma.h"

// The host has no data cache to maintain.
extern "C" void dsy_dma_clear_cache_for_buffer(uint8_t* buffer, size_t size) {}
extern "C" void dsy_dma_invalidate_cache_for_buffer(uint8_t* buffer, size_t size) {}
