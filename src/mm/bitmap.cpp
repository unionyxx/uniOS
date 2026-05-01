#include <kernel/debug.h>
#include <kernel/mm/bitmap.h>

void Bitmap::init(void *buffer, size_t size_in_bits)
{
    m_buffer = static_cast<uint8_t *>(buffer);
    m_size = size_in_bits;
    m_next_free_hint = 0;

    size_t size_in_bytes = (size_in_bits + 7) / 8;
    for (size_t i = 0; i < size_in_bytes; i++) {
        m_buffer[i] = 0;
    }
}

bool Bitmap::operator[](size_t index) const
{
    if (index >= m_size)
        return false;
    return (m_buffer[index / 8] & (1 << (index % 8))) != 0;
}

void Bitmap::set(size_t index, bool value)
{
    if (index >= m_size)
        return;
    if (value) {
        m_buffer[index / 8] |= (uint8_t)(1 << (index % 8));
    } else {
        m_buffer[index / 8] &= (uint8_t) ~(1 << (index % 8));
    }
}

void Bitmap::set_range(size_t start, size_t count, bool value)
{
    if (start >= m_size || count == 0)
        return;

    size_t end = start + count;
    if (end < start || end > m_size)
        end = m_size;

    size_t i = start;
    while (i < end && (i & 7U) != 0) {
        set(i++, value);
    }

    const uint8_t fill = value ? 0xFF : 0x00;
    while (i + 8 <= end) {
        m_buffer[i / 8] = fill;
        i += 8;
    }

    while (i < end) {
        set(i++, value);
    }
}

size_t Bitmap::get_hint() const
{
    return m_next_free_hint;
}

size_t Bitmap::find_first_free(size_t start_index) const
{
    if (m_size == 0)
        return static_cast<size_t>(-1);

    size_t search_start = (start_index == 0) ? m_next_free_hint : start_index;
    if (search_start >= m_size)
        search_start = 0;

    auto scan_range = [&](size_t begin, size_t end) -> size_t {
        if (begin >= end)
            return static_cast<size_t>(-1);

        size_t i = begin;
        while (i < end && (i & 7U) != 0) {
            if (!(*this)[i])
                return i;
            ++i;
        }

        const size_t full_byte_end = end & ~static_cast<size_t>(7);
        while (i < full_byte_end) {
            const uint8_t byte = m_buffer[i / 8];
            if (byte != 0xFF) {
                const uint8_t free_bits = static_cast<uint8_t>(~byte);
                const unsigned bit = __builtin_ctz(static_cast<unsigned>(free_bits));
                const size_t index = i + bit;
                if (index < end)
                    return index;
            }
            i += 8;
        }

        while (i < end) {
            if (!(*this)[i])
                return i;
            ++i;
        }

        return static_cast<size_t>(-1);
    };

    size_t index = scan_range(search_start, m_size);
    if (index == static_cast<size_t>(-1) && search_start != 0)
        index = scan_range(0, search_start);

    if (index != static_cast<size_t>(-1))
        m_next_free_hint = (index + 1 < m_size) ? (index + 1) : 0;

    return index;
}

size_t Bitmap::find_first_free_sequence(size_t count, size_t start_index) const
{
    if (count == 0 || count > m_size)
        return static_cast<size_t>(-1);
    if (start_index >= m_size)
        start_index = 0;

    size_t current_run = 0;
    size_t run_start = static_cast<size_t>(-1);

    // 1. Align to 8-bit boundary for fast hopping
    size_t i = start_index;
    while (i < m_size && (i % 8) != 0) {
        if (!(*this)[i]) {
            if (current_run == 0)
                run_start = i;
            current_run++;
            if (current_run >= count)
                return run_start;
        } else {
            current_run = 0;
            run_start = static_cast<size_t>(-1);
        }
        i++;
    }

    // 2. Fast byte-level scanning
    size_t b = i / 8;
    size_t bytes_total = m_size / 8;
    while (b < bytes_total) {
        if (m_buffer[b] == 0xFF) {
            // Entire byte allocated, reset run
            current_run = 0;
            run_start = static_cast<size_t>(-1);
            b++;
        } else {
            // Found a byte with space, check bit-by-bit
            for (int bit = 0; bit < 8; bit++) {
                size_t index = b * 8 + bit;
                if (index >= m_size)
                    break;
                if (!(*this)[index]) {
                    if (current_run == 0)
                        run_start = index;
                    current_run++;
                    if (current_run >= count)
                        return run_start;
                } else {
                    current_run = 0;
                    run_start = static_cast<size_t>(-1);
                }
            }
            b++;
        }
    }

    // 3. Remainder
    for (size_t j = (bytes_total * 8); j < m_size; j++) {
        if (j < i)
            continue; // Already scanned in step 1 if bytes_total was small
        if (!(*this)[j]) {
            if (current_run == 0)
                run_start = j;
            current_run++;
            if (current_run >= count)
                return run_start;
        } else {
            current_run = 0;
            run_start = static_cast<size_t>(-1);
        }
    }

    return static_cast<size_t>(-1);
}

size_t Bitmap::find_last_free_sequence(size_t count) const
{
    if (count == 0 || count > m_size)
        return static_cast<size_t>(-1);

    size_t current_run = 0;

    // 1. Handle remainder bits at the end
    size_t i = m_size;
    while (i > 0 && (i % 8) != 0) {
        size_t idx = i - 1;
        if (!(*this)[idx]) {
            current_run++;
            if (current_run >= count)
                return idx;
        } else {
            current_run = 0;
        }
        i--;
    }

    // 2. Fast byte-level scanning backwards
    if (i > 0) {
        size_t b = i / 8;
        while (b > 0) {
            b--;
            if (m_buffer[b] == 0xFF) {
                // Entire byte allocated, reset
                current_run = 0;
            } else if (m_buffer[b] == 0x00) {
                // Entire byte free, possibly fast-forward
                current_run += 8;
                if (current_run >= count) {
                    // We found enough 0s. The start is somewhere in this byte or earlier.
                    // To be safe and simple, just fall back to bit-scan from the end of this run.
                    // But wait, we need the LOWEST index.
                    // Let's just continue bit-by-bit within THIS byte to be precise.
                    current_run -= 8; // Backtrack to check bits in this byte correctly
                    for (int bit = 7; bit >= 0; bit--) {
                        size_t idx = b * 8 + bit;
                        if (!(*this)[idx]) {
                            current_run++;
                            if (current_run >= count)
                                return idx;
                        } else {
                            current_run = 0;
                        }
                    }
                }
            } else {
                // Mixed byte, check bits
                for (int bit = 7; bit >= 0; bit--) {
                    size_t idx = b * 8 + bit;
                    if (!(*this)[idx]) {
                        current_run++;
                        if (current_run >= count)
                            return idx;
                    } else {
                        current_run = 0;
                    }
                }
            }
        }
    }

    return static_cast<size_t>(-1);
}
