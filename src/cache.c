/**
 * @file cache.c
 * @brief Hardware-Agnostic File-Backed Caching Module — Implementation
 *
 * On-disk layout (single file per key)
 * -------------------------------------
 *   [ cache_file_header_t ]  — fixed-size binary header
 *   [ payload bytes       ]  — raw data supplied by the caller
 *
 * File naming
 * -----------
 *   <root_path>/<hex(fnv1a32(key))>.cache
 *
 * The FNV-1a 32-bit hash is fast, has no external dependencies, and
 * produces a fixed-length hex name that is valid on every FS.
 * Collisions are theoretically possible; for a generic cache module this
 * is an acceptable trade-off.  If required, the header stores the original
 * key so a collision can be detected and treated as CACHE_ERR_NOT_FOUND.
 */

#include "cache.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER_MAGIC 0xCA5ECAC4UL /* "CACHE" mangled into 32 bits  */
#define CACHE_FILE_EXT ".cache"
#define MAX_KEY_LEN 128 /* truncated in the header        */
#define MAX_PATH_LEN 256

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        /**< HEADER_MAGIC                    */
    uint32_t crc32;        /**< CRC-32 of everything after this */
    uint64_t created_at;   /**< Unix timestamp of write         */
    uint64_t expires_at;   /**< Unix timestamp; 0 = no expiry   */
    uint32_t payload_len;  /**< Bytes of payload following hdr  */
    char key[MAX_KEY_LEN]; /**< NUL-terminated original key     */
} cache_file_header_t;
#pragma pack(pop)

typedef struct {
    cache_config_t cfg;
    bool initialised;
} cache_state_t;

static cache_state_t s_state = {.initialised = false};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return ~crc;
}

static uint32_t fnv1a32(const char *str) {
    uint32_t hash = 0x811C9DC5UL;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 0x01000193UL;
    }
    return hash;
}

/** Build the full file path for a given key. */
static void build_path(const char *key, char *out, size_t out_sz) {
    uint32_t h = fnv1a32(key);
    snprintf(out, out_sz, "%s/%08x%s", s_state.cfg.root_path, h,
             CACHE_FILE_EXT);
}

/** Return current time as a Unix timestamp.
 *  time() is available on all POSIX targets and most RTOSes via <time.h>.
 *  On bare-metal targets without RTC, consider patching this to return a
 *  monotonic tick counter — expiry will still work relatively. */
static uint64_t now_sec(void) { return (uint64_t)time(NULL); }

/** Allocate memory using the configured allocator. */
static void *cache_malloc(size_t sz) {
    if (s_state.cfg.alloc.malloc_fn)
        return s_state.cfg.alloc.malloc_fn(sz);
    return malloc(sz);
}

/** Free memory using the configured allocator. */
static void cache_free_internal(void *ptr) {
    if (!ptr)
        return;
    if (s_state.cfg.alloc.free_fn)
        s_state.cfg.alloc.free_fn(ptr);
    else
        free(ptr);
}

int cache_init(const cache_config_t *config) {
    if (!config || !config->io || !config->root_path)
        return CACHE_ERR_PARAM;

    if (s_state.initialised)
        return CACHE_OK; /* idempotent */

    s_state.cfg = *config;
    s_state.initialised = true;
    return CACHE_OK;
}

void cache_deinit(void) {
    s_state.initialised = false;
    memset(&s_state.cfg, 0, sizeof(s_state.cfg));
}

int cache_put(const char *key, const void *data, size_t len, uint32_t ttl_sec) {
    if (!s_state.initialised)
        return CACHE_ERR_INIT;
    if (!key || (!data && len > 0))
        return CACHE_ERR_PARAM;

    const cache_io_t *io = s_state.cfg.io;

    cache_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = HEADER_MAGIC;
    hdr.created_at = now_sec();
    hdr.payload_len = (uint32_t)len;
    strncpy(hdr.key, key, MAX_KEY_LEN - 1);

    if (ttl_sec == CACHE_TTL_INFINITE &&
        s_state.cfg.default_ttl_sec != CACHE_TTL_INFINITE)
        ttl_sec = s_state.cfg.default_ttl_sec;

    hdr.expires_at =
        (ttl_sec == CACHE_TTL_INFINITE) ? 0 : hdr.created_at + ttl_sec;

    /* CRC covers everything after the magic+crc fields */
    uint32_t crc = 0;
    const uint8_t *hdr_body =
        (const uint8_t *)&hdr + sizeof(hdr.magic) + sizeof(hdr.crc32);
    size_t hdr_body_len = sizeof(hdr) - sizeof(hdr.magic) - sizeof(hdr.crc32);
    crc = crc32_update(crc, hdr_body, hdr_body_len);
    if (len > 0)
        crc = crc32_update(crc, (const uint8_t *)data, len);
    hdr.crc32 = crc;

    size_t total = sizeof(hdr) + len;
    uint8_t *buf = (uint8_t *)cache_malloc(total);
    if (!buf)
        return CACHE_ERR_NOMEM;

    memcpy(buf, &hdr, sizeof(hdr));
    if (len > 0)
        memcpy(buf + sizeof(hdr), data, len);

    char path[MAX_PATH_LEN];
    build_path(key, path, sizeof(path));

    int rc = io->write(path, buf, total);
    cache_free_internal(buf);

    return (rc == 0) ? CACHE_OK : CACHE_ERR_IO;
}

int cache_get_alloc(const char *key, void **out_data, size_t *out_len) {
    if (!s_state.initialised)
        return CACHE_ERR_INIT;
    if (!key || !out_data || !out_len)
        return CACHE_ERR_PARAM;

    *out_data = NULL;
    *out_len = 0;

    const cache_io_t *io = s_state.cfg.io;

    char path[MAX_PATH_LEN];
    build_path(key, path, sizeof(path));

    if (!io->exists(path))
        return CACHE_ERR_NOT_FOUND;

    long file_sz = io->get_size(path);
    if (file_sz < (long)sizeof(cache_file_header_t))
        return CACHE_ERR_CORRUPT;

    uint8_t *raw = (uint8_t *)cache_malloc((size_t)file_sz);
    if (!raw)
        return CACHE_ERR_NOMEM;

    int bytes_read = io->read(path, raw, (size_t)file_sz);
    if (bytes_read < (int)sizeof(cache_file_header_t)) {
        cache_free_internal(raw);
        return CACHE_ERR_IO;
    }

    cache_file_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));

    if (hdr.magic != HEADER_MAGIC) {
        cache_free_internal(raw);
        return CACHE_ERR_CORRUPT;
    }

    /* Verify key to catch hash collisions */
    if (strncmp(hdr.key, key, MAX_KEY_LEN - 1) != 0) {
        cache_free_internal(raw);
        return CACHE_ERR_NOT_FOUND;
    }

    uint32_t stored_crc = hdr.crc32;
    hdr.crc32 = 0; /* zero out before recomputing */
    uint32_t calc_crc = 0;
    const uint8_t *hdr_body =
        (const uint8_t *)&hdr + sizeof(hdr.magic) + sizeof(hdr.crc32);
    size_t hdr_body_len = sizeof(hdr) - sizeof(hdr.magic) - sizeof(hdr.crc32);
    calc_crc = crc32_update(calc_crc, hdr_body, hdr_body_len);
    if (hdr.payload_len > 0)
        calc_crc = crc32_update(calc_crc, raw + sizeof(cache_file_header_t),
                                hdr.payload_len);

    if (calc_crc != stored_crc) {
        cache_free_internal(raw);
        return CACHE_ERR_CORRUPT;
    }

    if (hdr.expires_at != 0 && now_sec() >= hdr.expires_at) {
        cache_free_internal(raw);
        /* Remove stale file opportunistically */
        io->remove(path);
        return CACHE_ERR_EXPIRED;
    }

    size_t payload_len = hdr.payload_len;
    uint8_t *payload =
        (uint8_t *)cache_malloc(payload_len + 1); /* +1 for safe NUL */
    if (!payload) {
        cache_free_internal(raw);
        return CACHE_ERR_NOMEM;
    }

    memcpy(payload, raw + sizeof(cache_file_header_t), payload_len);
    payload[payload_len] = '\0'; /* convenience NUL; not counted in out_len */

    cache_free_internal(raw);

    *out_data = payload;
    *out_len = payload_len;
    return CACHE_OK;
}

void cache_free(void *ptr) { cache_free_internal(ptr); }

int cache_remove(const char *key) {
    if (!s_state.initialised)
        return CACHE_ERR_INIT;
    if (!key)
        return CACHE_ERR_PARAM;

    const cache_io_t *io = s_state.cfg.io;
    char path[MAX_PATH_LEN];
    build_path(key, path, sizeof(path));

    if (!io->exists(path))
        return CACHE_ERR_NOT_FOUND;

    return (io->remove(path) == 0) ? CACHE_OK : CACHE_ERR_IO;
}

typedef struct {
    const cache_io_t *io;
    const char *root;
    bool purge_all; /**< If true, remove everything       */
    int removed;    /**< Running count of removed entries */
} cleanup_ctx_t;

static void cleanup_cb(const char *filename, void *user_ctx) {
    cleanup_ctx_t *ctx = (cleanup_ctx_t *)user_ctx;

    /* Only process our own files */
    size_t fn_len = strlen(filename);
    size_t ext_len = strlen(CACHE_FILE_EXT);
    if (fn_len < ext_len ||
        strcmp(filename + fn_len - ext_len, CACHE_FILE_EXT) != 0)
        return;

    /* Build full path */
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", ctx->root, filename);

    if (ctx->purge_all) {
        if (ctx->io->remove(path) == 0)
            ctx->removed++;
        return;
    }

    /* Read header only to check expiry — avoid reading full payload */
    long file_sz = ctx->io->get_size(path);
    if (file_sz < (long)sizeof(cache_file_header_t)) {
        /* Corrupted file — remove it */
        if (ctx->io->remove(path) == 0)
            ctx->removed++;
        return;
    }

    cache_file_header_t hdr;
    int n = ctx->io->read(path, &hdr, sizeof(hdr));
    if (n < (int)sizeof(hdr) || hdr.magic != HEADER_MAGIC) {
        ctx->io->remove(path);
        ctx->removed++;
        return;
    }

    if (hdr.expires_at != 0 && now_sec() >= hdr.expires_at) {
        if (ctx->io->remove(path) == 0)
            ctx->removed++;
    }
}

int cache_cleanup(void) {
    if (!s_state.initialised)
        return CACHE_ERR_INIT;

    cleanup_ctx_t ctx = {
        .io = s_state.cfg.io,
        .root = s_state.cfg.root_path,
        .purge_all = false,
        .removed = 0,
    };

    int rc = s_state.cfg.io->list_dir(s_state.cfg.root_path, cleanup_cb, &ctx);
    if (rc != 0)
        return CACHE_ERR_IO;

    return ctx.removed;
}

int cache_purge_all(void) {
    if (!s_state.initialised)
        return CACHE_ERR_INIT;

    cleanup_ctx_t ctx = {
        .io = s_state.cfg.io,
        .root = s_state.cfg.root_path,
        .purge_all = true,
        .removed = 0,
    };

    int rc = s_state.cfg.io->list_dir(s_state.cfg.root_path, cleanup_cb, &ctx);
    if (rc != 0)
        return CACHE_ERR_IO;

    return ctx.removed;
}
