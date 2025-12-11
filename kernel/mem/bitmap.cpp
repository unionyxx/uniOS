#include "bitmap.h"
#include "debug.h"

void Bitmap::init(void* buffer, size_t size_in_bits) {
    m_buffer = (uint8_t*)buffer;
    m_size = size_in_bits;
    
    // Clear bitmap initially
    size_t size_in_bytes = (size_in_bits + 7) / 8;
    for (size_t i = 0; i < size_in_bytes; i++) {
        m_buffer[i] = 0;
    }
}

bool Bitmap::operator[](size_t index) const {
    if (index >= m_size) return false;
    size_t byte_index = index / 8;
    size_t bit_index = index % 8;
    return (m_buffer[byte_index] & (1 << bit_index)) != 0;
}

void Bitmap::set(size_t index, bool value) {
    if (index >= m_size) return;
    size_t byte_index = index / 8;
    size_t bit_index = index % 8;
    if (value) {
        m_buffer[byte_index] |= (1 << bit_index);
    } else {
        m_buffer[byte_index] &= ~(1 << bit_index);
    }
}

void Bitmap::set_range(size_t start, size_t count, bool value) {
    for (size_t i = 0; i < count; i++) {
        set(start + i, value);
    }
}

size_t Bitmap::find_first_free(size_t start_index) const {
    for (size_t i = start_index; i < m_size; i++) {
        if (!(*this)[i]) {
            return i;
        }
    }
    return (size_t)-1;
}

size_t Bitmap::find_first_free_sequence(size_t count, size_t start_index) const {
    if (count == 0) return (size_t)-1;
    
    size_t current_run = 0;
    size_t run_start = (size_t)-1;
    
    for (size_t i = start_index; i < m_size; i++) {
        if (!(*this)[i]) {
            if (current_run == 0) run_start = i;
            current_run++;
            if (current_run >= count) return run_start;
        } else {
            current_run = 0;
            run_start = (size_t)-1;
        }
    }
    return (size_t)-1;
}
