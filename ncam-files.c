#define MODULE_LOG_PREFIX "files"

#include "globals.h"

#include "ncam-files.h"
#include "ncam-lock.h"
#include "ncam-string.h"

extern CS_MUTEX_LOCK readdir_lock;
extern char cs_tmpdir[200];

/* Gets the tmp dir */
char *get_tmp_dir(void)
{
	if(cs_tmpdir[0])
	{
		return cs_tmpdir;
	}
#if defined(__CYGWIN__)

	char *d = getenv("TMPDIR");

	if(!d || !d[0])
	{
		d = getenv("TMP");
	}

	if(!d || !d[0])
	{
		d = getenv("TEMP");
	}

	if(!d || !d[0])
	{
		if(!getcwd(cs_tmpdir, sizeof(cs_tmpdir) - 1))
			{ cs_strncpy(cs_tmpdir, ".", sizeof(cs_tmpdir)); }
	}
	else
	{
		cs_strncpy(cs_tmpdir, d, sizeof(cs_tmpdir));
	}
	char *p = cs_tmpdir;
	while(*p) { p++; }
	p--;
	if(*p != '/' && *p != '\\')
	{
		cs_strncpy(cs_tmpdir + cs_strlen(cs_tmpdir), "/", 2);
	}
	cs_strncpy(cs_tmpdir + cs_strlen(cs_tmpdir), "_ncam", 6);
#else
	cs_strncpy(cs_tmpdir, "/tmp/.ncam", sizeof(cs_tmpdir));
#endif
	mkdir(cs_tmpdir, S_IRWXU);
	return cs_tmpdir;
}

char *get_tmp_dir_filename(char *dest, size_t destlen, const char *filename)
{
	char *tmp_dir = get_tmp_dir();
	const char *slash = "/";
	if(tmp_dir[cs_strlen(tmp_dir) - 1] == '/')
	{
		slash = "";
	}
	snprintf(dest, destlen, "%s%s%s", tmp_dir, slash, filename);
	return dest;
}

/* Return 1 if the file exists, else 0 */
bool file_exists(const char *filename)
{
	return access(filename, R_OK) == 0;
}

/* Finds a file in the system PATH.
   Returns a newly allocated string with the full path, or NULL if not found.
   Caller must free() the returned string. */
char *find_in_path(const char *filename)
{
	// If filename already has '/', don't search PATH
	if (strchr(filename, '/')) {
		return strdup(filename);
	}

	char *paths; const char *path = getenv("PATH"); if (!path || !(paths = strdup(path))) return NULL;

	char *saveptr = NULL;
	char *dir = strtok_r(paths, ":", &saveptr);
	char fullpath[PATH_MAX];
	char *result = NULL;

	while (dir) {
		if (*dir == '\0') dir = ".";
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, filename);
		if (file_exists(fullpath)) {
			result = strdup(fullpath);
			break;
		}
		dir = strtok_r(NULL, ":", &saveptr);
	}

	free(paths);
	return result;
}

/* Copies a file from srcfile to destfile. If an error occured before writing,
   -1 is returned, else -2. On success, 0 is returned.*/

int32_t file_copy(char *srcfile, char *destfile)
{
	FILE *src, *dest;
	int32_t ch;

	src = fopen(srcfile, "r");
	if(!src)
	{
		cs_log("Error opening file %s for reading (errno=%d %s)!", srcfile, errno, strerror(errno));
		return -1;
	}

	dest = fopen(destfile, "w");
	if(!dest)
	{
		cs_log("Error opening file %s for writing (errno=%d %s)!", destfile, errno, strerror(errno));
		fclose(src);
		return -1;
	}

	while(1)
	{
		ch = fgetc(src);
		if(ch == EOF)
		{
			break;
		}
		else
		{
			fputc(ch, dest);
			if(ferror(dest))
			{
				cs_log("Error while writing to file %s (errno=%d %s)!", destfile, errno, strerror(errno));
				fclose(src);
				fclose(dest);
				return -2;
			}
		}
	}
	fclose(src);
	fclose(dest);
	return (0);
}

/* Overwrites destfile with temp_file. If forceBakOverWrite = 0,
   the bakfile will not be overwritten if it exists, else it will be.*/

int32_t safe_overwrite_with_bak(char *destfile, char *temp_file, char *bakfile, int32_t forceBakOverWrite)
{
	int32_t rc;
	if(file_exists(destfile))
	{
		if(forceBakOverWrite != 0 || !file_exists(bakfile))
		{
			if(file_copy(destfile, bakfile) < 0)
			{
				cs_log("Error copying original config file %s to %s. The original config will be left untouched!", destfile, bakfile);
				if(unlink(temp_file) < 0)
				{
					cs_log("Error removing temp config file %s (errno=%d %s)!", temp_file, errno, strerror(errno));
				}
				return 1;
			}
		}
	}

	rc = file_copy(temp_file, destfile);
	if(rc < 0)
	{
		cs_log("An error occured while writing the new config file %s.", destfile);
		if(rc == -2)
		{
			cs_log("The config will be missing or only partly filled upon next startup as this is a non-recoverable error! Please restore from backup or try again.");
		}
		if(unlink(temp_file) < 0)
		{
			cs_log("Error removing temp config file %s (errno=%d %s)!", temp_file, errno, strerror(errno));
		}
		return 1;
	}

	if(unlink(temp_file) < 0)
	{
		cs_log("Error removing temp config file %s (errno=%d %s)!", temp_file, errno, strerror(errno));
	}
	return 0;
}

#ifdef MODULE_GBOX
char *get_gbox_filename(char *dest, size_t destlen, const char *filename)
{
	char *tmp_dir = get_tmp_dir();
	const char *slash = "/";

	if(cfg.gbox_tmp_dir != NULL)
	{
		if(cfg.gbox_tmp_dir[cs_strlen(cfg.gbox_tmp_dir) - 1] == '/')
		{
			slash = "";
		}
		snprintf(dest, destlen, "%s%s%s", cfg.gbox_tmp_dir, slash, filename);
	}
	else
	{
		if(tmp_dir[cs_strlen(tmp_dir) - 1] == '/') { slash = ""; }
		snprintf(dest, destlen, "%s%s%s", tmp_dir, slash, filename);
	}
	return dest;
}
#endif

#ifdef WITH_LIBCURL
static pthread_once_t ncam_curl_once = PTHREAD_ONCE_INIT;
static CURLcode ncam_curl_global_init_result = CURLE_FAILED_INIT;

static void ncam_curl_global_init_once(void)
{
	ncam_curl_global_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

int32_t ncam_curl_global_init(void)
{
	pthread_once(&ncam_curl_once, ncam_curl_global_init_once);
	return ncam_curl_global_init_result == CURLE_OK;
}

size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	if(!contents || !userp || size == 0 || nmemb == 0 || nmemb > SIZE_MAX / size)
		return 0;
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;
	if(mem->size > SIZE_MAX - realsize - 1)
	{
		cs_log("libcurl response exceeds addressable memory");
		return 0;
	}
	char *grown = realloc(mem->memory, mem->size + realsize + 1);
	if(grown == NULL)
	{
		/* out of memory! */
		cs_log("not enough memory (realloc returned NULL)");
		return 0;
	}
	mem->memory = grown;
	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}

int curl(CURL *curl_handle, char *url)
{
	if(!ncam_curl_global_init() || !curl_handle || !url) { return 0; }
	if(url[0] != 0x68 || url[1] != 0x74 || url[2] != 0x74 || url[3] != 0x70) { return 0; }

	CURLcode res;
	struct curl_slist *headers = NULL;
	char errbuf[CURL_ERROR_SIZE];

	curl_easy_setopt(curl_handle, CURLOPT_URL, url); // specify URL to get
	curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, errbuf); // provide a buffer to store errors in
	errbuf[0] = 0; // set the error buffer as empty before performing a request
  	curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0L); // Switch on full protocol/debug output while testing
  	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L); // disable progress meter, set to 0L to enable it*/

	headers = curl_slist_append(headers, "Accept: text/html");
	if(!headers)
	{
		cs_log("libcurl: failed to allocate request headers");
		return 0;
	}
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	if(strncmp(url, "https:", 6) == 0)
	{
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0); // Set the default value: strict certificate check please "enabled=1L"
	}
	curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 5L); // complete within 5 seconds
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // example.com is redirected, so we tell libcurl to follow redirection
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:72.0) Gecko/20100101 Firefox/72.0");

	res = curl_easy_perform(curl_handle); // get it!

	int32_t ok = (res == CURLE_OK);
	if(res != CURLE_OK) // check for errors
	{
		size_t len = cs_strlen(errbuf);
		cs_log_dbg(D_TRACE, "libcurl: (url) %s", url);
		if(len)
		{
			cs_log("libcurl: (%d) %s%s", res, errbuf, ((errbuf[len - 1] != '\n') ? "\n" : ""));
		}
		else
		{
			cs_log("libcurl: (%d) %s", res , curl_easy_strerror(res));
		}
		ok = 0;
	}
	curl_slist_free_all(headers);
	return ok;
}
#endif
