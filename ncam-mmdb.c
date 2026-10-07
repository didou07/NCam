#include "ncam-mmdb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCAM_MMDB_METADATA_MARKER "\xAB\xCD\xEFMaxMind.com"
#define NCAM_MMDB_METADATA_MARKER_LEN 14U
#define NCAM_MMDB_METADATA_MAX 131072U
#define NCAM_MMDB_SEPARATOR 16U
#define NCAM_MMDB_MAX_NODES 100000000U
#define NCAM_MMDB_MAX_DEPTH 128U

enum
{
	MMDB_T_EXTENDED = 0,
	MMDB_T_POINTER = 1,
	MMDB_T_UTF8 = 2,
	MMDB_T_DOUBLE = 3,
	MMDB_T_BYTES = 4,
	MMDB_T_UINT16 = 5,
	MMDB_T_UINT32 = 6,
	MMDB_T_MAP = 7,
	MMDB_T_INT32 = 8,
	MMDB_T_UINT64 = 9,
	MMDB_T_UINT128 = 10,
	MMDB_T_ARRAY = 11,
	MMDB_T_CONTAINER = 12,
	MMDB_T_END = 13,
	MMDB_T_BOOL = 14,
	MMDB_T_FLOAT = 15
};

typedef struct
{
	uint8_t type;
	uint32_t size;
	uint32_t data_offset;
	uint32_t next_offset;
	uint32_t pointer;
	const uint8_t *bytes;
} NCAM_MMDB_VALUE;

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool range_ok(size_t total, uint32_t off, size_t len)
{
	return (size_t)off <= total && len <= total - (size_t)off;
}

static int get_size(const uint8_t *mem, size_t total, uint8_t ctrl, uint32_t *offset, uint32_t *size)
{
	uint32_t n = ctrl & 0x1fU;
	if(n == 29U)
	{
		if(!range_ok(total, *offset, 1)) return NCAM_MMDB_ERROR;
		n = 29U + mem[(*offset)++];
	}
	else if(n == 30U)
	{
		if(!range_ok(total, *offset, 2)) return NCAM_MMDB_ERROR;
		n = 285U + rd16(&mem[*offset]);
		*offset += 2;
	}
	else if(n == 31U)
	{
		if(!range_ok(total, *offset, 3)) return NCAM_MMDB_ERROR;
		n = 65821U + rd24(&mem[*offset]);
		*offset += 3;
	}
	*size = n;
	return NCAM_MMDB_OK;
}

static int decode(const uint8_t *mem, size_t total, uint32_t offset, NCAM_MMDB_VALUE *v)
{
	if(!v || !range_ok(total, offset, 1))
		return NCAM_MMDB_ERROR;
	memset(v, 0, sizeof(*v));
	v->data_offset = offset;
	uint32_t p = offset;
	uint8_t ctrl = mem[p++];
	uint8_t type = (uint8_t)((ctrl >> 5) & 7U);
	if(type == MMDB_T_EXTENDED)
	{
		if(!range_ok(total, p, 1)) return NCAM_MMDB_ERROR;
		type = (uint8_t)(7U + mem[p++]);
	}
	v->type = type;
	if(type == MMDB_T_POINTER)
	{
		uint32_t psize = ((ctrl >> 3) & 3U) + 1U;
		if(!range_ok(total, p, psize)) return NCAM_MMDB_ERROR;
		if(psize == 1U)
			v->pointer = ((uint32_t)(ctrl & 7U) << 8) | mem[p];
		else if(psize == 2U)
			v->pointer = 2048U + ((uint32_t)(ctrl & 7U) << 16) + ((uint32_t)mem[p] << 8) + mem[p + 1];
		else if(psize == 3U)
			v->pointer = 2048U + 524288U + ((uint32_t)(ctrl & 7U) << 24) + rd24(&mem[p]);
		else
			v->pointer = rd32(&mem[p]);
		v->size = psize;
		v->data_offset = p;
		v->next_offset = p + psize;
		return NCAM_MMDB_OK;
	}
	if(get_size(mem, total, ctrl, &p, &v->size) != NCAM_MMDB_OK)
		return NCAM_MMDB_ERROR;
	v->data_offset = p;
	if(type == MMDB_T_MAP || type == MMDB_T_ARRAY)
	{
		v->next_offset = p;
		return NCAM_MMDB_OK;
	}
	if(type == MMDB_T_BOOL)
	{
		v->next_offset = p;
		return NCAM_MMDB_OK;
	}
	if(!range_ok(total, p, v->size))
		return NCAM_MMDB_ERROR;
	v->bytes = &mem[p];
	v->next_offset = p + v->size;
	return NCAM_MMDB_OK;
}

static int follow(const uint8_t *mem, size_t total, uint32_t offset, NCAM_MMDB_VALUE *v)
{
	if(decode(mem, total, offset, v) != NCAM_MMDB_OK)
		return NCAM_MMDB_ERROR;
	if(v->type == MMDB_T_POINTER)
	{
		uint32_t next = v->next_offset;
		NCAM_MMDB_VALUE target;
		if(decode(mem, total, v->pointer, &target) != NCAM_MMDB_OK)
			return NCAM_MMDB_ERROR;
		if(target.type == MMDB_T_POINTER)
			return NCAM_MMDB_ERROR;
		*v = target;
		if(target.type != MMDB_T_MAP && target.type != MMDB_T_ARRAY)
			v->next_offset = next;
	}
	return NCAM_MMDB_OK;
}

static int skip_value(const uint8_t *mem, size_t total, uint32_t offset, uint32_t *next, uint32_t depth)
{
	if(depth > 64U) return NCAM_MMDB_ERROR;
	NCAM_MMDB_VALUE v;
	if(decode(mem, total, offset, &v) != NCAM_MMDB_OK)
		return NCAM_MMDB_ERROR;
	if(v.type == MMDB_T_POINTER)
	{
		*next = v.next_offset;
		return NCAM_MMDB_OK;
	}
	if(v.type == MMDB_T_MAP)
	{
		uint32_t p = v.next_offset;
		for(uint32_t i = 0; i < v.size; i++)
		{
			if(skip_value(mem, total, p, &p, depth + 1U) != NCAM_MMDB_OK) return NCAM_MMDB_ERROR;
			if(skip_value(mem, total, p, &p, depth + 1U) != NCAM_MMDB_OK) return NCAM_MMDB_ERROR;
		}
		*next = p;
		return NCAM_MMDB_OK;
	}
	if(v.type == MMDB_T_ARRAY)
	{
		uint32_t p = v.next_offset;
		for(uint32_t i = 0; i < v.size; i++)
			if(skip_value(mem, total, p, &p, depth + 1U) != NCAM_MMDB_OK) return NCAM_MMDB_ERROR;
		*next = p;
		return NCAM_MMDB_OK;
	}
	*next = v.next_offset;
	return NCAM_MMDB_OK;
}

static int map_find(const uint8_t *mem, size_t total, uint32_t offset, const char *key, uint32_t *value_offset)
{
	NCAM_MMDB_VALUE root;
	if(follow(mem, total, offset, &root) != NCAM_MMDB_OK || root.type != MMDB_T_MAP)
		return NCAM_MMDB_ERROR;
	uint32_t p = root.next_offset;
	for(uint32_t i = 0; i < root.size; i++)
	{
		NCAM_MMDB_VALUE k;
		if(follow(mem, total, p, &k) != NCAM_MMDB_OK || k.type != MMDB_T_UTF8)
			return NCAM_MMDB_ERROR;
		uint32_t value = k.next_offset;
		if(strlen(key) == k.size && memcmp(k.bytes, key, k.size) == 0)
		{
			if(value_offset) *value_offset = value;
			return NCAM_MMDB_OK;
		}
		if(skip_value(mem, total, value, &p, 0) != NCAM_MMDB_OK)
			return NCAM_MMDB_ERROR;
	}
	return NCAM_MMDB_NOT_FOUND;
}

static int parse_metadata(const uint8_t *mem, size_t total, size_t marker_offset, uint32_t *node_count, uint16_t *record_size, uint16_t *ip_version)
{
	if(marker_offset + NCAM_MMDB_METADATA_MARKER_LEN > total)
		return NCAM_MMDB_ERROR;
	uint32_t p = (uint32_t)(marker_offset + NCAM_MMDB_METADATA_MARKER_LEN);
	NCAM_MMDB_VALUE root;
	if(follow(mem, total, p, &root) != NCAM_MMDB_OK || root.type != MMDB_T_MAP)
		return NCAM_MMDB_ERROR;
	struct field
	{
		const char *name;
		uint32_t *u32;
		uint16_t *u16;
	} fields[] = {
		{"node_count", node_count, NULL},
		{"record_size", NULL, record_size},
		{"ip_version", NULL, ip_version}
	};
	uint32_t cur = root.next_offset;
	for(uint32_t i = 0; i < root.size; i++)
	{
		NCAM_MMDB_VALUE k;
		if(follow(mem, total, cur, &k) != NCAM_MMDB_OK || k.type != MMDB_T_UTF8)
			return NCAM_MMDB_ERROR;
		uint32_t value = k.next_offset;
		for(size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++)
		{
			if(strlen(fields[f].name) != k.size || memcmp(k.bytes, fields[f].name, k.size) != 0)
				continue;
			NCAM_MMDB_VALUE val;
			if(follow(mem, total, value, &val) != NCAM_MMDB_OK)
				return NCAM_MMDB_ERROR;
			if(val.type == MMDB_T_UINT16 && fields[f].u16)
			{
				if(val.size > 2) return NCAM_MMDB_ERROR;
				uint16_t x = 0;
				for(uint32_t j = 0; j < val.size; j++) x = (uint16_t)((x << 8) | val.bytes[j]);
				*fields[f].u16 = x;
			}
			else if(val.type == MMDB_T_UINT32 && fields[f].u32)
			{
				if(val.size > 4) return NCAM_MMDB_ERROR;
				uint32_t x = 0;
				for(uint32_t j = 0; j < val.size; j++) x = (x << 8) | val.bytes[j];
				*fields[f].u32 = x;
			}
			else
			{
				return NCAM_MMDB_ERROR;
			}
		}
		if(skip_value(mem, total, value, &cur, 0) != NCAM_MMDB_OK)
			return NCAM_MMDB_ERROR;
	}
	return (*node_count && *record_size && (*ip_version == 4 || *ip_version == 6)) ? NCAM_MMDB_OK : NCAM_MMDB_ERROR;
}

static int find_marker(const uint8_t *mem, size_t total, size_t *marker)
{
	if(total < NCAM_MMDB_METADATA_MARKER_LEN) return NCAM_MMDB_ERROR;
	size_t begin = total > NCAM_MMDB_METADATA_MAX ? total - NCAM_MMDB_METADATA_MAX : 0;
	for(size_t p = total - NCAM_MMDB_METADATA_MARKER_LEN + 1; p > begin; )
	{
		--p;
		if(memcmp(mem + p, NCAM_MMDB_METADATA_MARKER, NCAM_MMDB_METADATA_MARKER_LEN) == 0)
		{
			*marker = p;
			return NCAM_MMDB_OK;
		}
	}
	return NCAM_MMDB_ERROR;
}

static uint32_t record_left(const NCAM_MMDB *db, const uint8_t *p)
{
	if(db->record_size == 24)
		return rd24(p);
	if(db->record_size == 28)
		return rd24(p) + ((uint32_t)(p[3] & 0xf0U) << 20);
	return rd32(p);
}

static uint32_t record_right(const NCAM_MMDB *db, const uint8_t *p)
{
	if(db->record_size == 24)
		return rd24(p);
	if(db->record_size == 28)
		return rd32(p) & 0x0fffffffU;
	return rd32(p);
}

static uint32_t record_bytes(const NCAM_MMDB *db)
{
	return (uint32_t)db->record_size * 2U / 8U;
}

static int find_ipv4_start(const NCAM_MMDB *db, uint32_t *node, uint16_t *netmask)
{
	uint32_t value = 0;
	uint16_t bits = 0;
	uint32_t rb = record_bytes(db);
	while(bits < 96U && value < db->node_count)
	{
		if((uint64_t)value * rb + rb > db->file_size) return NCAM_MMDB_ERROR;
		value = record_left(db, db->file_content + (size_t)value * rb);
		bits++;
	}
	*node = value;
	*netmask = bits;
	return NCAM_MMDB_OK;
}

int ncam_mmdb_open(const char *path, NCAM_MMDB *db)
{
	if(!path || !*path || !db) return NCAM_MMDB_ERROR;
	memset(db, 0, sizeof(*db));
	FILE *fp = fopen(path, "rb");
	if(!fp) return NCAM_MMDB_ERROR;
	if(fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NCAM_MMDB_ERROR; }
	long end = ftell(fp);
	if(end <= 0) { fclose(fp); return NCAM_MMDB_ERROR; }
	db->file_size = (size_t)end;
	if((long)db->file_size != end) { fclose(fp); return NCAM_MMDB_ERROR; }
	if(fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NCAM_MMDB_ERROR; }
	db->file_content = (uint8_t *)malloc(db->file_size);
	if(!db->file_content) { fclose(fp); return NCAM_MMDB_ERROR; }
	if(fread(db->file_content, 1, db->file_size, fp) != db->file_size)
	{
		fclose(fp);
		free(db->file_content);
		memset(db, 0, sizeof(*db));
		return NCAM_MMDB_ERROR;
	}
	fclose(fp);
	size_t marker = 0;
	if(find_marker(db->file_content, db->file_size, &marker) != NCAM_MMDB_OK) goto fail;
	if(marker < NCAM_MMDB_SEPARATOR) goto fail;
	uint32_t node_count = 0;
	uint16_t record_size = 0, ip_version = 0;
	if(parse_metadata(db->file_content, db->file_size, marker, &node_count, &record_size, &ip_version) != NCAM_MMDB_OK) goto fail;
	if(node_count > NCAM_MMDB_MAX_NODES || (record_size != 24 && record_size != 28 && record_size != 32)) goto fail;
	uint32_t rb = (uint32_t)record_size * 2U / 8U;
	uint64_t tree_size = (uint64_t)node_count * rb;
	uint64_t data_start = tree_size + NCAM_MMDB_SEPARATOR;
	if(data_start > marker || data_start > db->file_size || marker - data_start > UINT32_MAX) goto fail;
	db->node_count = node_count;
	db->record_size = record_size;
	db->ip_version = ip_version;
	db->depth = ip_version == 4 ? 32 : 128;
	db->data_section = db->file_content + (size_t)data_start;
	db->data_section_size = (uint32_t)(marker - data_start);
	if(db->data_section_size < 3) goto fail;
	if(ip_version == 6 && find_ipv4_start(db, &db->ipv4_start_node, &db->ipv4_start_netmask) != NCAM_MMDB_OK) goto fail;
	db->open = true;
	return NCAM_MMDB_OK;
fail:
	free(db->file_content);
	memset(db, 0, sizeof(*db));
	return NCAM_MMDB_ERROR;
}

void ncam_mmdb_close(NCAM_MMDB *db)
{
	if(!db) return;
	free(db->file_content);
	memset(db, 0, sizeof(*db));
}

int ncam_mmdb_lookup(const NCAM_MMDB *db, const uint8_t address[16], bool address_is_ipv6, uint32_t *data_offset)
{
	if(!db || !db->open || !address || !data_offset) return NCAM_MMDB_ERROR;
	const uint8_t *addr = address;
	uint8_t mapped[16];
	uint32_t value = 0;
	uint16_t bit = 0;
	if(db->ip_version == 4)
	{
		if(address_is_ipv6) return NCAM_MMDB_NOT_FOUND;
		addr = address + 12;
	}
	else if(!address_is_ipv6)
	{
		memset(mapped, 0, sizeof(mapped));
		memcpy(mapped + 12, address + 12, 4);
		addr = mapped;
		value = db->ipv4_start_node;
		bit = db->ipv4_start_netmask;
	}
	uint32_t rb = record_bytes(db);
	for(; bit < db->depth && value < db->node_count; bit++)
	{
		if((uint64_t)value * rb + rb > db->file_size) return NCAM_MMDB_ERROR;
		const uint8_t *record = db->file_content + (size_t)value * rb;
		uint32_t next;
		uint8_t b = (uint8_t)((addr[bit >> 3] >> (7 - (bit & 7))) & 1U);
		if(b)
			next = record_right(db, record + (db->record_size == 24 ? 3 : db->record_size == 28 ? 3 : 4));
		else
			next = record_left(db, record);
		value = next;
	}
	if(value == db->node_count) return NCAM_MMDB_NOT_FOUND;
	if(value < db->node_count) return NCAM_MMDB_ERROR;
	uint64_t off = (uint64_t)value - db->node_count - NCAM_MMDB_SEPARATOR;
	if(off >= db->data_section_size) return NCAM_MMDB_ERROR;
	*data_offset = (uint32_t)off;
	return NCAM_MMDB_OK;
}

int ncam_mmdb_get_country(const NCAM_MMDB *db, uint32_t data_offset, char code[3])
{
	if(code) memcpy(code, "??", 3);
	if(!db || !db->open || !code || data_offset >= db->data_section_size) return NCAM_MMDB_ERROR;
	uint32_t country_offset = 0;
	int rc = map_find(db->data_section, db->data_section_size, data_offset, "country", &country_offset);
	if(rc != NCAM_MMDB_OK) return rc;
	uint32_t iso_offset = 0;
	rc = map_find(db->data_section, db->data_section_size, country_offset, "iso_code", &iso_offset);
	if(rc != NCAM_MMDB_OK) return rc;
	NCAM_MMDB_VALUE v;
	if(follow(db->data_section, db->data_section_size, iso_offset, &v) != NCAM_MMDB_OK || v.type != MMDB_T_UTF8 || v.size != 2)
		return NCAM_MMDB_ERROR;
	code[0] = (char)toupper((unsigned char)v.bytes[0]);
	code[1] = (char)toupper((unsigned char)v.bytes[1]);
	code[2] = '\0';
	return NCAM_MMDB_OK;
}
