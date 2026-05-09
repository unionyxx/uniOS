#include <kernel/debug.h>
#include <kernel/mm/bitmap.h>

void Bitmap::init(void *buffer, size_t size_in_bits)
{
    m_buffer = static_cast<uint8_t *>(buffer);
    m_size = size_in_bits;
    m_next_free_hint = 0;

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
        while (i < end && (i & 63)) {
            if (!(*this)[i])
                return i;
            i++;
        }

        const uint64_t *qwords = reinterpret_cast<const uint64_t *>(m_buffer);
        size_t qword_end = end & ~static_cast<size_t>(63);

        while (i < qword_end) {
            uint64_t qw = qwords[i >> 6];
            if (qw != ~0ULL) {
                return i + __builtin_ctzll(~qw);
            }
            i += 64;
        }

        while (i < end) {
            if (!(*this)[i])
                return i;
            i++;
        }

        return static_cast<size_t>(-1);
    };

    size_t index = scan_range(search_start, m_size);
    if (index == static_cast<size_t>(-1) && search_start != 0) {
        index = scan_range(0, search_start);
    }

    if (index != static_cast<size_t>(-1)) {
        m_next_free_hint = (index + 1 < m_size) ? (index + 1) : 0;
    }

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

    size_t i = start_index;
    size_t qword_start = (i + 63) & ~static_cast<size_t>(63);
    if (qword_start > m_size)
        qword_start = m_size;

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
    size_t qword_end = m_size & ~static_cast<size_t>(63);

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

    while (i < m_size) {
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

size_t Bitmap::find_last_free_sequence(size_t count) const
{
    if (count == 0 || count > m_size)
        return static_cast<size_t>(-1);

    size_t current_run = 0;
    size_t i = m_size;

    size_t qword_end = m_size & ~static_cast<size_t>(63);
    while (i > qword_end) {
        i--;
        if (!(*this)[i]) {
            if (++current_run == count)
                return i;
        } else {
            current_run = 0;
        }
    }

    if (i > 0) {
        const uint64_t *qwords = reinterpret_cast<const uint64_t *>(m_buffer);
        size_t q_idx = i >> 6;
        while (q_idx > 0) {
            q_idx--;
            uint64_t qw = qwords[q_idx];

            if (qw == 0) {
                current_run += 64;
                if (current_run >= count) {
                    return (q_idx << 6) + (current_run - count);
                }
            } else if (qw == ~0ULL) {
                current_run = 0;
            } else {
                for (int bit = 63; bit >= 0; bit--) {
                    if (!(qw & (1ULL << bit))) {
                        if (++current_run == count) {
                            return (q_idx << 6) + bit;
                        }
                    } else {
                        current_run = 0;
                    }
                }
            }
        }
    }

    return static_cast<size_t>(-1);
}