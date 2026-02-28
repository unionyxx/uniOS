#pragma once
#include <stdint.h>
#include <stddef.h>

class Bitmap {
public:
    void init(void* buffer, size_t size_in_bits);
    
    [[nodiscard]] bool operator[](size_t index) const;
    void set(size_t index, bool value);
    void set_range(size_t start, size_t count, bool value);
    
    [[nodiscard]] size_t find_first_free(size_t start_index = 0) const;
    [[nodiscard]] size_t find_first_free_sequence(size_t count, size_t start_index = 0) const;
    
    [[nodiscard]] size_t get_size() const { return m_size; }
    [[nodiscard]] void* get_buffer() const { return m_buffer; }

    void update_hint(size_t freed_index) {
        if (freed_index < m_next_free_hint) m_next_free_hint = freed_index;
    }
    void reset_hint() { m_next_free_hint = 0; }

private:
    uint8_t* m_buffer = nullptr;
    size_t m_size = 0;
    mutable size_t m_next_free_hint = 0;
};
