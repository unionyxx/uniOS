#include "pipe.h"
#include <stddef.h>

static Pipe pipes[MAX_PIPES];
static bool pipes_initialized = false;

void pipe_init() {
    if (pipes_initialized) return;
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].in_use = false;
    }
    pipes_initialized = true;
}

int pipe_create() {
    pipe_init();
    
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) {
            pipes[i].in_use = true;
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].count = 0;
            pipes[i].write_closed = false;
            pipes[i].read_closed = false;
            return i;
        }
    }
    return -1;
}

int64_t pipe_read(int pipe_id, char* buf, uint64_t count) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].in_use) {
        return -1;
    }
    
    Pipe* p = &pipes[pipe_id];
    
    // If no data and write end closed, return 0 (EOF)
    if (p->count == 0 && p->write_closed) {
        return 0;
    }
    
    // Read available data
    uint64_t to_read = (count < p->count) ? count : p->count;
    
    for (uint64_t i = 0; i < to_read; i++) {
        buf[i] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUFFER_SIZE;
    }
    p->count -= to_read;
    
    return to_read;
}

int64_t pipe_write(int pipe_id, const char* buf, uint64_t count) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].in_use) {
        return -1;
    }
    
    Pipe* p = &pipes[pipe_id];
    
    // If read end closed, return error
    if (p->read_closed) {
        return -1;
    }
    
    // Write as much as possible
    uint64_t space = PIPE_BUFFER_SIZE - p->count;
    uint64_t to_write = (count < space) ? count : space;
    
    for (uint64_t i = 0; i < to_write; i++) {
        p->buffer[p->write_pos] = buf[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUFFER_SIZE;
    }
    p->count += to_write;
    
    return to_write;
}

void pipe_close_read(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES) return;
    pipes[pipe_id].read_closed = true;
    
    // If both ends closed, free the pipe
    if (pipes[pipe_id].write_closed) {
        pipes[pipe_id].in_use = false;
    }
}

void pipe_close_write(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES) return;
    pipes[pipe_id].write_closed = true;
    
    // If both ends closed, free the pipe
    if (pipes[pipe_id].read_closed) {
        pipes[pipe_id].in_use = false;
    }
}
