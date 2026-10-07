#define MODULE_LOG_PREFIX "country"

#include "globals.h"
#include "ncam-country.h"
#include "ncam-net.h"
#include "ncam-files.h"
#include "ncam-string.h"
#include "ncam-mmdb.h"
#include <ctype.h>
#include <strings.h>

#ifdef WITH_LIBCURL
#include <curl/curl.h>
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__CYGWIN__)
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#endif

#define NCAM_COUNTRY_MAX_CODES 256
#define NCAM_COUNTRY_DBIP_DEFAULT "dbip-country-lite.mmdb"
#define NCAM_COUNTRY_DBIP_MAX_COMPRESSED (16U * 1024U * 1024U)
#define NCAM_COUNTRY_DBIP_MAX_DATABASE (32U * 1024U * 1024U)

typedef struct
{
	const char *code;
	const char *name;
} NCAM_COUNTRY_CODE;

static const NCAM_COUNTRY_CODE country_codes[] =
{
	{"AF","Afghanistan"},{"AL","Albania"},{"DZ","Algeria"},{"AS","American Samoa"},{"AD","Andorra"},{"AO","Angola"},{"AI","Anguilla"},{"AQ","Antarctica"},{"AG","Antigua and Barbuda"},{"AR","Argentina"},{"AM","Armenia"},{"AW","Aruba"},{"AU","Australia"},{"AT","Austria"},{"AZ","Azerbaijan"},
	{"BS","Bahamas"},{"BH","Bahrain"},{"BD","Bangladesh"},{"BB","Barbados"},{"BY","Belarus"},{"BE","Belgium"},{"BZ","Belize"},{"BJ","Benin"},{"BM","Bermuda"},{"BT","Bhutan"},{"BO","Bolivia"},{"BQ","Bonaire, Sint Eustatius and Saba"},{"BA","Bosnia and Herzegovina"},{"BW","Botswana"},{"BV","Bouvet Island"},{"BR","Brazil"},{"IO","British Indian Ocean Territory"},{"BN","Brunei"},{"BG","Bulgaria"},{"BF","Burkina Faso"},{"BI","Burundi"},
	{"CV","Cabo Verde"},{"KH","Cambodia"},{"CM","Cameroon"},{"CA","Canada"},{"KY","Cayman Islands"},{"CF","Central African Republic"},{"TD","Chad"},{"CL","Chile"},{"CN","China"},{"CX","Christmas Island"},{"CC","Cocos (Keeling) Islands"},{"CO","Colombia"},{"KM","Comoros"},{"CG","Congo"},{"CD","Congo, Democratic Republic of the"},{"CK","Cook Islands"},{"CR","Costa Rica"},{"CI","Cote d'Ivoire"},{"HR","Croatia"},{"CU","Cuba"},{"CW","Curacao"},{"CY","Cyprus"},{"CZ","Czechia"},
	{"DK","Denmark"},{"DJ","Djibouti"},{"DM","Dominica"},{"DO","Dominican Republic"},{"EC","Ecuador"},{"EG","Egypt"},{"SV","El Salvador"},{"GQ","Equatorial Guinea"},{"ER","Eritrea"},{"EE","Estonia"},{"SZ","Eswatini"},{"ET","Ethiopia"},{"FK","Falkland Islands"},{"FO","Faroe Islands"},{"FJ","Fiji"},{"FI","Finland"},{"FR","France"},{"GF","French Guiana"},{"PF","French Polynesia"},{"TF","French Southern Territories"},
	{"GA","Gabon"},{"GM","Gambia"},{"GE","Georgia"},{"DE","Germany"},{"GH","Ghana"},{"GI","Gibraltar"},{"GR","Greece"},{"GL","Greenland"},{"GD","Grenada"},{"GP","Guadeloupe"},{"GU","Guam"},{"GT","Guatemala"},{"GG","Guernsey"},{"GN","Guinea"},{"GW","Guinea-Bissau"},{"GY","Guyana"},{"HT","Haiti"},{"HM","Heard Island and McDonald Islands"},{"VA","Holy See"},{"HN","Honduras"},{"HK","Hong Kong"},{"HU","Hungary"},
	{"IS","Iceland"},{"IN","India"},{"ID","Indonesia"},{"IR","Iran"},{"IQ","Iraq"},{"IE","Ireland"},{"IM","Isle of Man"},{"IL","Israel"},{"IT","Italy"},{"JM","Jamaica"},{"JP","Japan"},{"JE","Jersey"},{"JO","Jordan"},{"KZ","Kazakhstan"},{"KE","Kenya"},{"KI","Kiribati"},{"KP","Korea, North"},{"KR","Korea, South"},{"KW","Kuwait"},{"KG","Kyrgyzstan"},
	{"LA","Laos"},{"LV","Latvia"},{"LB","Lebanon"},{"LS","Lesotho"},{"LR","Liberia"},{"LY","Libya"},{"LI","Liechtenstein"},{"LT","Lithuania"},{"LU","Luxembourg"},{"MO","Macao"},{"MG","Madagascar"},{"MW","Malawi"},{"MY","Malaysia"},{"MV","Maldives"},{"ML","Mali"},{"MT","Malta"},{"MH","Marshall Islands"},{"MQ","Martinique"},{"MR","Mauritania"},{"MU","Mauritius"},{"YT","Mayotte"},{"MX","Mexico"},{"FM","Micronesia"},{"MD","Moldova"},{"MC","Monaco"},{"MN","Mongolia"},{"ME","Montenegro"},{"MS","Montserrat"},{"MA","Morocco"},{"MZ","Mozambique"},{"MM","Myanmar"},
	{"NA","Namibia"},{"NR","Nauru"},{"NP","Nepal"},{"NL","Netherlands"},{"NC","New Caledonia"},{"NZ","New Zealand"},{"NI","Nicaragua"},{"NE","Niger"},{"NG","Nigeria"},{"NU","Niue"},{"NF","Norfolk Island"},{"MK","North Macedonia"},{"MP","Northern Mariana Islands"},{"NO","Norway"},{"OM","Oman"},{"PK","Pakistan"},{"PW","Palau"},{"PS","Palestine"},{"PA","Panama"},{"PG","Papua New Guinea"},{"PY","Paraguay"},{"PE","Peru"},{"PH","Philippines"},{"PN","Pitcairn"},{"PL","Poland"},{"PT","Portugal"},{"PR","Puerto Rico"},{"QA","Qatar"},
	{"RE","Reunion"},{"RO","Romania"},{"RU","Russia"},{"RW","Rwanda"},{"BL","Saint Barthelemy"},{"SH","Saint Helena, Ascension and Tristan da Cunha"},{"KN","Saint Kitts and Nevis"},{"LC","Saint Lucia"},{"MF","Saint Martin"},{"PM","Saint Pierre and Miquelon"},{"VC","Saint Vincent and the Grenadines"},{"WS","Samoa"},{"SM","San Marino"},{"ST","Sao Tome and Principe"},{"SA","Saudi Arabia"},{"SN","Senegal"},{"RS","Serbia"},{"SC","Seychelles"},{"SL","Sierra Leone"},{"SG","Singapore"},{"SX","Sint Maarten"},{"SK","Slovakia"},{"SI","Slovenia"},{"SB","Solomon Islands"},{"SO","Somalia"},{"ZA","South Africa"},{"GS","South Georgia and the South Sandwich Islands"},{"SS","South Sudan"},{"ES","Spain"},{"LK","Sri Lanka"},{"SD","Sudan"},{"SR","Suriname"},{"SJ","Svalbard and Jan Mayen"},{"SE","Sweden"},{"CH","Switzerland"},{"SY","Syria"},{"TW","Taiwan"},{"TJ","Tajikistan"},{"TZ","Tanzania"},{"TH","Thailand"},{"TL","Timor-Leste"},{"TG","Togo"},{"TK","Tokelau"},{"TO","Tonga"},{"TT","Trinidad and Tobago"},{"TN","Tunisia"},{"TR","Turkey"},{"TM","Turkmenistan"},{"TC","Turks and Caicos Islands"},{"TV","Tuvalu"},{"UG","Uganda"},{"UA","Ukraine"},{"AE","United Arab Emirates"},{"GB","United Kingdom"},{"UM","United States Minor Outlying Islands"},{"US","United States"},{"UY","Uruguay"},{"UZ","Uzbekistan"},{"VU","Vanuatu"},{"VE","Venezuela"},{"VN","Vietnam"},{"VG","Virgin Islands, British"},{"VI","Virgin Islands, U.S."},{"WF","Wallis and Futuna"},{"EH","Western Sahara"},{"YE","Yemen"},{"ZM","Zambia"},{"ZW","Zimbabwe"}
};

typedef struct
{
	NCAM_MMDB mmdb;
	bool db_open;
	bool initialized;
	bool service_enabled;
	bool allowed[26][26];
	char configured_codes[NCAM_COUNTRY_MAX_CODES * NCAM_COUNTRY_CODE_STR_LEN];
	char status[256];
	pthread_rwlock_t lock;
	bool lock_ready;
} NCAM_COUNTRY_STATE;

static NCAM_COUNTRY_STATE country_state = {0};
extern char cs_confdir[];

static void country_set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(country_state.status, sizeof(country_state.status), fmt, ap);
	va_end(ap);
}

static bool country_code_valid(const char *code)
{
	return code && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z' && code[2] == '\0';
}

static const char *country_find_name(const char *code);

static void country_codes_clear(void)
{
	memset(country_state.allowed, 0, sizeof(country_state.allowed));
	country_state.configured_codes[0] = '\0';
	country_state.service_enabled = false;
}

static void country_parse_config(void)
{
	country_codes_clear();
	country_state.service_enabled = cfg.http_country_enabled != 0;
	if(!cfg.http_allowed_countries || !*cfg.http_allowed_countries)
		return;
	char *tmp = cs_strdup(cfg.http_allowed_countries);
	if(!tmp)
		return;
	char *saveptr = NULL;
	char *tok = strtok_r(tmp, ",; \t\r\n", &saveptr);
	while(tok)
	{
		if(cs_strlen(tok) == 2)
		{
			tok[0] = (char)toupper((unsigned char)tok[0]);
			tok[1] = (char)toupper((unsigned char)tok[1]);
			if(country_code_valid(tok) && country_find_name(tok))
			{
				country_state.allowed[tok[0] - 'A'][tok[1] - 'A'] = true;
				size_t used = cs_strlen(country_state.configured_codes);
				if(used + (used ? 1 : 0) + 2 < sizeof(country_state.configured_codes))
				{
					if(used) country_state.configured_codes[used++] = ',';
					country_state.configured_codes[used++] = tok[0];
					country_state.configured_codes[used++] = tok[1];
					country_state.configured_codes[used] = '\0';
				}
			}
			else
			{
				cs_log("country: ignoring invalid country code '%s'", tok);
			}
		}
		tok = strtok_r(NULL, ",; \t\r\n", &saveptr);
	}
	free(tmp);
}

static const char *country_find_name(const char *code)
{
	if(!country_code_valid(code))
		return NULL;
	for(size_t i = 0; i < sizeof(country_codes) / sizeof(country_codes[0]); i++)
	{
		if(code[0] == country_codes[i].code[0] && code[1] == country_codes[i].code[1])
			return country_codes[i].name;
	}
	return NULL;
}

const char *ncam_country_name(const char *code)
{
	const char *name = country_find_name(code);
	return name ? name : "Unknown";
}

void ncam_country_flag(const char *code, char *out, size_t outlen)
{
	if(!out || outlen == 0)
		return;
	out[0] = '\0';
	if(!country_code_valid(code))
		return;
	if(outlen < 9)
	{
		snprintf(out, outlen, "%s", code);
		return;
	}
	unsigned char flag[9] = {0xF0, 0x9F, 0x87, (unsigned char)(0xA6 + (code[0] - 'A')), 0xF0, 0x9F, 0x87, (unsigned char)(0xA6 + (code[1] - 'A')), 0};
	memcpy(out, flag, sizeof(flag));
}

static bool country_local_or_private(IN_ADDR_T ip)
{
#ifdef IPV6SUPPORT
	if(IN6_IS_ADDR_LOOPBACK(&ip) || IN6_IS_ADDR_LINKLOCAL(&ip))
		return true;
	if((ip.s6_addr[0] & 0xFE) == 0xFC)
		return true;
	return false;
#else
	uint32_t v = ntohl(ip);
	return ((v >> 24) == 10) || ((v >> 20) == 0xAC1) || ((v >> 16) == 0xC0A8) || ((v >> 24) == 127) || ((v >> 16) == 0xA9FE);
#endif
}

static bool country_ip_exception(IN_ADDR_T ip)
{
	if(check_ip(cfg.http_country_exceptions, ip))
		return true;
	return false;
}

static void country_close_locked(void)
{
	if(country_state.db_open)
	{
		ncam_mmdb_close(&country_state.mmdb);
		country_state.db_open = false;
	}
}

static bool country_open_db_locked(const char *path)
{
	if(!path || !*path)
		return false;
	return ncam_mmdb_open(path, &country_state.mmdb) == NCAM_MMDB_OK;
}

static void country_default_db_path(char *out, size_t outlen)
{
	if(!out || !outlen) return;
	const char *base = cs_confdir;
	if(!base || !*base) base = ".";
	size_t n = cs_strlen(base);
	if(n && (base[n - 1] == '/' || base[n - 1] == '\\'))
		snprintf(out, outlen, "%s%s", base, NCAM_COUNTRY_DBIP_DEFAULT);
	else
		snprintf(out, outlen, "%s/%s", base, NCAM_COUNTRY_DBIP_DEFAULT);
}


static void country_refresh_status_locked(void)
{
	if(!country_state.service_enabled)
	{
		country_set_status(country_state.db_open ? "Country access disabled · GeoIP ready" : "Country access disabled · GeoIP database not found");
	}
	else if(!country_state.db_open)
	{
		country_set_status("Country access enabled · GeoIP database not found");
	}
	else if(!country_state.configured_codes[0])
	{
		country_set_status("Country access enabled · no countries configured");
	}
	else
	{
		country_set_status("Country access enabled · GeoIP ready");
	}
}

static bool country_load_locked(void)
{
	char default_path[512];
	country_default_db_path(default_path, sizeof(default_path));
	if(file_exists(default_path) && country_open_db_locked(default_path))
	{
		country_state.db_open = true;
		country_refresh_status_locked();
		return true;
	}
	country_close_locked();
	country_refresh_status_locked();
	return false;
}


#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__CYGWIN__)
static bool country_run_program(const char *program, char *const argv[], const char *stdout_path)
{
	pid_t pid = fork();
	if(pid < 0)
		return false;
	if(pid == 0)
	{
		int nullfd = open("/dev/null", O_WRONLY);
		if(nullfd >= 0)
		{
			dup2(nullfd, STDERR_FILENO);
			if(nullfd != STDERR_FILENO)
				close(nullfd);
		}
		if(stdout_path)
		{
			int outfd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if(outfd < 0)
				_exit(126);
			if(dup2(outfd, STDOUT_FILENO) < 0)
				_exit(126);
			if(outfd != STDOUT_FILENO)
				close(outfd);
		}
		execv(program, argv);
		_exit(127);
	}
	int status = 0;
	while(waitpid(pid, &status, 0) < 0)
	{
		if(errno != EINTR)
			return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#ifdef WITH_LIBCURL
static size_t country_curl_write_file(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	FILE *fp = (FILE *)userdata;
	if(!fp || !ptr || size == 0 || nmemb == 0)
		return 0;
	size_t bytes = size * nmemb;
	long pos = ftell(fp);
	if(pos < 0 || (uint64_t)pos + (uint64_t)bytes > NCAM_COUNTRY_DBIP_MAX_COMPRESSED)
		return 0;
	return fwrite(ptr, 1, bytes, fp);
}
#endif

static bool country_download_file(const char *url, const char *dest)
{
#ifdef WITH_LIBCURL
	if(!url || !*url || !dest || !*dest)
		return false;
	if(curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
		return false;

	FILE *fp = fopen(dest, "wb");
	if(!fp)
		return false;

	CURL *curl_handle = curl_easy_init();
	if(!curl_handle)
	{
		fclose(fp);
		return false;
	}

	char errbuf[CURL_ERROR_SIZE] = {0};
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, country_curl_write_file);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 180L);
	curl_easy_setopt(curl_handle, CURLOPT_MAXFILESIZE, (long)NCAM_COUNTRY_DBIP_MAX_COMPRESSED);
	curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "NCam/GeoIP");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);

	CURLcode res = curl_easy_perform(curl_handle);
	long http_code = 0;
	curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl_handle);
	bool close_ok = (fclose(fp) == 0);
	curl_global_cleanup();

	if(res != CURLE_OK || !close_ok)
	{
		cs_log("GeoIP download failed: %s%s%s", curl_easy_strerror(res), errbuf[0] ? " - " : "", errbuf[0] ? errbuf : "");
		unlink(dest);
		return false;
	}
	if(http_code < 200 || http_code >= 300)
	{
		cs_log("GeoIP download rejected HTTP status %ld", http_code);
		unlink(dest);
		return false;
	}
	return true;
#else
	(void)url;
	(void)dest;
	cs_log("GeoIP download unavailable: NCam was built without libcurl");
	return false;
#endif
}

static bool country_decompress_file(const char *src, const char *dest)
{
	char *tool = find_in_path("gzip");
	if(tool)
	{
		char *argv[] = {tool, "-dc", (char *)src, NULL};
		bool ok = country_run_program(tool, argv, dest);
		free(tool);
		if(ok)
			return true;
	}
	tool = find_in_path("busybox");
	if(tool)
	{
		char *argv[] = {tool, "gzip", "-dc", (char *)src, NULL};
		bool ok = country_run_program(tool, argv, dest);
		free(tool);
		return ok;
	}
	return false;
}
#endif

int32_t ncam_country_download_db(char *out, size_t outlen)
{
	if(out && outlen) out[0] = '\0';
#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__CYGWIN__)
	if(out && outlen) snprintf(out, outlen, "GeoIP download is not supported on this platform");
	return 0;
#else
	char target[512];
	country_default_db_path(target, sizeof(target));
	char gz_path[1024], mmdb_path[1024];
	pid_t pid = getpid();
	if(snprintf(gz_path, sizeof(gz_path), "%s.download.%ld.gz", target, (long)pid) >= (int)sizeof(gz_path) ||
		snprintf(mmdb_path, sizeof(mmdb_path), "%s.download.%ld.mmdb", target, (long)pid) >= (int)sizeof(mmdb_path))
	{
		if(out && outlen) snprintf(out, outlen, "GeoIP destination path is too long");
		return 0;
	}
	unlink(gz_path);
	unlink(mmdb_path);
	bool downloaded = false;
	char url[256];
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	for(int attempt = 0; attempt < 2 && !downloaded; attempt++)
	{
		int year = tmv.tm_year + 1900;
		int month = tmv.tm_mon + 1 - attempt;
		if(month <= 0) { month += 12; year--; }
		snprintf(url, sizeof(url), "https://download.db-ip.com/free/dbip-country-lite-%04d-%02d.mmdb.gz", year, month);
		downloaded = country_download_file(url, gz_path);
	}
	if(!downloaded)
	{
		unlink(gz_path);
		if(out && outlen) snprintf(out, outlen, "DB-IP download failed; source is unreachable or unavailable");
		return 0;
	}
	struct stat st;
	if(stat(gz_path, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > NCAM_COUNTRY_DBIP_MAX_COMPRESSED)
	{
		unlink(gz_path);
		if(out && outlen) snprintf(out, outlen, "DB-IP download size is invalid");
		return 0;
	}
	if(!country_decompress_file(gz_path, mmdb_path))
	{
		unlink(gz_path);
		unlink(mmdb_path);
		if(out && outlen) snprintf(out, outlen, "Unable to decompress DB-IP database; gzip or busybox is required");
		return 0;
	}
	unlink(gz_path);
	if(stat(mmdb_path, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > NCAM_COUNTRY_DBIP_MAX_DATABASE)
	{
		unlink(mmdb_path);
		if(out && outlen) snprintf(out, outlen, "DB-IP database size is invalid after decompression");
		return 0;
	}
	if(!country_state.initialized) ncam_country_init();
	if(!country_state.lock_ready)
	{
		unlink(mmdb_path);
		if(out && outlen) snprintf(out, outlen, "GeoIP state could not be initialized");
		return 0;
	}
	SAFE_RWLOCK_WRLOCK(&country_state.lock);
	NCAM_MMDB testdb;
	memset(&testdb, 0, sizeof(testdb));
	bool valid = ncam_mmdb_open(mmdb_path, &testdb) == NCAM_MMDB_OK;
	if(valid) ncam_mmdb_close(&testdb);
	if(valid && rename(mmdb_path, target) != 0) valid = false;
	if(valid) country_refresh_status_locked();
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
	unlink(mmdb_path);
	if(!valid)
	{
		if(out && outlen) snprintf(out, outlen, "DB-IP database validation or installation failed");
		return 0;
	}
	if(out && outlen) snprintf(out, outlen, "DB-IP Country Lite installed successfully: %s", target);
	return 1;
#endif
}

void ncam_country_init(void)
{
	if(!country_state.lock_ready)
	{
		if(pthread_rwlock_init(&country_state.lock, NULL) != 0)
			return;
		country_state.lock_ready = true;
	}
	SAFE_RWLOCK_WRLOCK(&country_state.lock);
	country_parse_config();
	country_close_locked();
	(void)country_load_locked();
	country_state.initialized = true;
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
}

void ncam_country_reload(void)
{
	ncam_country_init();
}

void ncam_country_status(char *out, size_t outlen)
{
	if(!out || outlen == 0) return;
	out[0] = '\0';
	if(!country_state.initialized)
		ncam_country_init();
	SAFE_RWLOCK_RDLOCK(&country_state.lock);
	cs_strncpy(out, country_state.status, outlen);
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
}

int32_t ncam_country_lookup(IN_ADDR_T ip, char code[NCAM_COUNTRY_CODE_STR_LEN])
{
	if(code) memcpy(code, "??", sizeof("??"));
	if(!country_state.initialized) ncam_country_init();
	SAFE_RWLOCK_RDLOCK(&country_state.lock);
	if(!country_state.db_open)
	{
		SAFE_RWLOCK_UNLOCK(&country_state.lock);
		return 0;
	}
	uint8_t address[16] = {0};
#ifdef IPV6SUPPORT
	memcpy(address, ip.s6_addr, sizeof(address));
	bool is_ipv6 = true;
#else
	uint32_t v = htonl(ip);
	memcpy(address + 12, &v, sizeof(v));
	bool is_ipv6 = false;
#endif
	uint32_t data_offset = 0;
	int rc = ncam_mmdb_lookup(&country_state.mmdb, address, is_ipv6, &data_offset);
	if(rc != NCAM_MMDB_OK)
	{
		SAFE_RWLOCK_UNLOCK(&country_state.lock);
		return 0;
	}
	char tmp[NCAM_COUNTRY_CODE_STR_LEN];
	rc = ncam_mmdb_get_country(&country_state.mmdb, data_offset, tmp);
	if(rc == NCAM_MMDB_OK && country_code_valid(tmp))
	{
		if(code) memcpy(code, tmp, sizeof(tmp));
		SAFE_RWLOCK_UNLOCK(&country_state.lock);
		return 1;
	}
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
	return 0;
}

int32_t ncam_country_access_allowed(IN_ADDR_T ip)
{
	if(!country_state.initialized)
		ncam_country_init();
	SAFE_RWLOCK_RDLOCK(&country_state.lock);
	bool enabled = country_state.service_enabled && country_state.configured_codes[0] != '\0';
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
	if(!enabled)
		return 1;
	if(country_local_or_private(ip) || country_ip_exception(ip))
		return 1;
	char code[NCAM_COUNTRY_CODE_STR_LEN];
	if(!ncam_country_lookup(ip, code))
		return 0;
	SAFE_RWLOCK_RDLOCK(&country_state.lock);
	bool allowed = country_code_valid(code) && country_state.allowed[code[0] - 'A'][code[1] - 'A'];
	SAFE_RWLOCK_UNLOCK(&country_state.lock);
	return allowed ? 1 : 0;
}
