#ifndef NCAM_COUNTRY_H_
#define NCAM_COUNTRY_H_

#include "globals.h"

#define NCAM_COUNTRY_CODE_LEN 2
#define NCAM_COUNTRY_CODE_STR_LEN 3

void ncam_country_init(void);
void ncam_country_reload(void);
int32_t ncam_country_access_allowed(IN_ADDR_T ip);
int32_t ncam_country_lookup(IN_ADDR_T ip, char code[NCAM_COUNTRY_CODE_STR_LEN]);
const char *ncam_country_name(const char *code);
void ncam_country_status(char *out, size_t outlen);
int32_t ncam_country_download_db(char *out, size_t outlen);

#endif
