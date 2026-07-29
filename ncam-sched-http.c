#define MODULE_LOG_PREFIX "sched"  // Logging prefix for all scheduler messages

#include "globals.h"
#include "ncam-string.h"
#include "ncam-time.h"
#include "ncam-net.h"
#include "cscrypt/md5.h"
#include "ncam-sched-priv.h"

/*
 * NCam API HTTP Client + Digest Authentication
 *
 * Split out of ncam-sched.c: this is a self-contained HTTP client (talks
 * to NCam's own local WebIf), independent of the job state machine and
 * task.cfg parsing, and can be reasoned about / modified on its own.
 */

#ifdef WEBIF

// ============================================================================
// HTTP DIGEST AUTHENTICATION SUBSYSTEM
// ============================================================================

/*
 * HTTP Digest Authentication Extraction Helper
 * Purpose: Extracts quoted parameter values from HTTP WWW-Authenticate header
 * Process: Searches for "param="value"" pattern and copies value to output buffer
 * Security: Uses bounded copying to prevent buffer overflow
 */
static int http_extract_quoted(const char *hdr, const char *param, char *out, size_t len)
{
    char search[64];                                       // Buffer for search pattern
    snprintf(search, sizeof(search), "%s=\"", param);      // Build "param=" pattern
    
    const char *start = strstr(hdr, search);               // Locate parameter start
    if (!start) return 0;                                  // Parameter not found
    
    start += cs_strlen(search);                            // Skip to value start
    const char *end = strchr(start, '"');                  // Find closing quote
    if (!end) return 0;                                    // Malformed header
    
    size_t n = end - start;                                // Calculate value length
    if (n >= len) n = len - 1;                             // Enforce buffer limit
    memcpy(out, start, n);                                 // Copy value
    out[n] = '\0';                                         // Null-terminate
    return 1;                                              // Success
}

/*
 * MD5 Hexadecimal Conversion
 * Purpose: Converts 16-byte MD5 hash to 32-character hexadecimal string
 * Process: Iterates through hash bytes, formatting each as two hex characters
 * Output: Null-terminated hex string (requires 33-byte buffer)
 */
static void digest_md5_hex(const unsigned char *md5, char *hex)
{
    int i;
    for (i = 0; i < MD5_DIGEST_LENGTH; i++)               // Process each hash byte
        snprintf(hex + (i * 2), 3, "%02x", md5[i]);       // Convert byte to hex
}

/*
 * HTTP Digest Authentication Header Generator
 * Purpose: Creates RFC 2617 compliant Digest Authentication header
 * Algorithm: response = MD5(MD5(username:realm:password):nonce:MD5(method:uri))
 * Security: Uses server-provided nonce, supports qop=auth for replay protection
 */
static int http_digest_auth(const char *user, const char *pass, const char *uri,
                            const char *realm, const char *nonce, const char *qop,
                            const char *opaque, char *out, size_t out_sz)
{
    // Step 1: Calculate HA1 = MD5(username:realm:password)
    char ha1_in[512];
    snprintf(ha1_in, sizeof(ha1_in), "%s:%s:%s", user, realm, pass ? pass : "");
    
    unsigned char ha1_md5[MD5_DIGEST_LENGTH];
    MD5((unsigned char *)ha1_in, cs_strlen(ha1_in), ha1_md5);
    
    char ha1[33];
    digest_md5_hex(ha1_md5, ha1);
    
    // Step 2: Calculate HA2 = MD5(method:uri)
    char ha2_in[MAX_QUERY + 8];
    snprintf(ha2_in, sizeof(ha2_in), "GET:%s", uri);
    
    unsigned char ha2_md5[MD5_DIGEST_LENGTH];
    MD5((unsigned char *)ha2_in, cs_strlen(ha2_in), ha2_md5);
    
    char ha2[33];
    digest_md5_hex(ha2_md5, ha2);
    
    // Step 3: Generate client nonce (timestamp + thread id to avoid
    // collisions between concurrent requests within the same second)
    char cnonce[32];
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", 
             (unsigned long)cs_time(), (unsigned long)pthread_self());
    
    // Step 4: Calculate response based on qop presence
    char resp_in[1024];
    if (qop && qop[0]) {                                  // qop=auth enabled
        snprintf(resp_in, sizeof(resp_in), "%s:%s:00000001:%s:auth:%s", 
                ha1, nonce, cnonce, ha2);
    } else {                                              // Legacy qop-less
        snprintf(resp_in, sizeof(resp_in), "%s:%s:%s", ha1, nonce, ha2);
    }
    
    // Step 5: Final response hash
    unsigned char resp_md5[MD5_DIGEST_LENGTH];
    MD5((unsigned char *)resp_in, cs_strlen(resp_in), resp_md5);
    
    char response[33];
    digest_md5_hex(resp_md5, response);
    
    // Step 6: Format complete Authorization header
    if (qop && qop[0]) {                                  // Modern with qop
        if (opaque && opaque[0]) {                        // Include opaque if provided
            snprintf(out, out_sz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\", "
                "response=\"%s\", opaque=\"%s\"",
                user, realm, nonce, uri, cnonce, response, opaque);
        } else {                                          // Without opaque
            snprintf(out, out_sz,
                "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                "uri=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\", response=\"%s\"",
                user, realm, nonce, uri, cnonce, response);
        }
    } else {                                              // Legacy without qop
        snprintf(out, out_sz,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
            "uri=\"%s\", response=\"%s\"",
            user, realm, nonce, uri, response);
    }
    
    return 1;                                             // Success
}

/*
 * HTTP Status Code Extractor
 * Purpose: Parses HTTP response line to extract numeric status code
 * Process: Locates "HTTP/" prefix, finds first space, converts following number
 * Returns: Integer status code, 0 if malformed response
 */
static int http_get_status(const char *resp)
{
    if (!resp) return 0;                                  // Null check
    const char *http = strstr(resp, "HTTP/");            // Find protocol start
    if (!http) return 0;                                  // Not HTTP response
    const char *space = strchr(http, ' ');                // Locate status separator
    return space ? atoi(space + 1) : 0;                  // Convert to integer
}

/*
 * HTTP Request Execution
 * Purpose: Performs TCP HTTP request to local NCam web interface
 * Process: Creates socket, connects to localhost, sends GET, receives response
 * Security: Uses TCP_NODELAY for performance, configurable timeouts
 */
static int http_request(int port, const char *uri, const char *auth, 
                        char *resp, size_t resp_sz)
{
    // Step 1: Configure socket address (localhost)
    struct SOCKADDR sad;
    memset(&sad, 0, sizeof(sad));                        // Clear structure
    SIN_GET_FAMILY(sad) = DEFAULT_AF;                    // IPv4/IPv6 based on config
    set_localhost_ip(&SIN_GET_ADDR(sad));                // Set to 127.0.0.1
    SIN_GET_PORT(sad) = htons(port);                     // Convert port to network order
    
    // Step 2: Create TCP socket
    int sock = socket(DEFAULT_AF, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;                             // Socket creation failed
    
    // Step 3: Configure socket options
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // Disable Nagle
    setTCPTimeouts(sock);                                 // Apply NCam timeout defaults
    
    // Step 4: Establish connection
    if (connect(sock, (struct sockaddr *)&sad, sizeof(sad)) < 0) {
        close(sock);                                     // Cleanup on failure
        return -1;
    }
    
    // Step 5: Build HTTP GET request
    char req[32768];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "%s%s%s"
        "Connection: close\r\n\r\n",
        uri, port,
        auth ? "Authorization: " : "",                   // Conditional auth header
        auth ? auth : "",
        auth ? "\r\n" : "");
    
    // Step 6: Validate request length
    if (len < 0 || (size_t)len >= sizeof(req)) {
        close(sock);
        return -1;
    }
    
    // Step 7: Send request (loop to handle partial writes)
    size_t sent = 0;
    while (sent < (size_t)len) {
        ssize_t n = send(sock, req + sent, len - sent, 0);
        if (n <= 0) {
            close(sock);
            return -1;
        }
        sent += (size_t)n;
    }
    
    // Step 8: Receive response (loop to accumulate a response that may
    // arrive across multiple TCP segments, e.g. a large 401 Digest
    // challenge header split across reads)
    size_t total = 0;
    while (total < resp_sz - 1) {
        ssize_t n = recv(sock, resp + total, resp_sz - 1 - total, 0);
        if (n <= 0) break;                               // Error or connection closed
        total += (size_t)n;
    }
    close(sock);                                         // Always close socket
    
    if (total == 0) return -1;                           // Nothing received
    resp[total] = '\0';                                  // Null-terminate
    
    return http_get_status(resp);                        // Return HTTP status
}

/*
 * Public API Call Interface
 * Purpose: Main entry point for job steps to execute NCam API calls
 * Process: Attempts unauthenticated request, handles 401 with Digest auth if needed
 * Security: Validates credentials before use, logs all failures
 */
api_result_t ncam_api_call(const char *query)
{
    // Step 1: Input validation
    if (!query || !*query) return API_ERR;              // Empty query
    
    // Step 2: Determine HTTP port
    int port = cfg.http_port > 0 ? cfg.http_port : 8181; // Default to 8181
    
    // Step 3: Build URI from query
    char uri[MAX_QUERY];
    snprintf(uri, sizeof(uri), "/%s", query);           // Add leading slash
    
    // Step 4: Check if authentication required
    int need_auth = (cfg.http_user && cfg.http_user[0]);
    char resp[8192];
    
    // Step 5: Initial unauthenticated attempt
    int code = http_request(port, uri, NULL, resp, sizeof(resp));
    
    if (code < 0) {                                      // Network/connection error
        cs_log("API: Connection failed to 127.0.0.1:%d", port);
        return API_ERR;
    }
    
    // Step 6: Handle 401 Unauthorized with Digest auth
    if (code == 401 && need_auth) {
        char realm[256] = {0}, nonce[256] = {0};
        char qop[64] = {0}, opaque[256] = {0};
        
        // Extract authentication parameters from WWW-Authenticate header
        if (http_extract_quoted(resp, "realm", realm, sizeof(realm)) &&
            http_extract_quoted(resp, "nonce", nonce, sizeof(nonce))) {
            
            http_extract_quoted(resp, "qop", qop, sizeof(qop));
            http_extract_quoted(resp, "opaque", opaque, sizeof(opaque));
            
            // Generate Digest authentication header
            char auth[16384];
            if (!http_digest_auth(cfg.http_user, 
                                  cfg.http_pwd ? cfg.http_pwd : "",
                                  uri, realm, nonce, qop, opaque, 
                                  auth, sizeof(auth))) {
                return API_ERR;                         // Auth generation failed
            }
            
            // Retry request with authentication
            code = http_request(port, uri, auth, resp, sizeof(resp));
            if (code < 0) {                              // Auth request failed
                cs_log("API: Auth request failed");
                return API_ERR;
            }
        }
    }
    
    // Step 7: Evaluate response
    if (code >= 200 && code < 300) return API_OK;       // Success (2xx)
    
    if (code >= 400) {                                   // Client/Server error
        cs_log("API: HTTP %d for %s", code, query);
    }
    
    return API_ERR;                                      // Any other failure
}

#else // !WEBIF (Compilation without web interface)

/*
 * Stub API Function (WEBIF disabled)
 * Purpose: Provides null implementation when web interface is not compiled
 * Rationale: Allows scheduler to compile without HTTP support
 */
api_result_t ncam_api_call(const char *query) 
{ 
    (void)query;                                         // Suppress unused parameter
    return API_ERR;                                      // Always fail
}

#endif // WEBIF
