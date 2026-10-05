#pragma once
#include <stddef.h>
#include <stdint.h>

// Flat frame bitmap. Every scan is a pure range query over [start, end):
// there is no hidden cursor state. The PMM owns the per-zone allocation
// cursors and derives the scan bounds from its zone descriptors.
class Bitmap
{
public:
    void init(void *buffer, size_t size_in_bits);

    [[nodiscard]] bool operator[](size_t index) const;
    void set(size_t index, bool value);
    void set_range(size_t start, size_t count, bool value);

    // First free bit in [start, end), or SIZE_MAX.
    [[nodiscard]] size_t find_first_free(size_t start, size_t end) const;
    // Lowest run of `count` free bits fully inside [start, end), or SIZE_MAX.
    [[nodiscard]] size_t find_first_free_sequence(size_t count, size_t start, size_t end) const;
    // Highest run of `count` free bits fully inside [start, end), or SIZE_MAX.
    [[nodiscard]] size_t find_last_free_sequence(size_t count, size_t start, size_t end) const;

private:
    uint8_t *m_buffer = nullptr;
    size_t m_size = 0;
};
