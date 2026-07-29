#define MODULE_LOG_PREFIX "emu"

#include "globals.h"

#ifdef WITH_EMU

#include "ncam-string.h"
#include "module-emulator-ecmdb-priv.h"

/*
 * ECMDB RAM Backend
 * Loads a channel's entire ECM/CW table into memory (offset-based pool +
 * hash table), for fast O(1)-average lookups at the cost of RAM.
 */

// RAM Mode - Channel Loading with offset-based storage
static int ecmdb_ram_load(ecmdb_channel_t *ch, const char *filepath,
                          uint8_t ecm_start, uint8_t ecm_end)
{
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;
    
    uint16_t expected_len = ecm_end - ecm_start;
    
    ch->hash_table = calloc(ECMDB_HASH_SIZE, sizeof(ecmdb_entry_t*));
    if (!ch->hash_table)
    {
        fclose(fp);
        return 0;
    }
    
    size_t pool_size = 2 * 1024 * 1024;
    ch->data_pool = malloc(pool_size);
    if (!ch->data_pool)
    {
        free(ch->hash_table);
        fclose(fp);
        return 0;
    }
    
    ch->pool_size = pool_size;
    ch->pool_used = 0;
    
    char line[1024];
    uint8_t ecm_buf[ECMDB_MAX_ECM_LEN], cw_buf[ECMDB_CW_LEN];
    uint16_t ecm_len;
    uint32_t skipped_count = 0;
    uint32_t length_mismatch = 0;
    
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
            continue;
        
        // Parse line (accepts any valid length)
        if (!parse_ecm_line(line, ecm_buf, cw_buf, &ecm_len))
        {
            skipped_count++;
            continue;
        }
        
        // Check if length matches filename specification
        if (ecm_len != expected_len)
        {
            length_mismatch++;
            continue;
        }
        
        uint32_t hash = xxhash32(ecm_buf, ecm_len, 0);
        uint32_t idx = hash % ECMDB_HASH_SIZE;

        // Check duplicates by walking this hash bucket and verifying the
        // full ECM bytes (not just the 32-bit hash, which can collide on
        // large databases and would otherwise silently drop distinct
        // entries). O(1) average, and has no cap on database size.
        int dup = 0;
        ecmdb_entry_t *probe = ch->hash_table[idx];
        while (probe)
        {
            if (probe->hash == hash && probe->ecm_len == ecm_len &&
                memcmp(ch->data_pool + probe->ecm_offset, ecm_buf, ecm_len) == 0)
            {
                dup = 1;
                break;
            }
            probe = probe->next;
        }
        if (dup) continue;
        
        // Expand pool if needed
        if (ch->pool_used + ecm_len > ch->pool_size)
        {
            size_t new_size = ch->pool_size * 2;
            if (new_size > 100 * 1024 * 1024)
            {
                cs_log("ECMDB: 100MB pool cap reached for %s, remaining entries skipped", filepath);
                break;
            }
            
            uint8_t *new_pool = realloc(ch->data_pool, new_size);
            if (!new_pool)
            {
                cs_log("ECMDB: realloc failed while loading %s, remaining entries skipped", filepath);
                break;
            }
            
            ch->data_pool = new_pool;
            ch->pool_size = new_size;
        }
        
        // Add entry using offset instead of pointer
        ecmdb_entry_t *entry = malloc(sizeof(ecmdb_entry_t));
        if (!entry)
        {
            cs_log("ECMDB: out of memory while loading %s, remaining entries skipped", filepath);
            break;
        }
        
        entry->ecm_offset = ch->pool_used;
        memcpy(ch->data_pool + ch->pool_used, ecm_buf, ecm_len);
        ch->pool_used += ecm_len;
        
        memcpy(entry->cw, cw_buf, ECMDB_CW_LEN);
        entry->ecm_len = ecm_len;
        entry->hash = hash;
        
        entry->next = ch->hash_table[idx];
        ch->hash_table[idx] = entry;
        
        ch->entry_count++;
    }
    
    secure_zero(ecm_buf, sizeof(ecm_buf));
    secure_zero(cw_buf, sizeof(cw_buf));
    fclose(fp);
    
    if (skipped_count > 0)
    {
        cs_log("ECMDB: Skipped %u invalid entries in %s", 
               skipped_count, filepath);
    }
    
    if (length_mismatch > 0)
    {
        cs_log("ECMDB: Skipped %u entries with wrong length (expected %u bytes) in %s", 
               length_mismatch, expected_len, filepath);
    }
    
    return ch->entry_count > 0;
}

static int ecmdb_ram_search(ecmdb_t *UNUSED(db), ecmdb_channel_t *ch,
                            uint32_t UNUSED(channel_idx),
                            const uint8_t *ecm_data, size_t ecm_len, uint8_t *cw)
{
    uint32_t hash = xxhash32(ecm_data, ecm_len, 0);
    uint32_t idx = hash % ECMDB_HASH_SIZE;
    
    ecmdb_entry_t *entry = ch->hash_table[idx];
    
    while (entry)
    {
        if (entry->hash == hash && 
            entry->ecm_len == ecm_len &&
            memcmp(ecm_data, ch->data_pool + entry->ecm_offset, ecm_len) == 0)
        {
            memcpy(cw, entry->cw, ECMDB_CW_LEN);
            return 1;
        }
        entry = entry->next;
    }
    
    return 0;
}

static void ecmdb_ram_cleanup(ecmdb_channel_t *ch)
{
    if (ch->hash_table)
    {
        uint32_t j;
        for (j = 0; j < ECMDB_HASH_SIZE; j++)
        {
            ecmdb_entry_t *entry = ch->hash_table[j];
            while (entry)
            {
                ecmdb_entry_t *next = entry->next;
                secure_zero(entry->cw, ECMDB_CW_LEN);
                free(entry);
                entry = next;
            }
        }
        free(ch->hash_table);
        ch->hash_table = NULL;
    }

    if (ch->data_pool)
    {
        secure_zero(ch->data_pool, ch->pool_used);
        free(ch->data_pool);
        ch->data_pool = NULL;
    }
}

const ecmdb_backend_t ecmdb_backend_ram = {
    .load    = ecmdb_ram_load,
    .search  = ecmdb_ram_search,
    .cleanup = ecmdb_ram_cleanup,
};

#endif // WITH_EMU
