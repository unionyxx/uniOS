#include <kernel/mm/bitmap.h>
#include <kernel/debug.h>

void Bitmap::init(void* buffer, size_t size_in_bits) {
    m_buffer = static_cast<uint8_t*>(buffer);
    m_size = size_in_bits;
    m_next_free_hint = 0;
    
    size_t size_in_bytes = (size_in_bits + 7) / 8;
    for (size_t i = 0; i < size_in_bytes; i++) {
        m_buffer[i] = 0;
    }
}

bool Bitmap::operator[](size_t index) const {
    if (index >= m_size) return false;
    return (m_buffer[index / 8] & (1 << (index % 8))) != 0;
}

void Bitmap::set(size_t index, bool value) {
    if (index >= m_size) return;
    if (value) {
        m_buffer[index / 8] |= (1 << (index % 8));
    } else {
        m_buffer[index / 8] &= ~(1 << (index % 8));
    }
}

void Bitmap::set_range(size_t start, size_t count, bool value) {
    for (size_t i = 0; i < count; i++) {
        set(start + i, value);
    }
}

size_t Bitmap::find_first_free(size_t start_index) const {
    size_t search_start = (start_index == 0) ? m_next_free_hint : start_index;
    
    for (size_t i = search_start; i < m_size; i++) {
        if (!(*this)[i]) {
            m_next_free_hint = i + 1;
            return i;
        }
    }
    
    if (search_start > 0) {
        for (size_t i = 0; i < search_start; i++) {
            if (!(*this)[i]) {
                m_next_free_hint = i + 1;
                return i;
            }
        }
    }
    
    return static_cast<size_t>(-1);
}

size_t Bitmap::find_first_free_sequence(size_t count, size_t start_index) const {
    if (count == 0) return static_cast<size_t>(-1);
    
    size_t current_run = 0;
    size_t run_start = static_cast<size_t>(-1);
    
    for (size_t i = start_index; i < m_size; i++) {
        if (!(*this)[i]) {
            if (current_run == 0) run_start = i;
            current_run++;
            if (current_run >= count) return run_start;
        } else {
            current_run = 0;
            run_start = static_cast<size_t>(-1);
        }
    }
    return static_cast<size_t>(-1);
}
