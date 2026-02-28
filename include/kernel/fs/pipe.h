#pragma once
#include <stdint.h>

#define PIPE_BUFFER_SIZE 4096
#define MAX_PIPES 8

struct Pipe {
    bool in_use;
    uint8_t buffer[PIPE_BUFFER_SIZE];
    uint64_t read_pos;
    uint64_t write_pos;
    uint64_t count;      // Bytes in buffer
    bool write_closed;
    bool read_closed;
};

struct VNode;

void pipe_init();
int pipe_create();  // Returns pipe ID, or -1 on error
struct VNode* pipe_get_vnode(int pipe_id, bool is_write);
int64_t pipe_read(int pipe_id, char* buf, uint64_t count);
int64_t pipe_write(int pipe_id, const char* buf, uint64_t count);
void pipe_close_read(int pipe_id);
void pipe_close_write(int pipe_id);
