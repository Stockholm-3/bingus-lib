/**
 * @file cache.c
 * @brief Hardware-Agnostic File-Backed Caching Module — Implementation
 *
 * On-disk layout (one file per key)
 * ------------------------------------
 *   [ CacheFileHeader ]  — fixed-size packed binary header
 *   [ payload bytes   ]  — raw data supplied by the caller
 *
 * File naming
 * -----------
 *   <root_path>/<hex(fnv1a32(key))>.cache
 *
 * FNV-1a 32-bit is fast, dependency-free, and produces a fixed-length
 * filename valid on every filesystem. The original key is stored in the
 * header so hash collisions are detected and reported as CACHE_ERR_NOT_FOUND.
 *
 * Multi-instance model
 * --------------------
 * All logic lives in the cache_h*() family which operates on a
 * heap-allocated CacheInstance. The global cache_*() functions are thin
 * wrappers that forward to a single static handle (g_global).
 */

#include "cache.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------- */

#define HEADER_MAGIC 0xCA5ECAC4UL
#define CACHE_FILE_EXT ".cache"
#define MAX_KEY_LEN 128
#define MAX_PATH_LEN 256

/* -------------------------------------------------------------------------
 * On-disk header (packed — same layout on every target)
 * ---------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        /**< Must equal HEADER_MAGIC           */
    uint32_t crc32;        /**< CRC-32 of all bytes that follow   */
    uint64_t created_at;   /**< Unix timestamp at write time      */
    uint64_t expires_at;   /**< Unix timestamp; 0 = no expiry     */
    uint32_t payload_len;  /**< Byte count of payload after header */
    char key[MAX_KEY_LEN]; /**< NUL-terminated original key       */
} CacheFileHeader;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Instance struct — body matches the typedef in cache.h
 *
 * The header declares:  typedef struct CacheInstance *CacheHandle;
 * So the struct tag MUST be CacheInstance here.
 * ---------------------------------------------------------------------- */

struct CacheInstance {
    CacheConfig cfg;
};

/* -------------------------------------------------------------------------
 * Global singleton
 * ---------------------------------------------------------------------- */

static CacheHandle g_global = NULL;

/* -------------------------------------------------------------------------
 * CRC-32 (no external dependencies)
 * ---------------------------------------------------------------------- */

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & -(uint32_t)(crc & 1U));
        }
    }
    return ~crc;
}

/* -------------------------------------------------------------------------
 * FNV-1a 32-bit hash
 * ---------------------------------------------------------------------- */

static uint32_t fnv1a32(const char* str) {
    uint32_t h = 0x811C9DC5UL;
    while (*str) {
        h ^= (uint8_t)*str++;
        h *= 0x01000193UL;
    }
    return h;
}

/* -------------------------------------------------------------------------
 * Per-instance helpers
 * ---------------------------------------------------------------------- */

static void inst_build_path(const struct CacheInstance* inst, const char* key, char* out,
                            size_t out_sz) {
    snprintf(out, out_sz, "%s/%08" PRIx32 "%s", inst->cfg.root_path, fnv1a32(key), CACHE_FILE_EXT);
}

static void* inst_malloc(const struct CacheInstance* inst, size_t sz) {
    if (inst->cfg.alloc.malloc_fn) {
        return inst->cfg.alloc.malloc_fn(sz);
    }
    return malloc(sz);
}

static void inst_free(const struct CacheInstance* inst, void* ptr) {
    if (!ptr) {
        return;
    }
    if (inst->cfg.alloc.free_fn) {
        inst->cfg.alloc.free_fn(ptr);
    } else {
        free(ptr);
    }
}

/**
 * Current time as a Unix timestamp.
 * On bare-metal targets without an RTC, replace time() with a monotonic
 * tick counter — relative TTLs will still function correctly.
 */
static uint64_t now_sec(void) { return (uint64_t)time(NULL); }

/* =========================================================================
 * Handle-based API
 * ====================================================================== */

int cache_create(const CacheConfig* config, CacheHandle* out_handle) {
    if (!config || !config->io || !config->root_path || !out_handle) {
        return CACHE_ERR_PARAM;
    }

    struct CacheInstance* inst = malloc(sizeof(*inst));
    if (!inst) {
        return CACHE_ERR_NOMEM;
    }

    inst->cfg   = *config;
    *out_handle = inst;
    return CACHE_OK;
}

void cache_hdestroy(CacheHandle handle) {
    if (!handle) {
        return;
    }
    free(handle);
}

int cache_hput(CacheHandle handle, const char* key, const void* data, size_t len,
               uint32_t ttl_sec) {
    if (!handle) {
        return CACHE_ERR_INIT;
    }
    if (!key || (!data && len > 0U)) {
        return CACHE_ERR_PARAM;
    }

    struct CacheInstance* inst = handle;
    const CacheIo* io          = inst->cfg.io;

    CacheFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = HEADER_MAGIC;
    hdr.created_at  = now_sec();
    hdr.payload_len = (uint32_t)len;
    strncpy(hdr.key, key, MAX_KEY_LEN - 1);

    if (ttl_sec == CACHE_TTL_INFINITE && inst->cfg.default_ttl_sec != CACHE_TTL_INFINITE) {
        ttl_sec = inst->cfg.default_ttl_sec;
    }

    hdr.expires_at = (ttl_sec == CACHE_TTL_INFINITE) ? 0U : hdr.created_at + ttl_sec;

    /* CRC covers every header byte after magic+crc fields, then payload */
    const uint8_t* hdr_body = (const uint8_t*)&hdr + sizeof(hdr.magic) + sizeof(hdr.crc32);
    size_t hdr_body_len     = sizeof(hdr) - sizeof(hdr.magic) - sizeof(hdr.crc32);
    uint32_t crc            = crc32_update(0U, hdr_body, hdr_body_len);
    if (len > 0U) {
        crc = crc32_update(crc, (const uint8_t*)data, len);
    }
    hdr.crc32 = crc;

    size_t total = sizeof(hdr) + len;
    uint8_t* buf = (uint8_t*)inst_malloc(inst, total);
    if (!buf) {
        return CACHE_ERR_NOMEM;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    if (len > 0U) {
        memcpy(buf + sizeof(hdr), data, len);
    }

    char path[MAX_PATH_LEN];
    inst_build_path(inst, key, path, sizeof(path));

    int rc = io->write(path, buf, total);
    inst_free(inst, buf);
    return (rc == 0) ? CACHE_OK : CACHE_ERR_IO;
}

int cache_hget_alloc(CacheHandle handle, const char* key, void** out_data, size_t* out_len) {
    if (!handle) {
        return CACHE_ERR_INIT;
    }
    if (!key || !out_data || !out_len) {
        return CACHE_ERR_PARAM;
    }

    *out_data = NULL;
    *out_len  = 0U;

    struct CacheInstance* inst = handle;
    const CacheIo* io          = inst->cfg.io;

    char path[MAX_PATH_LEN];
    inst_build_path(inst, key, path, sizeof(path));

    if (!io->exists(path)) {
        return CACHE_ERR_NOT_FOUND;
    }

    long file_sz = io->get_size(path);
    if (file_sz < (long)sizeof(CacheFileHeader)) {
        return CACHE_ERR_CORRUPT;
    }

    uint8_t* raw = (uint8_t*)inst_malloc(inst, (size_t)file_sz);
    if (!raw) {
        return CACHE_ERR_NOMEM;
    }

    int nread = io->read(path, raw, (size_t)file_sz);
    if (nread < (int)sizeof(CacheFileHeader)) {
        inst_free(inst, raw);
        return CACHE_ERR_IO;
    }

    CacheFileHeader hdr;
    memcpy(&hdr, raw, sizeof(hdr));

    if (hdr.magic != HEADER_MAGIC) {
        inst_free(inst, raw);
        return CACHE_ERR_CORRUPT;
    }

    if (strncmp(hdr.key, key, MAX_KEY_LEN - 1) != 0) {
        inst_free(inst, raw);
        return CACHE_ERR_NOT_FOUND;
    }

    /* Verify CRC — zero the field before recomputing */
    uint32_t stored_crc     = hdr.crc32;
    hdr.crc32               = 0U;
    const uint8_t* hdr_body = (const uint8_t*)&hdr + sizeof(hdr.magic) + sizeof(hdr.crc32);
    size_t hdr_body_len     = sizeof(hdr) - sizeof(hdr.magic) - sizeof(hdr.crc32);
    uint32_t calc_crc       = crc32_update(0U, hdr_body, hdr_body_len);
    if (hdr.payload_len > 0U) {
        calc_crc = crc32_update(calc_crc, raw + sizeof(CacheFileHeader), hdr.payload_len);
    }

    if (calc_crc != stored_crc) {
        inst_free(inst, raw);
        return CACHE_ERR_CORRUPT;
    }

    if (hdr.expires_at != 0U && now_sec() >= hdr.expires_at) {
        inst_free(inst, raw);
        (void)io->remove(path); /* opportunistic removal; ignore error */
        return CACHE_ERR_EXPIRED;
    }

    size_t plen      = hdr.payload_len;
    uint8_t* payload = (uint8_t*)inst_malloc(inst, plen + 1U); /* +1 for NUL */
    if (!payload) {
        inst_free(inst, raw);
        return CACHE_ERR_NOMEM;
    }

    memcpy(payload, raw + sizeof(CacheFileHeader), plen);
    payload[plen] = '\0'; /* convenience NUL; not reflected in out_len */

    inst_free(inst, raw);
    *out_data = payload;
    *out_len  = plen;
    return CACHE_OK;
}

void cache_hfree(CacheHandle handle, void* ptr) {
    if (!handle || !ptr) {
        return;
    }
    inst_free(handle, ptr);
}

int cache_hremove(CacheHandle handle, const char* key) {
    if (!handle) {
        return CACHE_ERR_INIT;
    }
    if (!key) {
        return CACHE_ERR_PARAM;
    }

    struct CacheInstance* inst = handle;
    char path[MAX_PATH_LEN];
    inst_build_path(inst, key, path, sizeof(path));

    if (!inst->cfg.io->exists(path)) {
        return CACHE_ERR_NOT_FOUND;
    }

    return (inst->cfg.io->remove(path) == 0) ? CACHE_OK : CACHE_ERR_IO;
}

/* -------------------------------------------------------------------------
 * Cleanup / purge internals
 * ---------------------------------------------------------------------- */

typedef struct {
    const struct CacheInstance* inst;
    bool purge_all;
    int removed;
} CleanupCtx;

static void cleanup_cb(const char* filename, void* user_ctx) {
    CleanupCtx* ctx = (CleanupCtx*)user_ctx;

    size_t fn_len  = strlen(filename);
    size_t ext_len = strlen(CACHE_FILE_EXT);
    if (fn_len < ext_len || strcmp(filename + fn_len - ext_len, CACHE_FILE_EXT) != 0) {
        return;
    }

    char path[MAX_PATH_LEN];
    int written = snprintf(path, sizeof(path), "%s/%s", ctx->inst->cfg.root_path, filename);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return;
    }

    if (ctx->purge_all) {
        if (ctx->inst->cfg.io->remove(path) == 0) {
            ctx->removed++;
        }
        return;
    }

    /* Header-only read — avoids loading the full payload */
    long file_sz = ctx->inst->cfg.io->get_size(path);
    if (file_sz < (long)sizeof(CacheFileHeader)) {
        (void)ctx->inst->cfg.io->remove(path);
        ctx->removed++;
        return;
    }

    CacheFileHeader hdr;
    int n = ctx->inst->cfg.io->read(path, &hdr, sizeof(hdr));
    if (n < (int)sizeof(hdr) || hdr.magic != HEADER_MAGIC) {
        (void)ctx->inst->cfg.io->remove(path);
        ctx->removed++;
        return;
    }

    if (hdr.expires_at != 0U && now_sec() >= hdr.expires_at) {
        if (ctx->inst->cfg.io->remove(path) == 0) {
            ctx->removed++;
        }
    }
}

int cache_hcleanup(CacheHandle handle) {
    if (!handle) {
        return CACHE_ERR_INIT;
    }
    struct CacheInstance* inst = handle;
    CleanupCtx ctx             = {.inst = inst, .purge_all = false, .removed = 0};
    int rc                     = inst->cfg.io->list_dir(inst->cfg.root_path, cleanup_cb, &ctx);
    return (rc == 0) ? ctx.removed : CACHE_ERR_IO;
}

int cache_hpurge_all(CacheHandle handle) {
    if (!handle) {
        return CACHE_ERR_INIT;
    }
    struct CacheInstance* inst = handle;
    CleanupCtx ctx             = {.inst = inst, .purge_all = true, .removed = 0};
    int rc                     = inst->cfg.io->list_dir(inst->cfg.root_path, cleanup_cb, &ctx);
    return (rc == 0) ? ctx.removed : CACHE_ERR_IO;
}

/* =========================================================================
 * Global singleton wrappers
 * ====================================================================== */

int cache_init(const CacheConfig* config) {
    if (g_global) {
        return CACHE_OK;
    }
    return cache_create(config, &g_global);
}

void cache_deinit(void) {
    cache_hdestroy(g_global);
    g_global = NULL;
}

int cache_put(const char* key, const void* data, size_t len, uint32_t ttl_sec) {
    if (!g_global) {
        return CACHE_ERR_INIT;
    }
    return cache_hput(g_global, key, data, len, ttl_sec);
}

int cache_get_alloc(const char* key, void** out_data, size_t* out_len) {
    if (!g_global) {
        return CACHE_ERR_INIT;
    }
    return cache_hget_alloc(g_global, key, out_data, out_len);
}

void cache_free(void* ptr) {
    if (g_global) {
        inst_free(g_global, ptr);
    } else {
        free(ptr);
    }
}

int cache_remove(const char* key) {
    if (!g_global) {
        return CACHE_ERR_INIT;
    }
    return cache_hremove(g_global, key);
}

int cache_cleanup(void) {
    if (!g_global) {
        return CACHE_ERR_INIT;
    }
    return cache_hcleanup(g_global);
}

int cache_purge_all(void) {
    if (!g_global) {
        return CACHE_ERR_INIT;
    }
    return cache_hpurge_all(g_global);
}
