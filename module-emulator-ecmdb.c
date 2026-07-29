#define MODULE_LOG_PREFIX "emu"

#include "globals.h"

#ifdef WITH_EMU

#include "ncam-string.h"
#include "module-emulator-nemu.h"
#include "module-emulator-ecmdb-priv.h"

/*
 * ECMDB Core: hashing, line parsing, channel directory scan, and the
 * public API (ecmdb_init/ecmdb_ecm/ecmdb_cleanup).
 *
 * Storage-mode-specific code lives in its own file and is reached only
 * through the ecmdb_backend_t dispatch table (see
 * module-emulator-ecmdb-priv.h):
 *   module-emulator-ecmdb-ram.c     - ECMDB_MODE_RAM
 *   module-emulator-ecmdb-direct.c  - ECMDB_MODE_DIRECT
 */

static ecmdb_t *ecmdb = NULL;

// xxHash32 Implementation (inline, optimized)
#define XXH_PRIME32_1  0x9E3779B1U
#define XXH_PRIME32_2  0x85EBCA77U
#define XXH_PRIME32_3  0xC2B2AE3DU
#define XXH_PRIME32_4  0x27D4EB2FU
#define XXH_PRIME32_5  0x165667B1U

static inline uint32_t xxh_rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t xxh_read32(const void *ptr)
{
    uint32_t val;
    memcpy(&val, ptr, sizeof(val));
    return val;
}

uint32_t xxhash32(const uint8_t *data, size_t len, uint32_t seed)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t h32;
    
    if (len >= 16)
    {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + XXH_PRIME32_1 + XXH_PRIME32_2;
        uint32_t v2 = seed + XXH_PRIME32_2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XXH_PRIME32_1;
        
        do {
            v1 += xxh_read32(p) * XXH_PRIME32_2; p += 4;
            v1 = xxh_rotl32(v1, 13);
            v1 *= XXH_PRIME32_1;
            
            v2 += xxh_read32(p) * XXH_PRIME32_2; p += 4;
            v2 = xxh_rotl32(v2, 13);
            v2 *= XXH_PRIME32_1;
            
            v3 += xxh_read32(p) * XXH_PRIME32_2; p += 4;
            v3 = xxh_rotl32(v3, 13);
            v3 *= XXH_PRIME32_1;
            
            v4 += xxh_read32(p) * XXH_PRIME32_2; p += 4;
            v4 = xxh_rotl32(v4, 13);
            v4 *= XXH_PRIME32_1;
        } while (p <= limit);
        
        h32 = xxh_rotl32(v1, 1) + xxh_rotl32(v2, 7) + 
              xxh_rotl32(v3, 12) + xxh_rotl32(v4, 18);
    }
    else
    {
        h32 = seed + XXH_PRIME32_5;
    }
    
    h32 += (uint32_t)len;
    
    while (p + 4 <= end)
    {
        h32 += xxh_read32(p) * XXH_PRIME32_3;
        h32 = xxh_rotl32(h32, 17) * XXH_PRIME32_4;
        p += 4;
    }
    
    while (p < end)
    {
        h32 += (*p++) * XXH_PRIME32_5;
        h32 = xxh_rotl32(h32, 11) * XXH_PRIME32_1;
    }
    
    h32 ^= h32 >> 15;
    h32 *= XXH_PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= XXH_PRIME32_3;
    h32 ^= h32 >> 16;
    
    return h32;
}

// Utility Functions
static inline uint64_t make_lookup_key(uint16_t caid, uint16_t srvid)
{
    return ((uint64_t)caid << 32) | srvid;
}

static int compare_keys(const void *a, const void *b)
{
    uint64_t ka = *(const uint64_t*)a;
    uint64_t kb = *(const uint64_t*)b;
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--) *p++ = 0;
}

// Fast hex nibble decode - avoids per-byte sscanf() overhead in the hot
// parsing path (called for every line loaded and, in DIRECT mode, for
// every seek-and-verify on an ECM lookup).
static inline int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int hex_decode(const char *src, uint8_t *dst, size_t nbytes)
{
    size_t i;
    for (i = 0; i < nbytes; i++)
    {
        int hi = hex_nibble(src[i * 2]);
        int lo = hex_nibble(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

// Filename Parser
static int parse_channel_filename(const char *filename, uint16_t *caid, 
                                   uint16_t *srvid, uint8_t *start, uint8_t *end)
{
    const char *bracket = strchr(filename, '[');
    const char *bracket_end = strchr(filename, ']');
    
    if (!bracket || !bracket_end || bracket_end <= bracket)
        return 0;
    
    char base[64];
    size_t len = bracket - filename;
    if (len >= sizeof(base)) return 0;
    
    memcpy(base, filename, len);
    base[len] = '\0';
    
    int s, e;
    if (sscanf(bracket, "[%d#%d]", &s, &e) != 2 || 
        s < 0 || s >= 256 || e <= s || e > 256)
        return 0;
    
    *start = (uint8_t)s;
    *end = (uint8_t)e;
    
    return sscanf(base, "%04hX@%04hX", caid, srvid) == 2;
}

// ECM Line Parser - Modified to accept any valid length
int parse_ecm_line(const char *line, uint8_t *ecm, uint8_t *cw, 
                    uint16_t *ecm_len)
{
    while (*line == ' ' || *line == '\t') line++;
    
    const char *cw_marker = strstr(line, " #CW ");
    if (!cw_marker) return 0;
    
    size_t hex_len = cw_marker - line;
    if (hex_len == 0 || hex_len % 2 != 0)
        return 0;
    
    *ecm_len = hex_len / 2;
    
    // Only check maximum length
    if (*ecm_len > ECMDB_MAX_ECM_LEN)
        return 0;
    
    if (!hex_decode(line, ecm, *ecm_len))
        return 0;
    
    const char *cw_start = cw_marker + 5;
    while (*cw_start == ' ' || *cw_start == '\t') cw_start++;
    
    if (cs_strlen(cw_start) < ECMDB_CW_LEN * 2)
        return 0;
    
    if (!hex_decode(cw_start, cw, ECMDB_CW_LEN))
        return 0;
    
    return 1;
}

// Selects the storage backend for the database's current mode.
static const ecmdb_backend_t *ecmdb_backend(void)
{
    return (ecmdb->mode == ECMDB_MODE_RAM) ? &ecmdb_backend_ram : &ecmdb_backend_direct;
}

// Channel Management
static int add_channel(const char *filepath, uint16_t caid, uint16_t srvid, 
                      uint8_t ecm_start, uint8_t ecm_end)
{
    if (ecmdb->channel_count >= ECMDB_MAX_CHANNELS) 
        return 0;
    
    ecmdb_channel_t *ch = &ecmdb->channels[ecmdb->channel_count];
    memset(ch, 0, sizeof(ecmdb_channel_t));
    
    ch->caid = caid;
    ch->srvid = srvid;
    ch->ecm_start = ecm_start;
    ch->ecm_end = ecm_end;
    ch->filepath = cs_strdup(filepath);
    
    if (!ch->filepath) return 0;
    
    if (!ecmdb_backend()->load(ch, filepath, ecm_start, ecm_end))
    {
        cs_log("ECMDB: %04X@%04X [%d#%d] No valid entries found",
               caid, srvid, ecm_start, ecm_end);
        free(ch->filepath);
        return 0;
    }

    if (ecmdb->mode == ECMDB_MODE_RAM)
    {
        cs_log("ECMDB: %04X@%04X [%d#%d] %u entries (%zu KB)", 
               caid, srvid, ecm_start, ecm_end, 
               ch->entry_count, ch->pool_used / 1024);
    }
    else
    {
        cs_log("ECMDB: %04X@%04X [%d#%d] %u entries (DIRECT mode)", 
               caid, srvid, ecm_start, ecm_end, ch->entry_count);
    }
    
    ecmdb->lookup_keys[ecmdb->channel_count] = make_lookup_key(caid, srvid);
    ecmdb->channel_count++;
    
    return 1;
}

static void scan_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;
    
    struct dirent *entry;
    char fullpath[512];
    
    while ((entry = readdir(dir)) && 
           ecmdb->channel_count < ECMDB_MAX_CHANNELS)
    {
        if (entry->d_name[0] == '.' || strstr(entry->d_name, ".."))
            continue;
        
        int ret = snprintf(fullpath, sizeof(fullpath), "%s/%s", 
                          path, entry->d_name);
        if (ret >= (int)sizeof(fullpath) || ret < 0) 
            continue;
        
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        
        if (S_ISDIR(st.st_mode))
        {
            scan_directory(fullpath);
        }
        else if (S_ISREG(st.st_mode))
        {
            uint16_t caid, srvid;
            uint8_t start, end;
            
            if (parse_channel_filename(entry->d_name, &caid, &srvid, 
                                      &start, &end))
            {
                add_channel(fullpath, caid, srvid, start, end);
            }
        }
    }
    
    closedir(dir);

    if (ecmdb->channel_count >= ECMDB_MAX_CHANNELS)
    {
        cs_log("ECMDB: Channel limit (%d) reached, some files under %s may have been skipped", 
               ECMDB_MAX_CHANNELS, path);
    }
}

static ecmdb_channel_t* find_channel(uint16_t caid, uint16_t srvid, 
                                     uint32_t *out_idx)
{
    uint64_t key = make_lookup_key(caid, srvid);
    
    uint64_t *found = bsearch(&key, ecmdb->lookup_keys, 
                              ecmdb->channel_count,
                              sizeof(uint64_t), compare_keys);
    if (!found) return NULL;
    
    uint32_t idx = found - ecmdb->lookup_keys;
    if (out_idx) *out_idx = idx;
    
    return &ecmdb->channels[idx];
}

// Public API
int8_t ecmdb_init(struct s_reader *rdr)
{
    if (ecmdb) return EMU_OK;
    
    if (!rdr->ecmdb_path || cs_strlen(rdr->ecmdb_path) == 0)
        return EMU_NOT_SUPPORTED;
    
    struct stat st;
    if (stat(rdr->ecmdb_path, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        cs_log("ECMDB: Invalid path: %s", rdr->ecmdb_path);
        return EMU_NOT_SUPPORTED;
    }
    
    ecmdb = calloc(1, sizeof(ecmdb_t));
    if (!ecmdb) return EMU_OUT_OF_MEMORY;
    
    ecmdb->channels = calloc(ECMDB_MAX_CHANNELS, sizeof(ecmdb_channel_t));
    ecmdb->lookup_keys = calloc(ECMDB_MAX_CHANNELS, sizeof(uint64_t));
    
    if (!ecmdb->channels || !ecmdb->lookup_keys)
    {
        free(ecmdb->channels);
        free(ecmdb->lookup_keys);
        free(ecmdb);
        ecmdb = NULL;
        return EMU_OUT_OF_MEMORY;
    }
    
    if (pthread_mutex_init(&ecmdb->lock, NULL) != 0)
    {
        free(ecmdb->channels);
        free(ecmdb->lookup_keys);
        free(ecmdb);
        ecmdb = NULL;
        return EMU_OUT_OF_MEMORY;
    }
    
    ecmdb->mode = (rdr->ecmdb_mode == 1) ? ECMDB_MODE_RAM : ECMDB_MODE_DIRECT;
    
    cs_log("ECMDB: Loading from %s [%s mode]", 
           rdr->ecmdb_path,
           ecmdb->mode == ECMDB_MODE_RAM ? "RAM" : "DIRECT");
    
    scan_directory(rdr->ecmdb_path);
    
    if (ecmdb->channel_count > 0)
    {
        qsort(ecmdb->lookup_keys, ecmdb->channel_count, 
              sizeof(uint64_t), compare_keys);
        
        size_t total_memory = 0;
        uint32_t total_entries = 0;
        uint32_t i;
        
        for (i = 0; i < ecmdb->channel_count; i++)
        {
            ecmdb_channel_t *ch = &ecmdb->channels[i];
            total_memory += ch->pool_used;
            total_entries += ch->entry_count;
        }
        
        cs_log("ECMDB: %u channels loaded, %u total entries", 
               ecmdb->channel_count, total_entries);
        
        if (ecmdb->mode == ECMDB_MODE_RAM)
        {
            cs_log("ECMDB: Memory: %.2f MB", total_memory / (1024.0f * 1024.0f));
        }
        
        ecmdb->initialized = 1;
    }
    else
    {
        cs_log("ECMDB: No channels found");
    }
    
    return EMU_OK;
}

int8_t ecmdb_ecm(uint16_t caid, uint16_t srvid, const uint8_t *ecm, uint8_t *cw)
{
    if (!ecmdb || !ecmdb->initialized)
        return EMU_NOT_SUPPORTED;
    
    SAFE_MUTEX_LOCK(&ecmdb->lock);
    
    uint32_t channel_idx;
    ecmdb_channel_t *ch = find_channel(caid, srvid, &channel_idx);
    
    if (!ch)
    {
        SAFE_MUTEX_UNLOCK(&ecmdb->lock);
        return EMU_KEY_NOT_FOUND;
    }
    
    size_t ecm_len = ch->ecm_end - ch->ecm_start;
    if (ecm_len == 0 || ch->ecm_start + ecm_len > ECMDB_MAX_ECM_LEN)
    {
        SAFE_MUTEX_UNLOCK(&ecmdb->lock);
        return EMU_CORRUPT_DATA;
    }
    
    const uint8_t *ecm_data = &ecm[ch->ecm_start];

    int found = ecmdb_backend()->search(ecmdb, ch, channel_idx, ecm_data, ecm_len, cw);
    
    SAFE_MUTEX_UNLOCK(&ecmdb->lock);
    
    return found ? EMU_OK : EMU_CW_NOT_FOUND;
}

void ecmdb_cleanup(void)
{
    if (!ecmdb) return;
    
    SAFE_MUTEX_LOCK(&ecmdb->lock);
    
    ecmdb_direct_cache_close_all(ecmdb);
    
    if (ecmdb->channels)
    {
        uint32_t i;
        for (i = 0; i < ecmdb->channel_count; i++)
        {
            ecmdb_channel_t *ch = &ecmdb->channels[i];
            
            if (ch->filepath) free(ch->filepath);

            ecmdb_backend_ram.cleanup(ch);
            ecmdb_backend_direct.cleanup(ch);
        }
        free(ecmdb->channels);
    }
    
    free(ecmdb->lookup_keys);
    
    SAFE_MUTEX_UNLOCK(&ecmdb->lock);
    pthread_mutex_destroy(&ecmdb->lock);
    
    free(ecmdb);
    ecmdb = NULL;
    
    cs_log("ECMDB: Cleanup complete");
}

#endif // WITH_EMU
