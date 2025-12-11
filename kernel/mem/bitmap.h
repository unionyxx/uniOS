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
};
