#pragma once
#include <stdint.h>
#include <stddef.h>

class Bitmap {
public:
    void init(void* buffer, size_t size_in_bits);
    bool operator[](size_t index) const;
    void set(size_t index, bool value);
    void set_range(size_t start, size_t count, bool value);
    size_t find_first_free(size_t start_index = 0) const;
    size_t find_first_free_sequence(size_t count, size_t start_index = 0) const;
    
    size_t get_size() const { return m_size; }
    void* get_buffer() const { return m_buffer; }

private:
    uint8_t* m_buffer;
    size_t m_size; // in bits
    mutable size_t m_next_free_hint;  // Optimization: start search from here
    
public:
    void update_hint(size_t freed_index) {
        if (freed_index < m_next_free_hint) m_next_free_hint = freed_index;
    }
    void reset_hint() { m_next_free_hint = 0; }
};
