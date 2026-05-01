#include <drivers/rtc/rtc.h>
#include <kernel/debug.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/time/timer.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <libk/kuser.h>

static char *kstrdup(const char *s)
{
    if (!s)
        return nullptr;
    size_t len = kstring::strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d)
        kstring::strncpy(d, s, len + 1);
    return d;
}

static int str_to_int(const char *s)
{
    if (!s)
        return 0;
    int result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

void kfree_passwd(passwd *pw)
{
    if (!pw)
        return;
    if (pw->pw_name)
        free(pw->pw_name);
    if (pw->pw_passwd)
        free(pw->pw_passwd);
    if (pw->pw_gecos)
        free(pw->pw_gecos);
    if (pw->pw_dir)
        free(pw->pw_dir);
    if (pw->pw_shell)
        free(pw->pw_shell);
    free(pw);
}

void kfree_spwd(spwd *sp)
{
    if (!sp)
        return;
    if (sp->sp_namp)
        free(sp->sp_namp);
    if (sp->sp_pwdp)
        free(sp->sp_pwdp);
    free(sp);
}

static char *read_file_to_buf(const char *path, uint64_t *out_size)
{
    VNodeStat st;
    if (vfs_stat(path, &st) < 0)
        return nullptr;

    char *buf = (char *)malloc(st.size + 1);
    if (!buf)
        return nullptr;

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return nullptr;
    }

    int64_t r = vfs_read(fd, buf, st.size);
    vfs_close(fd);

    if (r < 0) {
        free(buf);
        return nullptr;
    }

    buf[r] = '\0';
    if (out_size)
        *out_size = (uint64_t)r;
    return buf;
}

passwd *kgetpwnam(const char *name)
{
    if (!name)
        return nullptr;
    uint64_t size;
    char *buf = read_file_to_buf("/etc/passwd", &size);
    if (!buf)
        return nullptr;

    passwd *result = nullptr;
    char *line = buf;
    while (line && *line) {
        char *next_line = kstring::strchr(line, '\n');
        if (next_line)
            *next_line = '\0';

        char *p = line;
        char *u_end = kstring::strchr(p, ':');
        if (u_end) {
            *u_end = '\0';
            if (kstring::strcmp(p, name) == 0) {
                result = (passwd *)malloc(sizeof(passwd));
                if (result) {
                    kstring::memset(result, 0, sizeof(passwd));
                    result->pw_name = kstrdup(p);

                    p = u_end + 1; // password
                    char *pw_end = kstring::strchr(p, ':');
                    if (pw_end) {
                        *pw_end = '\0';
                        result->pw_passwd = kstrdup(p);
                        p = pw_end + 1; // uid

                        char *uid_end = kstring::strchr(p, ':');
                        if (uid_end) {
                            *uid_end = '\0';
                            result->pw_uid = (uint32_t)str_to_int(p);
                            p = uid_end + 1; // gid

                            char *gid_end = kstring::strchr(p, ':');
                            if (gid_end) {
                                *gid_end = '\0';
                                result->pw_gid = (uint32_t)str_to_int(p);
                                p = gid_end + 1; // gecos

                                char *gecos_end = kstring::strchr(p, ':');
                                if (gecos_end) {
                                    *gecos_end = '\0';
                                    result->pw_gecos = kstrdup(p);
                                    p = gecos_end + 1; // dir

                                    char *dir_end = kstring::strchr(p, ':');
                                    if (dir_end) {
                                        *dir_end = '\0';
                                        result->pw_dir = kstrdup(p);
                                        result->pw_shell = kstrdup(dir_end + 1);
                                    }
                                }
                            }
                        }
                    }
                }
                break;
            }
        }
        line = next_line ? next_line + 1 : nullptr;
    }

    free(buf);
    return result;
}

passwd *kgetpwuid(uint32_t uid)
{
    uint64_t size;
    char *buf = read_file_to_buf("/etc/passwd", &size);
    if (!buf)
        return nullptr;

    passwd *result = nullptr;
    char *line = buf;
    while (line && *line) {
        char *next_line = kstring::strchr(line, '\n');
        if (next_line)
            *next_line = '\0';

        char *p = line;
        char *u_end = kstring::strchr(p, ':');
        if (u_end) {
            *u_end = '\0';
            char *username = p;
            p = u_end + 1; // password
            char *pw_end = kstring::strchr(p, ':');
            if (pw_end) {
                *pw_end = '\0';
                char *password = p;
                p = pw_end + 1; // uid
                char *uid_end = kstring::strchr(p, ':');
                if (uid_end) {
                    *uid_end = '\0';
                    if ((uint32_t)str_to_int(p) == uid) {
                        result = (passwd *)malloc(sizeof(passwd));
                        if (result) {
                            kstring::memset(result, 0, sizeof(passwd));
                            result->pw_name = kstrdup(username);
                            result->pw_passwd = kstrdup(password);
                            result->pw_uid = uid;

                            p = uid_end + 1; // gid
                            char *gid_end = kstring::strchr(p, ':');
                            if (gid_end) {
                                *gid_end = '\0';
                                result->pw_gid = (uint32_t)str_to_int(p);
                                p = gid_end + 1; // gecos
                                char *gecos_end = kstring::strchr(p, ':');
                                if (gecos_end) {
                                    *gecos_end = '\0';
                                    result->pw_gecos = kstrdup(p);
                                    p = gecos_end + 1; // dir
                                    char *dir_end = kstring::strchr(p, ':');
                                    if (dir_end) {
                                        *dir_end = '\0';
                                        result->pw_dir = kstrdup(p);
                                        result->pw_shell = kstrdup(dir_end + 1);
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        line = next_line ? next_line + 1 : nullptr;
    }

    free(buf);
    return result;
}

spwd *kgetspnam(const char *name)
{
    if (!name)
        return nullptr;
    uint64_t size;
    char *buf = read_file_to_buf("/etc/shadow", &size);
    if (!buf)
        return nullptr;

    spwd *result = nullptr;
    char *line = buf;
    while (line && *line) {
        char *next_line = kstring::strchr(line, '\n');
        if (next_line)
            *next_line = '\0';

        char *p = line;
        char *u_end = kstring::strchr(p, ':');
        if (u_end) {
            *u_end = '\0';
            if (kstring::strcmp(p, name) == 0) {
                result = (spwd *)malloc(sizeof(spwd));
                if (result) {
                    kstring::memset(result, 0, sizeof(spwd));
                    result->sp_namp = kstrdup(p);

                    p = u_end + 1;
                    char *field3 = kstring::strchr(p, ':'); // end of salt
                    if (field3) {
                        char *field4 = kstring::strchr(field3 + 1, ':'); // end of hash
                        if (field4)
                            *field4 = '\0';
                        result->sp_pwdp = kstrdup(p);
                    } else {
                        kfree_spwd(result);
                        result = nullptr;
                    }
                }
                break;
            }
        }
        line = next_line ? next_line + 1 : nullptr;
    }

    free(buf);
    return result;
}

struct SHA256_CTX
{
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
};

#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[])
{
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[])
{
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        kstring::memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = ctx->bitlen;
    ctx->data[62] = ctx->bitlen >> 8;
    ctx->data[61] = ctx->bitlen >> 16;
    ctx->data[60] = ctx->bitlen >> 24;
    ctx->data[59] = ctx->bitlen >> 32;
    ctx->data[58] = ctx->bitlen >> 40;
    ctx->data[57] = ctx->bitlen >> 48;
    ctx->data[56] = ctx->bitlen >> 56;
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4] = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8] = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

void ksha256_string(const char *salt, const char *pass, char *out_hex)
{
    uint8_t hash[32];
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)salt, kstring::strlen(salt));
    sha256_update(&ctx, (const uint8_t *)pass, kstring::strlen(pass));
    sha256_final(&ctx, hash);
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[hash[i] >> 4];
        out_hex[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    out_hex[64] = '\0';
}

void kuser_generate_salt(char *out, int max_len)
{
    static uint32_t counter = 0;

    uint64_t t = timer_get_ticks();
    uint64_t u = rtc_get_uptime_seconds();
    uint64_t p = reinterpret_cast<uint64_t>(process_get_current());

    uint32_t seed = (uint32_t)(t ^ (t >> 32) ^ u ^ (p >> 12) ^ (counter++));

    char tmp[64];
    kstring::itoa(seed, tmp);
    kstring::strncpy(out, "salt", max_len - 1);
    kstring::strncat(out, tmp, max_len - 1);
}
int kuser_count()
{
    uint64_t size;
    char *buf = read_file_to_buf("/etc/passwd", &size);
    if (!buf)
        return 0;

    int count = 0;
    char *line = buf;
    while (line && *line) {
        char *next_line = kstring::strchr(line, '\n');
        if (kstring::strchr(line, ':'))
            count++;
        line = next_line ? next_line + 1 : nullptr;
    }

    free(buf);
    return count;
}

bool kuser_create(const char *username, const char *password, uint32_t uid)
{
    if (!username || !password)
        return false;

    // 1. Add to /etc/passwd
    char entry[256];
    char uid_str[16];
    kstring::itoa(uid, uid_str);

    entry[0] = '\0';
    kstring::strncat(entry, username, 255);
    kstring::strncat(entry, ":x:", 255);
    kstring::strncat(entry, uid_str, 255);
    kstring::strncat(entry, ":", 255);
    kstring::strncat(entry, uid_str, 255);
    kstring::strncat(entry, "::/home/", 255);
    kstring::strncat(entry, username, 255);
    kstring::strncat(entry, ":/bin/sh\n", 255);
    int len = kstring::strlen(entry);

    int fd = vfs_open("/etc/passwd", O_WRONLY | O_APPEND | O_CREAT);
    if (fd >= 0) {
        vfs_write(fd, entry, len);
        vfs_close(fd);
    }

    // 2. Add to /etc/shadow
    char salt[16];
    kuser_generate_salt(salt, 16);
    char hash[65];
    ksha256_string(salt, password, hash);
    kstring::memset((void *)password, 0, kstring::strlen(password));

    char shadow_entry[256];
    shadow_entry[0] = '\0';
    kstring::strncat(shadow_entry, username, 255);
    kstring::strncat(shadow_entry, ":", 255);
    kstring::strncat(shadow_entry, salt, 255);
    kstring::strncat(shadow_entry, ":", 255);
    kstring::strncat(shadow_entry, hash, 255);
    kstring::strncat(shadow_entry, ":::::\n", 255);
    int slen = kstring::strlen(shadow_entry);

    fd = vfs_open("/etc/shadow", O_WRONLY | O_APPEND | O_CREAT);
    if (fd >= 0) {
        vfs_write(fd, shadow_entry, slen);
        vfs_close(fd);
    }

    // 3. Create home dir
    char home[256] = "/home/";
    kstring::strncat(home, username, 255);
    vfs_mkdir(home);

    return true;
}

bool kuser_authenticate(const char *username, const char *password)
{
    if (!username || !password)
        return false;
    spwd *sp = kgetspnam(username);
    if (!sp)
        return false;

    char *salt = sp->sp_pwdp;
    char *hash = kstring::strchr(salt, ':');
    if (!hash) {
        kfree_spwd(sp);
        return false;
    }
    *hash = '\0';
    hash++;

    char hex[65];
    ksha256_string(salt, password, hex);
    bool match = (kstring::strcmp(hex, hash) == 0);

    kfree_spwd(sp);
    return match;
}

static char *kread_file_to_buf(const char *path, uint64_t *out_size)
{
    VNodeStat st;
    if (vfs_stat(path, &st) < 0)
        return nullptr;
    char *buf = (char *)malloc(st.size + 1);
    if (!buf)
        return nullptr;
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return nullptr;
    }
    int64_t r = vfs_read(fd, buf, st.size);
    vfs_close(fd);
    if (r < 0) {
        free(buf);
        return nullptr;
    }
    buf[r] = '\0';
    if (out_size)
        *out_size = (uint64_t)r;
    return buf;
}

static void ksync_file(const char *src, const char *dst, bool *success)
{
    uint64_t size;
    char *buf = kread_file_to_buf(src, &size);
    if (!buf) {
        if (success)
            *success = false;
        return;
    }
    int fd = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        if (vfs_write(fd, buf, size) < 0) {
            if (success)
                *success = false;
        }
        vfs_close(fd);
    } else {
        if (success)
            *success = false;
    }
    free(buf);
}

bool kuser_sync_to_disk()
{
    VNodeStat st;
    bool success = true;
    if (vfs_stat("/data", &st) < 0)
        return false;
    if (vfs_stat("/data/etc", &st) < 0)
        vfs_mkdir("/data/etc");
    ksync_file("/etc/passwd", "/data/etc/passwd", &success);
    ksync_file("/etc/shadow", "/data/etc/shadow", &success);
    return success;
}

void kuser_restore_from_disk()
{
    VNodeStat st;
    if (vfs_stat("/data/etc/passwd", &st) == 0 && vfs_stat("/data/etc/shadow", &st) == 0) {
        ksync_file("/data/etc/passwd", "/etc/passwd", nullptr);
        ksync_file("/data/etc/shadow", "/etc/shadow", nullptr);
    }
}
