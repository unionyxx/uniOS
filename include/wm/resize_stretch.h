#pragma once

#include <stdint.h>

namespace wm {

// Nearest-neighbor coordinate mapping for stretching src_len pixels across
// dst_len pixels. Pure integer math shared verbatim between the compositor's
// in-flight resize stretch and the kernel ktest suite.
constexpr int stretch_map_coord(int dst_coord, int src_len, int dst_len)
{
    if (src_len <= 0 || dst_len <= 0 || dst_coord < 0)
        return 0;
    int64_t mapped = (static_cast<int64_t>(dst_coord) * src_len) / dst_len;
    if (mapped >= src_len)
        mapped = src_len - 1;
    return static_cast<int>(mapped);
}

} // namespace wm
