#ifndef MODULE_EMULATOR_ECMDB_PRIV_H
#define MODULE_EMULATOR_ECMDB_PRIV_H

#ifdef WITH_EMU

#include "module-emulator-ecmdb.h"

/*
 * Internal declarations shared between:
 *   module-emulator-ecmdb.c         - hashing, parsing, channel scan,
 *                                      public API, backend dispatch
 *   module-emulator-ecmdb-ram.c     - RAM backend (full in-memory hash table)
 *   module-emulator-ecmdb-direct.c  - DIRECT backend (on-disk index + seek)
 *
 * Not a public API - other modules should only include
 * module-emulator-ecmdb.h.
 */

// --- module-emulator-ecmdb.c -----------------------------------------
uint32_t xxhash32(const uint8_t *data, size_t len, uint32_t seed);
int parse_ecm_line(const char *line, uint8_t *ecm, uint8_t *cw, uint16_t *ecm_len);
void secure_zero(void *ptr, size_t len);

/*
 * Backend Dispatch Table
 * Purpose: Lets ecmdb_init()/add_channel()/ecmdb_ecm()/ecmdb_cleanup() in
 * the core file treat RAM and DIRECT mode identically, instead of an
 * `if (mode == RAM) {...} else {...}` scattered through each of them.
 * Adding a third storage mode later means adding a new backend file and
 * one dispatch-table entry - not touching the core lookup path.
 */
typedef struct {
    // Load/validate one channel file. Returns 1 on success (entry_count
    // set on ch), 0 on failure.
    int (*load)(ecmdb_channel_t *ch, const char *filepath,
                uint8_t ecm_start, uint8_t ecm_end);

    // Look up ecm_data (ecm_len bytes) within this channel. db and
    // channel_idx are only meaningful for DIRECT (needed for the shared
    // file-handle cache); RAM's implementation ignores them. Returns 1
    // and fills cw on a match, 0 otherwise.
    int (*search)(ecmdb_t *db, ecmdb_channel_t *ch, uint32_t channel_idx,
                  const uint8_t *ecm_data, size_t ecm_len, uint8_t *cw);

    // Free whatever the backend allocated on ch during load().
    void (*cleanup)(ecmdb_channel_t *ch);
} ecmdb_backend_t;

// --- module-emulator-ecmdb-ram.c --------------------------------------
extern const ecmdb_backend_t ecmdb_backend_ram;

// --- module-emulator-ecmdb-direct.c -----------------------------------
extern const ecmdb_backend_t ecmdb_backend_direct;
// Closes every cached FILE* in db->file_cache (DIRECT mode only). Called
// once from ecmdb_cleanup() in the core file, since the cache lives on
// the shared ecmdb_t rather than per-channel.
void ecmdb_direct_cache_close_all(ecmdb_t *db);

#endif // WITH_EMU
#endif // MODULE_EMULATOR_ECMDB_PRIV_H
