#include <kernel/mm/bitmap.h>

void Bitmap::init(void *buffer, size_t size_in_bits)
{
    m_buffer = static_cast<uint8_t *>(buffer);
    m_size = size_in_bits;

    size_t size_in_bytes = (size_in_bits + 7) >> 3;
    __builtin_memset(m_buffer, 0, size_in_bytes);
}

bool Bitmap::operator[](size_t index) const
{
    if (index >= m_size)
        return false;
    return (m_buffer[index >> 3] & (1 << (index & 7))) != 0;
}

void Bitmap::set(size_t index, bool value)
{
    if (index >= m_size)
        return;
    if (value) {
        m_buffer[index >> 3] |= static_cast<uint8_t>(1 << (index & 7));
    } else {
        m_buffer[index >> 3] &= static_cast<uint8_t>(~(1 << (index & 7)));
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
    while (i < end && (i & 7)) {
        if (value)
            m_buffer[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
        else
            m_buffer[i >> 3] &= static_cast<uint8_t>(~(1 << (i & 7)));
        i++;
    }

    size_t bytes_to_fill = (end - i) >> 3;
    if (bytes_to_fill > 0) {
        __builtin_memset(m_buffer + (i >> 3), value ? 0xFF : 0x00, bytes_to_fill);
        i += bytes_to_fill << 3;
    }

    while (i < end) {
        if (value)
            m_buffer[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
        else
            m_buffer[i >> 3] &= static_cast<uint8_t>(~(1 << (i & 7)));
        i++;
    }
}

size_t Bitmap::find_first_free(size_t start, size_t end) const
{
    if (m_size == 0)
        return static_cast<size_t>(-1);
    if (end > m_size)
        end = m_size;
    if (start >= end)
        return static_cast<size_t>(-1);

    size_t i = start;
    while (i < end && (i & 63)) {
        if (!(*this)[i])
            return i;
        i++;
    }

    const uint64_t *qwords = reinterpret_cast<const uint64_t *>(m_buffer);
    size_t qword_end = end & ~static_cast<size_t>(63);

    while (i < qword_end) {
        uint64_t qw = qwords[i >> 6];
        if (qw != ~0ULL)
            return i + __builtin_ctzll(~qw);
        i += 64;
    }

    while (i < end) {
        if (!(*this)[i])
            return i;
        i++;
    }

    return static_cast<size_t>(-1);
}

size_t Bitmap::find_first_free_sequence(size_t count, size_t start, size_t end) const
{
    if (count == 0 || count > m_size)
        return static_cast<size_t>(-1);
    if (end > m_size)
        end = m_size;
    if (start >= end || count > end - start)
        return static_cast<size_t>(-1);

    size_t current_run = 0;
    size_t run_start = static_cast<size_t>(-1);

    // Leading partial qword.
    size_t i = start;
    size_t qword_start = (i + 63) & ~static_cast<size_t>(63);
    if (qword_start > end)
        qword_start = end;

    while (i < qword_start) {
        if (!(*this)[i]) {
            if (current_run == 0)
                run_start = i;
            if (++current_run == count)
                return run_start;
        } else {
            current_run = 0;
        }
        i++;
    }

    const uint64_t *qwords = reinterpret_cast<const uint64_t *>(m_buffer);
    size_t qword_end = end & ~static_cast<size_t>(63);

    while (i < qword_end) {
        size_t q_idx = i >> 6;
        uint64_t qw = qwords[q_idx];

        if (qw == 0) {
            if (current_run == 0)
                run_start = i;
            current_run += 64;
            if (current_run >= count)
                return run_start;
            i += 64;
        } else if (qw == ~0ULL) {
            current_run = 0;
            i += 64;
        } else {
            for (int bit = 0; bit < 64; bit++) {
                if (!(qw & (1ULL << bit))) {
                    if (current_run == 0)
                        run_start = i + bit;
                    if (++current_run == count)
                        return run_start;
                } else {
                    current_run = 0;
                }
            }
            i += 64;
        }
    }

    // Trailing partial qword.
    while (i < end) {
        if (!(*this)[i]) {
            if (current_run == 0)
                run_start = i;
            if (++current_run == count)
                return run_start;
        } else {
            current_run = 0;
        }
        i++;
    }

    return static_cast<size_t>(-1);
}

size_t Bitmap::find_last_free_sequence(size_t count, size_t start, size_t end) const
{
    if (count == 0 || count > m_size)
        return static_cast<size_t>(-1);
    if (end > m_size)
        end = m_size;
    if (start >= end || count > end - start)
        return static_cast<size_t>(-1);

    size_t current_run = 0;
    size_t i = end;

    // Trailing partial qword: bits [qword_end, end), high to low.
    size_t qword_end = end & ~static_cast<size_t>(63);
    while (i > qword_end && i > start) {
        i--;
        if (!(*this)[i]) {
            if (++current_run == count)
                return i;
        } else {
            current_run = 0;
        }
    }

    // Full qwords, high to low, stopping above the qword holding start.
    size_t low_bound = (start + 63) & ~static_cast<size_t>(63);
    if (i > low_bound) {
        const uint64_t *qwords = reinterpret_cast<const uint64_t *>(m_buffer);
        do {
            size_t q_idx = (i >> 6) - 1;
            uint64_t qw = qwords[q_idx];

            if (qw == 0) {
                // The run extends below this qword; the highest `count`-bit
                // window of it ends at the run's top edge, i.e. i + (run - 64).
                current_run += 64;
                if (current_run >= count)
                    return (q_idx << 6) + (current_run - count);
            } else if (qw == ~0ULL) {
                current_run = 0;
            } else {
                for (int bit = 63; bit >= 0; bit--) {
                    if (!(qw & (1ULL << bit))) {
                        if (++current_run == count)
                            return (q_idx << 6) + bit;
                    } else {
                        current_run = 0;
                    }
                }
            }
            i -= 64;
        } while (i > low_bound);
    }

    // Leading partial qword: bits [start, low_bound), high to low.
    while (i > start) {
        i--;
        if (!(*this)[i]) {
            if (++current_run == count)
                return i;
        } else {
            current_run = 0;
        }
    }

    return static_cast<size_t>(-1);
}
