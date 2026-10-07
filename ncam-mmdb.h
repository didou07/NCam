#ifndef NCAM_MMDB_H_
#define NCAM_MMDB_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NCAM_MMDB_OK 0
#define NCAM_MMDB_NOT_FOUND 1
#define NCAM_MMDB_ERROR -1

typedef struct
{
	uint8_t *file_content;
	size_t file_size;
	const uint8_t *data_section;
	uint32_t data_section_size;
	uint32_t node_count;
	uint16_t record_size;
	uint16_t ip_version;
	uint16_t depth;
	uint32_t ipv4_start_node;
	uint16_t ipv4_start_netmask;
	bool open;
} NCAM_MMDB;

int ncam_mmdb_open(const char *path, NCAM_MMDB *db);
void ncam_mmdb_close(NCAM_MMDB *db);
int ncam_mmdb_lookup(const NCAM_MMDB *db, const uint8_t address[16], bool address_is_ipv6, uint32_t *data_offset);
int ncam_mmdb_get_country(const NCAM_MMDB *db, uint32_t data_offset, char code[3]);

#endif
