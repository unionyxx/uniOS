#include <kernel/arch/x86_64/serial.h>
#include <kernel/arch/x86_64/io.h>
#include <stdarg.h>

static uint16_t active_port = COM1_PORT;
static bool serial_initialized = false;

// Serial port register offsets
#define SERIAL_DATA         0   // Data register (R/W)
#define SERIAL_INT_ENABLE   1   // Interrupt enable register
#define SERIAL_FIFO_CTRL    2   // FIFO control register
#define SERIAL_LINE_CTRL    3   // Line control register
#define SERIAL_MODEM_CTRL   4   // Modem control register
#define SERIAL_LINE_STATUS  5   // Line status register
#define SERIAL_MODEM_STATUS 6   // Modem status register
#define SERIAL_SCRATCH      7   // Scratch register

// When DLAB=1, registers 0 and 1 become:
#define SERIAL_DIVISOR_LOW  0   // Divisor latch low byte
#define SERIAL_DIVISOR_HIGH 1   // Divisor latch high byte

void serial_init_port(uint16_t port, uint32_t baud) {
    active_port = port;
    
    // Calculate divisor for baud rate (115200 base clock / baud)
    uint16_t divisor = 115200 / baud;
    
    // Disable interrupts
    outb(port + SERIAL_INT_ENABLE, 0x00);
    
    // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_LINE_CTRL, 0x80);
    
    // Set divisor (lo byte)
    outb(port + SERIAL_DIVISOR_LOW, divisor & 0xFF);
    
    // Set divisor (hi byte)
    outb(port + SERIAL_DIVISOR_HIGH, (divisor >> 8) & 0xFF);
    
    // 8 bits, no parity, one stop bit (8N1)
    outb(port + SERIAL_LINE_CTRL, 0x03);
    
    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + SERIAL_FIFO_CTRL, 0xC7);
    
    // IRQs enabled, RTS/DSR set
    outb(port + SERIAL_MODEM_CTRL, 0x0B);
    
    // Test serial chip (set loopback mode)
    outb(port + SERIAL_MODEM_CTRL, 0x1E);
    
    // Send test byte
    outb(port + SERIAL_DATA, 0xAE);
    
    // Check if we receive the same byte back
    if (inb(port + SERIAL_DATA) != 0xAE) {
        // Serial port not working
        serial_initialized = false;
        return;
    }
    
    // Set to normal operation mode (not loopback)
    // IRQs enabled, OUT1 and OUT2 bits set
    outb(port + SERIAL_MODEM_CTRL, 0x0F);
    
    serial_initialized = true;
}

void serial_init() {
    serial_init_port(COM1_PORT, 115200);
}

bool serial_is_ready() {
    if (!serial_initialized) return false;
    return (inb(active_port + SERIAL_LINE_STATUS) & 0x20) != 0;
}

void serial_putc(char c) {
    if (!serial_initialized) return;
    
    // Wait for transmit buffer to be empty
    while (!serial_is_ready());
    
    outb(active_port + SERIAL_DATA, c);
}

void serial_puts(const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_putc('\r');  // CR before LF for proper terminal display
        }
        serial_putc(*str++);
    }
}

// Simple printf implementation
void serial_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char buf[256];
    int bi = 0;
    
    while (*fmt && bi < 255) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    if (!s) s = "(null)";
                    while (*s && bi < 255) buf[bi++] = *s++;
                    break;
                }
                case 'd':
                case 'i': {
                    int64_t n = va_arg(args, int);
                    if (n < 0) { buf[bi++] = '-'; n = -n; }
                    if (n == 0) { buf[bi++] = '0'; break; }
                    char tmp[20]; int ti = 0;
                    while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
                    while (ti > 0 && bi < 255) buf[bi++] = tmp[--ti];
                    break;
                }
                case 'u': {
                    uint64_t n = va_arg(args, unsigned int);
                    if (n == 0) { buf[bi++] = '0'; break; }
                    char tmp[20]; int ti = 0;
                    while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
                    while (ti > 0 && bi < 255) buf[bi++] = tmp[--ti];
                    break;
                }
                case 'x':
                case 'X': {
                    uint64_t n = va_arg(args, unsigned int);
                    const char* hex = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    if (n == 0) { buf[bi++] = '0'; break; }
                    char tmp[16]; int ti = 0;
                    while (n > 0) { tmp[ti++] = hex[n & 0xF]; n >>= 4; }
                    while (ti > 0 && bi < 255) buf[bi++] = tmp[--ti];
                    break;
                }
                case 'p': {
                    uint64_t n = va_arg(args, uint64_t);
                    buf[bi++] = '0'; buf[bi++] = 'x';
                    const char* hex = "0123456789abcdef";
                    char tmp[16]; int ti = 0;
                    if (n == 0) { buf[bi++] = '0'; break; }
                    while (n > 0) { tmp[ti++] = hex[n & 0xF]; n >>= 4; }
                    while (ti > 0 && bi < 255) buf[bi++] = tmp[--ti];
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    buf[bi++] = c;
                    break;
                }
                case '%':
                    buf[bi++] = '%';
                    break;
                default:
                    buf[bi++] = '%';
                    buf[bi++] = *fmt;
                    break;
            }
            fmt++;
        } else {
            buf[bi++] = *fmt++;
        }
    }
    buf[bi] = '\0';
    
    va_end(args);
    serial_puts(buf);
}
