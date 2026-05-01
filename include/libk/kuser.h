#pragma once
#include <stdint.h>

struct passwd
{
    char *pw_name;   // Username
    char *pw_passwd; // Shadow password marker.
    uint32_t pw_uid; // User ID
    uint32_t pw_gid; // Group ID (unused for now)
    char *pw_gecos;  // Real name / Info
    char *pw_dir;    // Home directory
    char *pw_shell;  // Shell program
};

struct spwd
{
    char *sp_namp;         // Username
    char *sp_pwdp;         // Salted hash (e.g. salt:hash)
    long sp_lstchg;        // Last change
    long sp_min;           // Min age
    long sp_max;           // Max age
    long sp_warn;          // Warning period
    long sp_inact;         // Inactivity period
    long sp_expire;        // Expiration date
    unsigned long sp_flag; // Reserved
};

// POSIX-like API for kernel/shell use
passwd *kgetpwnam(const char *name);
passwd *kgetpwuid(uint32_t uid);
spwd *kgetspnam(const char *name);

// Kernel-specific user management
int kuser_count();
bool kuser_create(const char *username, const char *password, uint32_t uid);
bool kuser_authenticate(const char *username, const char *password);
void kuser_generate_salt(char *out, int max_len);
void ksha256_string(const char *salt, const char *pass, char *out_hex);
bool kuser_sync_to_disk();
void kuser_restore_from_disk();

// Memory cleanup
void kfree_passwd(passwd *pw);
void kfree_spwd(spwd *sp);
