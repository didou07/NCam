#define MODULE_LOG_PREFIX "emu"

#include "globals.h"

#ifdef WITH_EMU

#include "ncam-string.h"
#include "module-emulator-ecmdb-priv.h"

/*
 * ECMDB DIRECT Backend
 *
 * Keeps ECM/CW data on disk instead of in RAM (the whole point of this
 * mode), but - unlike the previous implementation - builds a lightweight
 * on-disk index at load time: just {hash, file offset, length} per line,
 * sorted by hash. A lookup then binary-searches the index and seeks
 * straight to the matching line(s) instead of scanning the entire file
 * top to bottom on every single ECM. This turns lookups from O(n) file
 * scans into O(log n) index searches + a handful of seeks, while the
 * index itself stays tiny (12 bytes/entry) compared to keeping the ECM
 * bytes themselves in memory (RAM mode's job).
 */

#define ECMDB_DIRECT_INDEX_INITIAL_CAP 64

// ---------------------------------------------------------------------
// File Handle Cache (shared across all channels via the ecmdb_t passed
// into search(); the cache array itself lives on the shared db so it
// isn't duplicated per channel)
// ---------------------------------------------------------------------

static ecmdb_cache_t *ecmdb_direct_cache_find(ecmdb_t *db, uint32_t channel_idx)
{
    int i;
    for (i = 0; i < ECMDB_FILE_CACHE_SIZE; i++)
    {
        if (db->file_cache[i].fp &&
            db->file_cache[i].channel_idx == channel_idx)
        {
            db->file_cache[i].last_access = time(NULL);
            return &db->file_cache[i];
        }
    }
    return NULL;
}

static ecmdb_cache_t *ecmdb_direct_cache_get_lru(ecmdb_t *db)
{
    ecmdb_cache_t *lru = NULL;
    time_t oldest = time(NULL);
    int i;

    for (i = 0; i < ECMDB_FILE_CACHE_SIZE; i++)
    {
        if (!db->file_cache[i].fp)
            return &db->file_cache[i];

        if (!db->file_cache[i].in_use &&
            db->file_cache[i].last_access < oldest)
        {
            oldest = db->file_cache[i].last_access;
            lru = &db->file_cache[i];
        }
    }
    return lru;
}

static FILE *ecmdb_direct_cache_open(ecmdb_t *db, ecmdb_channel_t *ch, uint32_t channel_idx)
{
    ecmdb_cache_t *entry = ecmdb_direct_cache_find(db, channel_idx);
    if (entry)
    {
        entry->in_use = 1;
        return entry->fp;
    }

    FILE *fp = fopen(ch->filepath, "r");
    if (!fp) return NULL;

    entry = ecmdb_direct_cache_get_lru(db);
    if (!entry)
    {
        fclose(fp);
        return NULL;
    }

    if (entry->fp) fclose(entry->fp);

    entry->fp = fp;
    entry->channel_idx = channel_idx;
    entry->last_access = time(NULL);
    entry->in_use = 1;

    return fp;
}

static void ecmdb_direct_cache_release(ecmdb_t *db, uint32_t channel_idx)
{
    int i;
    for (i = 0; i < ECMDB_FILE_CACHE_SIZE; i++)
    {
        if (db->file_cache[i].fp &&
            db->file_cache[i].channel_idx == channel_idx)
        {
            db->file_cache[i].in_use = 0;
            return;
        }
    }
}

void ecmdb_direct_cache_close_all(ecmdb_t *db)
{
    int i;
    for (i = 0; i < ECMDB_FILE_CACHE_SIZE; i++)
    {
        if (db->file_cache[i].fp)
        {
            fclose(db->file_cache[i].fp);
            db->file_cache[i].fp = NULL;
        }
    }
}

// ---------------------------------------------------------------------
// Index build (load) and lookup (search)
// ---------------------------------------------------------------------

static int ecmdb_direct_compare_by_hash(const void *a, const void *b)
{
    uint32_t ha = ((const ecmdb_index_entry_t *)a)->hash;
    uint32_t hb = ((const ecmdb_index_entry_t *)b)->hash;
    return (ha < hb) ? -1 : (ha > hb) ? 1 : 0;
}

static int ecmdb_direct_load(ecmdb_channel_t *ch, const char *filepath,
                             uint8_t ecm_start, uint8_t ecm_end)
{
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;

    uint16_t expected_len = ecm_end - ecm_start;
    char line[1024];
    uint8_t ecm_buf[ECMDB_MAX_ECM_LEN], cw_buf[ECMDB_CW_LEN];
    uint16_t ecm_len;

    uint32_t invalid_count = 0;
    uint32_t index_cap = 0, index_count = 0;
    ecmdb_index_entry_t *index = NULL;

    long line_offset;
    while ((line_offset = ftell(fp)) >= 0 && fgets(line, sizeof(line), fp))
    {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
            continue;

        if (!parse_ecm_line(line, ecm_buf, cw_buf, &ecm_len))
        {
            invalid_count++;
            continue;
        }

        // Check if length matches filename specification
        if (ecm_len != expected_len)
        {
            invalid_count++;
            continue;
        }

        if (index_count >= index_cap)
        {
            uint32_t new_cap = index_cap ? index_cap * 2 : ECMDB_DIRECT_INDEX_INITIAL_CAP;
            ecmdb_index_entry_t *new_index = realloc(index, new_cap * sizeof(ecmdb_index_entry_t));
            if (!new_index)
            {
                cs_log("ECMDB: out of memory building index for %s, remaining entries skipped", filepath);
                break;
            }
            index = new_index;
            index_cap = new_cap;
        }

        index[index_count].hash = xxhash32(ecm_buf, ecm_len, 0);
        index[index_count].offset = line_offset;
        index[index_count].len = ecm_len;
        index_count++;
    }

    secure_zero(ecm_buf, sizeof(ecm_buf));
    secure_zero(cw_buf, sizeof(cw_buf));
    fclose(fp);

    if (invalid_count > 0)
    {
        cs_log("ECMDB: Skipped %u invalid/mismatched-length entries in %s (DIRECT mode)",
               invalid_count, filepath);
    }

    if (index_count == 0)
    {
        cs_log("ECMDB: %s No valid entries found (DIRECT mode)", filepath);
        free(index);
        return 0;
    }

    qsort(index, index_count, sizeof(ecmdb_index_entry_t), ecmdb_direct_compare_by_hash);

    ch->index = index;
    ch->index_count = index_count;
    ch->entry_count = index_count;

    cs_log("ECMDB: %s %u entries indexed (DIRECT mode)", filepath, index_count);

    return 1;
}

static int ecmdb_direct_search(ecmdb_t *db, ecmdb_channel_t *ch, uint32_t channel_idx,
                               const uint8_t *ecm_data, size_t ecm_len, uint8_t *cw)
{
    if (!ch->index || ch->index_count == 0)
        return 0;

    uint32_t hash = xxhash32(ecm_data, ecm_len, 0);

    ecmdb_index_entry_t key = { .hash = hash, .offset = 0, .len = 0 };
    ecmdb_index_entry_t *found = bsearch(&key, ch->index, ch->index_count,
                                         sizeof(ecmdb_index_entry_t),
                                         ecmdb_direct_compare_by_hash);
    if (!found) return 0;

    // bsearch may land anywhere within a run of equal hashes (32-bit hash
    // collisions do happen on large databases) - widen to the whole
    // contiguous run and verify each candidate's real bytes below rather
    // than trusting the hash alone.
    uint32_t idx = (uint32_t)(found - ch->index);
    uint32_t lo = idx, hi = idx;
    while (lo > 0 && ch->index[lo - 1].hash == hash) lo--;
    while (hi + 1 < ch->index_count && ch->index[hi + 1].hash == hash) hi++;

    FILE *fp = ecmdb_direct_cache_open(db, ch, channel_idx);
    if (!fp) return 0;

    char line[1024];
    uint8_t line_ecm[ECMDB_MAX_ECM_LEN], line_cw[ECMDB_CW_LEN];
    uint16_t line_ecm_len;
    int matched = 0;
    uint32_t i;

    for (i = lo; i <= hi; i++)
    {
        if (ch->index[i].len != ecm_len)
            continue;

        if (fseek(fp, ch->index[i].offset, SEEK_SET) != 0)
            continue;
        if (!fgets(line, sizeof(line), fp))
            continue;

        if (!parse_ecm_line(line, line_ecm, line_cw, &line_ecm_len))
            continue;
        if (line_ecm_len != ecm_len)
            continue;

        if (memcmp(ecm_data, line_ecm, ecm_len) == 0)
        {
            memcpy(cw, line_cw, ECMDB_CW_LEN);
            matched = 1;
            break;
        }
    }

    secure_zero(line_ecm, sizeof(line_ecm));
    secure_zero(line_cw, sizeof(line_cw));
    ecmdb_direct_cache_release(db, channel_idx);

    return matched;
}

static void ecmdb_direct_cleanup(ecmdb_channel_t *ch)
{
    if (ch->index)
    {
        free(ch->index);
        ch->index = NULL;
        ch->index_count = 0;
    }
}

const ecmdb_backend_t ecmdb_backend_direct = {
    .load    = ecmdb_direct_load,
    .search  = ecmdb_direct_search,
    .cleanup = ecmdb_direct_cleanup,
};

#endif // WITH_EMU
