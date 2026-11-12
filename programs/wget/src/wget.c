#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/net.h>
#include <os/security.h>
#include <sys/stat.h>
#include "tls.h"

//
// wget: download one file over HTTP or HTTPS
//
//   wget [-q] [-O file] <url>
//
//   url   [http[s]://]host[:port][/path], host a name or a.b.c.d
//   -O    the file to write ("-" prints it instead); default: the last part
//         of the path as typed (a redirect never changes it), or
//         index.html when the path ends in '/'. Without -O an existing file
//         is never replaced: name it with -O to overwrite it.
//   -q    say nothing but errors
//   -u    once the server answers 2xx, write "wget-url: <address>" and a
//         newline on stderr, the address after any redirects, before any of
//         the file (for a program that reads wget's output, like htmlview)
//
//   wget example.com                  -> index.html
//   wget http://10.0.0.5:8080/a.txt   -> a.txt
//   wget -O - neverssl.com            print the page
//   wget https://example.com/         over TLS
//
// HTTP/1.1 (GET, Connection: close) over the kernel's TCP; https:// over
// TLS 1.0-1.2 from the tls library (<home>/sys/lib/tls, tls.c), which
// checks the server's certificate against the Mozilla roots built into it
// and the clock. Redirects (http or https) are followed (MAX_REDIRECTS).
// A chunked body is decoded; otherwise the body runs to Content-Length, or
// to the end of the connection.
//
// The host name is looked up with net_resolve() (the kernel's DNS client).
// The network needs SEC_NET, which a program started from the desktop or
// the console doesn't have: wget asks for administrator rights once (the
// desktop's prompt), takes the network bit, and hands administrator back
// at once, as the console's "net" does.
//
// Exit status: 0 the file was saved, 1 any failure (said on stderr), 2 a
// usage mistake.
//

#define WGET_VERSION        "1.0"
#define HOST_MAX            (NET_NAME_MAX + 1)
#define PATH_MAX_URL        1024
#define HEADER_MAX          8192            // Bytes of the status line and headers
#define LINE_MAX_HTTP       512
#define BUFFER_SIZE         4096
#define MAX_REDIRECTS       5
#define RESOLVE_TIMEOUT_MS  8000
#define CONNECT_TIMEOUT_MS  10000
#define IDLE_TIMEOUT_MS     15000           // Nothing received for this long: give up
#define PROGRESS_STEP       (16 * 1024)

typedef struct {
    char     host[HOST_MAX];
    uint16_t port;
    bool     https;
    char     path[PATH_MAX_URL];            // Starts with '/'
} url_t;

typedef struct {
    int      status;                        // 200, 404 ...
    char     reason[64];
    bool     chunked;
    bool     has_length;
    uint64_t length;
    char     location[PATH_MAX_URL + HOST_MAX + 16];
} response_t;

static bool opt_quiet;
static bool opt_url;                    // -u: say the final address first
static bool progress_open;              // A "\r  N bytes" line is waiting for its end

// End the progress line, so the next message starts on its own
static void end_progress(void) {
    if (progress_open)
        fputc('\n', stderr);
    progress_open = false;
}

static const char* net_error_text(int e);

static void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* format, ...) {
    if (opt_quiet)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);
}

static int fail(const char* format, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char* format, ...) {
    va_list args;
    end_progress();
    va_start(args, format);
    fputs("wget: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    return 1;
}

static int usage(void) {
    fprintf(stderr, "usage: wget [-q] [-u] [-O file] <url>\n"
                    "  url  [http[s]://]host[:port][/path]\n"
                    "  -O   save as this file (- prints it); default: the name in the url, or index.html\n"
                    "  -q   quiet\n"
                    "  -u   say the address it came from (after redirects) first\n");
    return 2;
}

//
// URLs
//

// "http[s]://host:port/path" (scheme and everything after the host optional).
// false with a message for anything else.
static bool url_parse(const char* text, url_t* out) {
    memset(out, 0, sizeof(*out));
    out->port = 80;

    const char* p = text;
    if (strncasecmp(p, "https://", 8) == 0) {
        p += 8;
        out->https = true;
        out->port = 443;
    } else if (strncasecmp(p, "http://", 7) == 0)
        p += 7;
    else if (strstr(p, "://") != NULL) {
        fail("%s: only http:// and https:// addresses can be fetched", text);
        return false;
    }

    // host: up to ':', '/', '?', '#' or the end. user@host isn't taken.
    size_t host_length = strcspn(p, ":/?#");
    if (host_length == 0) {
        fail("%s: no host name", text);
        return false;
    }
    if (host_length >= sizeof(out->host)) {
        fail("%s: host name too long", text);
        return false;
    }
    for (size_t i = 0; i < host_length; i++) {
        char c = p[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '.' || c == '_';
        if (!ok) {
            fail("%s: '%c' can't be in a host name", text, c);
            return false;
        }
    }
    memcpy(out->host, p, host_length);
    out->host[host_length] = '\0';
    p += host_length;

    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') {
            fail("%s: bad port", text);
            return false;
        }
        unsigned long port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned long)(*p++ - '0');
            if (port > 65535)
                break;
        }
        if (port == 0 || port > 65535 || (*p != '\0' && *p != '/' && *p != '?' && *p != '#')) {
            fail("%s: bad port", text);
            return false;
        }
        out->port = (uint16_t)port;
    }

    // The path (with any ?query), never the #fragment; '/' when there's none
    size_t path_length = strcspn(p, "#");
    const char* path = p;
    bool slash = path_length > 0 && path[0] == '/';
    if (path_length + (slash ? 0 : 1) >= sizeof(out->path)) {
        fail("%s: path too long", text);
        return false;
    }
    size_t o = 0;
    if (!slash)
        out->path[o++] = '/';
    for (size_t i = 0; i < path_length; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c <= ' ' || c >= 0x7F) {
            fail("%s: spaces and control characters must be written as %%XX", text);
            return false;
        }
        out->path[o++] = (char)c;
    }
    out->path[o] = '\0';
    return true;
}

// A Location header: absolute ("http://other/x"), host-relative ("/x") or
// relative to the current path ("x"). false when it can't be followed.
static bool url_redirect(const url_t* from, const char* location, url_t* out) {
    // Goes into the next request line: no spaces or control characters
    for (const char* p = location; *p != '\0'; p++) {
        if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7F) {
            fail("the server redirects to an address with spaces or control characters in it");
            return false;
        }
    }
    if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0 ||
        strncmp(location, "//", 2) == 0) {
        // "//host/x" keeps the scheme it came from
        char full[sizeof(((response_t*)0)->location) + 8];
        snprintf(full, sizeof(full), "%s%s",
                 location[0] == '/' ? (from->https ? "https:" : "http:") : "", location);
        return url_parse(full, out);
    }
    *out = *from;
    if (location[0] == '/') {
        if (strlen(location) >= sizeof(out->path)) {
            fail("redirect path too long");
            return false;
        }
        strcpy(out->path, location);
        return true;
    }
    // Relative: replace what follows the last '/' of the current path
    char* last = strrchr(out->path, '/');
    size_t keep = last != NULL ? (size_t)(last - out->path) + 1 : 0;
    if (keep + strlen(location) >= sizeof(out->path)) {
        fail("redirect path too long");
        return false;
    }
    strcpy(out->path + keep, location);
    return true;
}

// The file name to save as: the path's last part, without ?query, else
// index.html. Never a path: '/' and '\\' can't get through, nor "." / "..".
static void url_file_name(const url_t* url, char* out, size_t size) {
    char path[PATH_MAX_URL];
    strlcpy(path, url->path, sizeof(path));
    path[strcspn(path, "?")] = '\0';
    const char* name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;

    size_t o = 0;
    for (size_t i = 0; name[i] != '\0' && o + 1 < size; i++) {
        char c = name[i];
        // %XX stays as it is: harmless in a file name
        out[o++] = (c == '\\' || c == ':' || (unsigned char)c < ' ') ? '_' : c;
    }
    out[o] = '\0';
    if (o == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        strlcpy(out, "index.html", size);
}

//
// The connection, with a little read buffer in front of it
//

typedef struct {
    int         socket;
    wget_tls_t* tls;                        // https: everything goes through it
    const char* tls_why;                    // What went wrong in TLS, if it did
    uint8_t  buffer[BUFFER_SIZE];
    size_t   start, end;                    // Unread bytes are buffer[start..end)
    bool     closed;                        // The peer closed: no more after these
} conn_t;

// Done with the connection: close_notify for https, then the socket
static void conn_close(conn_t* c) {
    wget_tls_close(c->tls);
    c->tls = NULL;
    net_close(c->socket);
}

// More bytes into the buffer. 1 got some, 0 the peer closed, -1 error
// (errno, ETIMEDOUT when the server went quiet)
static int conn_fill(conn_t* c) {
    if (c->closed)
        return 0;
    if (c->start == c->end)
        c->start = c->end = 0;
    if (c->end == sizeof(c->buffer)) {
        // Make room: move what's left to the front
        memmove(c->buffer, c->buffer + c->start, c->end - c->start);
        c->end -= c->start;
        c->start = 0;
        if (c->end == sizeof(c->buffer)) {
            errno = ERANGE;
            return -1;
        }
    }
    ssize_t n;
    if (c->tls != NULL) {
        bool timed_out = false;
        n = wget_tls_recv(c->tls, c->buffer + c->end, sizeof(c->buffer) - c->end, &timed_out, &c->tls_why);
        if (n < 0) {
            errno = timed_out ? ETIMEDOUT : EIO;
            return -1;
        }
    } else {
        n = net_recv_wait(c->socket, c->buffer + c->end, sizeof(c->buffer) - c->end, IDLE_TIMEOUT_MS);
        if (n < 0)
            return -1;
    }
    if (n == 0) {
        c->closed = true;
        return 0;
    }
    c->end += (size_t)n;
    return 1;
}

// One line ending in "\n" (a "\r" before it is dropped), cut to `size`.
// Its length, or -1 (errno; EIO: the connection ended mid-line, or the
// line is longer than the buffer).
static int conn_line(conn_t* c, char* out, size_t size) {
    while (1) {
        uint8_t* nl = memchr(c->buffer + c->start, '\n', c->end - c->start);
        if (nl != NULL) {
            size_t length = (size_t)(nl - (c->buffer + c->start));
            size_t taken = length + 1;
            if (length > 0 && c->buffer[c->start + length - 1] == '\r')
                length--;
            size_t copy = length < size - 1 ? length : size - 1;
            memcpy(out, c->buffer + c->start, copy);
            out[copy] = '\0';
            c->start += taken;
            return (int)copy;
        }
        if (c->start == 0 && c->end == sizeof(c->buffer)) {
            errno = EIO;
            return -1;
        }
        int r = conn_fill(c);
        if (r < 0)
            return -1;
        if (r == 0) {
            errno = EIO;
            return -1;
        }
    }
}

// Up to `size` bytes of the body: what's buffered first, then the socket.
// Bytes copied, 0 when the peer closed, -1 error.
static ssize_t conn_read(conn_t* c, void* out, size_t size) {
    if (c->start == c->end) {
        int r = conn_fill(c);
        if (r <= 0)
            return r;
    }
    size_t n = c->end - c->start;
    if (n > size)
        n = size;
    memcpy(out, c->buffer + c->start, n);
    c->start += n;
    return (ssize_t)n;
}

//
// HTTP
//

static const char* skip_spaces(const char* p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

// The status line and headers. false with a message.
static bool http_read_head(conn_t* c, response_t* r) {
    memset(r, 0, sizeof(*r));
    char line[LINE_MAX_HTTP];
    size_t total = 0;

    // "HTTP/1.1 200 OK"; 1xx (100 Continue) heads are skipped
    while (1) {
        int n = conn_line(c, line, sizeof(line));
        if (n < 0) {
            if (errno == EIO && c->tls_why != NULL)
                fail("%s", c->tls_why);
            else if (errno == ETIMEDOUT)
                fail("the server didn't answer");
            else
                fail("the server closed the connection without answering");
            return false;
        }
        total += (size_t)n + 2;
        if (strncmp(line, "HTTP/", 5) != 0) {
            fail("that isn't an HTTP server (it said \"%.40s\")", line);
            return false;
        }
        const char* p = strchr(line, ' ');
        if (p == NULL || p[1] < '1' || p[1] > '5' || p[2] < '0' || p[2] > '9' || p[3] < '0' || p[3] > '9') {
            fail("bad status line from the server: \"%.60s\"", line);
            return false;
        }
        r->status = (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        strlcpy(r->reason, skip_spaces(p + 4), sizeof(r->reason));
        if (r->status >= 200)
            break;
        // Skip that interim head's headers. They count against HEADER_MAX
        // too, so a server can't keep us here with 1xx heads forever.
        while ((n = conn_line(c, line, sizeof(line))) > 0) {
            total += (size_t)n + 2;
            if (total > HEADER_MAX)
                break;
        }
        if (total > HEADER_MAX) {
            fail("the server's headers are too long");
            return false;
        }
        if (n < 0) {
            fail("the connection ended in the headers");
            return false;
        }
    }

    // Headers, until the empty line
    while (1) {
        int n = conn_line(c, line, sizeof(line));
        if (n < 0) {
            fail("the connection ended in the headers");
            return false;
        }
        if (n == 0)
            break;
        total += (size_t)n + 2;
        if (total > HEADER_MAX) {
            fail("the server's headers are too long");
            return false;
        }
        char* colon = strchr(line, ':');
        if (colon == NULL)
            continue;
        *colon = '\0';
        const char* value = skip_spaces(colon + 1);
        if (strcasecmp(line, "Content-Length") == 0) {
            char* end;
            errno = 0;
            unsigned long long v = strtoull(value, &end, 10);
            if (end != value && errno == 0 && *skip_spaces(end) == '\0') {
                r->has_length = true;
                r->length = v;
            }
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (strstr(value, "chunked") != NULL || strstr(value, "Chunked") != NULL)
                r->chunked = true;
        } else if (strcasecmp(line, "Location") == 0) {
            strlcpy(r->location, value, sizeof(r->location));
        }
    }
    if (r->chunked)
        r->has_length = false;          // RFC 9112: chunked wins
    return true;
}

typedef struct {
    FILE*    file;
    uint64_t written;
    uint64_t next_report;
    uint64_t expected;                  // 0: unknown
} sink_t;

static bool sink_write(sink_t* s, const void* data, size_t length) {
    if (length == 0)
        return true;
    if (fwrite(data, 1, length, s->file) != length) {
        fail("can't write the file: %s", strerror(errno));
        return false;
    }
    s->written += length;
    if (s->written >= s->next_report) {
        if (s->expected != 0)
            say("\r  %llu of %llu bytes", (unsigned long long)s->written, (unsigned long long)s->expected);
        else
            say("\r  %llu bytes", (unsigned long long)s->written);
        s->next_report = s->written + PROGRESS_STEP;
        progress_open = !opt_quiet;
    }
    return true;
}

// The body as it is: `length` bytes, or (has_length false) to the end
static bool http_body_plain(conn_t* c, sink_t* s, bool has_length, uint64_t length) {
    uint8_t chunk[BUFFER_SIZE];
    uint64_t left = length;
    while (!has_length || left > 0) {
        size_t want = sizeof(chunk);
        if (has_length && left < want)
            want = (size_t)left;
        ssize_t n = conn_read(c, chunk, want);
        if (n < 0) {
            if (errno == EIO && c->tls_why != NULL)
                fail("%s", c->tls_why);
            else if (errno == ETIMEDOUT)
                fail("the server stopped sending");
            else
                fail("receive failed: %s", net_error_text(errno));
            return false;
        }
        if (n == 0) {
            if (has_length) {
                fail("the connection ended %llu bytes early", (unsigned long long)left);
                return false;
            }
            return true;
        }
        if (!sink_write(s, chunk, (size_t)n))
            return false;
        left -= (uint64_t)n;
    }
    return true;
}

// Transfer-Encoding: chunked: "<hex size>[;ext]\r\n<data>\r\n" ... "0\r\n"
// then trailers up to an empty line
static bool http_body_chunked(conn_t* c, sink_t* s) {
    char line[LINE_MAX_HTTP];
    while (1) {
        if (conn_line(c, line, sizeof(line)) < 0) {
            fail("%s", errno == EIO && c->tls_why != NULL ? c->tls_why : "the connection ended inside the body");
            return false;
        }
        char* end;
        errno = 0;
        unsigned long long size = strtoull(line, &end, 16);
        if (end == line || errno != 0 || (*end != '\0' && *end != ';' && *end != ' ' && *end != '\t')) {
            fail("bad chunk size from the server: \"%.20s\"", line);
            return false;
        }
        if (size == 0)
            break;
        if (!http_body_plain(c, s, true, size))
            return false;
        // The CRLF after the data
        int n = conn_line(c, line, sizeof(line));
        if (n != 0) {
            if (n < 0)
                fail("the connection ended inside the body");
            else
                fail("bad chunk from the server");
            return false;
        }
    }
    // Trailers (ignored), up to the empty line. A server that just closes
    // after "0\r\n" is let off.
    int n;
    while ((n = conn_line(c, line, sizeof(line))) > 0)
        ;
    return true;
}

//
// Network permission (as the console's "net" gets it)
//

// What wget keeps once it has the network: reading and writing files (the
// download), the console it prints on, and the network. Administration,
// the whole screen and input go. A file's mask is the clearance a process needs
// to reach it, and folders in the "user" group policy ask for graphics,
// exec and elevate as well as file read/write (POLICY_USER_PERMISSIONS), so
// those stay too: without them the folder wget saves into can't be reached
// and fopen fails with "no such file or directory".
#define WGET_FILE_CLEARANCE (SEC_FILE_READ | SEC_FILE_WRITE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE)
#define WGET_KEEPS  (WGET_FILE_CLEARANCE | SEC_CONSOLE | SEC_NET)

static bool get_network_permission(void) {
    if (!sec_has(SEC_NET)) {
        if (!sec_has(SEC_ELEVATE)) {
            fail("this program may not use the network (no network permission, and it can't ask for one)");
            return false;
        }
        // The network bit only: that is what the prompt lists and grants
        if (sec_elevate_bits("Let wget use the network to download a file", SEC_NET) < 0) {
            if (errno == EACCES)
                fail("cancelled");
            else
                fail("can't ask for network permission: %s", strerror(errno));
            return false;
        }
    }
    sec_drop(sec_get() & ~WGET_KEEPS);
    return true;
}

static const char* net_error_text(int e) {
    switch (e) {
        case ENOENT:       return "no such host";
        case ETIMEDOUT:    return "no answer (timed out)";
        case ENETDOWN:     return "not connected to a network (no address yet?)";
        case ENETUNREACH:  return "no route to that address";
        case ECONNREFUSED: return "connection refused (nothing listens on that port)";
        case ECONNRESET:   return "the server reset the connection";
        case EIO:          return "the DNS server couldn't answer";
        case EINVAL:       return "not a valid host name";
        case ENAMETOOLONG: return "host name too long";
        case EPERM:        return "no network permission";
        case ENOMEM:       return "out of memory, or too many connections open";
        default:           return strerror(e);
    }
}

//
// One request
//

// Connect, send the GET, read the head. The open connection in *c, or
// false (said why).
static bool http_request(const url_t* url, conn_t* c, response_t* r) {
    uint32_t ip;
    char ip_text[16];
    if (!net_parse_ip(url->host, &ip)) {
        say("resolving %s... ", url->host);
        if (net_resolve(url->host, &ip, RESOLVE_TIMEOUT_MS) < 0) {
            say("\n");
            fail("%s: %s", url->host, net_error_text(errno));
            return false;
        }
        net_format_ip(ip, ip_text);
        say("%s\n", ip_text);
    } else {
        net_format_ip(ip, ip_text);
    }

    say("connecting to %s:%u... ", ip_text, (unsigned)url->port);
    memset(c, 0, sizeof(*c));
    c->socket = net_connect_wait(ip, url->port, CONNECT_TIMEOUT_MS);
    if (c->socket < 0) {
        say("\n");
        fail("%s:%u: %s", ip_text, (unsigned)url->port, net_error_text(errno));
        return false;
    }
    say("connected\n");
    if (url->https) {
        const char* why = NULL;
        c->tls = wget_tls_open(c->socket, url->host, IDLE_TIMEOUT_MS, &why);
        if (c->tls == NULL) {
            fail("%s", why);
            conn_close(c);
            return false;
        }
    }

    // Host carries the port only when it isn't the scheme's own (RFC 9110)
    char request[PATH_MAX_URL + HOST_MAX + 256];
    char host[HOST_MAX + 8];
    if (url->port == (url->https ? 443 : 80))
        snprintf(host, sizeof(host), "%s", url->host);
    else
        snprintf(host, sizeof(host), "%s:%u", url->host, (unsigned)url->port);
    int length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: bork-wget/" WGET_VERSION "\r\n"
                          "Accept: */*\r\n"
                          "Accept-Encoding: identity\r\n"
                          "Connection: close\r\n"
                          "\r\n", url->path, host);
    if (length < 0 || (size_t)length >= sizeof(request)) {
        fail("the request is too long");
        conn_close(c);
        return false;
    }
    if (c->tls != NULL) {
        // The handshake happens here: the certificate is checked first
        const char* why = NULL;
        if (wget_tls_send(c->tls, request, (size_t)length, &why) < 0) {
            fail("%s: %s", url->host, why);
            conn_close(c);
            return false;
        }
    } else if (net_send_all(c->socket, request, (size_t)length) < 0) {
        fail("sending the request failed: %s", net_error_text(errno));
        conn_close(c);
        return false;
    }
    say("GET %s ... ", url->path);
    if (!http_read_head(c, r)) {
        conn_close(c);
        return false;
    }
    say("%d %s\n", r->status, r->reason);
    return true;
}

static bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

int main(int argc, char** argv) {
    const char* out_name = NULL;
    const char* address = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0)
            opt_quiet = true;
        else if (strcmp(argv[i], "-u") == 0)
            opt_url = true;
        else if (strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "-o") == 0) {
            if (++i >= argc)
                return usage();
            out_name = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            return usage();
        else if (argv[i][0] == '-' && argv[i][1] != '\0')
            return usage();
        else if (address == NULL)
            address = argv[i];
        else
            return usage();
    }
    if (address == NULL)
        return usage();

    url_t url;
    if (!url_parse(address, &url))
        return 1;
    // The file name comes from what the user typed: a server's redirect
    // must not choose which file in this folder gets written
    url_t asked = url;
    if (!get_network_permission())
        return 1;

    conn_t* c = (conn_t*)malloc(sizeof(conn_t));
    if (c == NULL)
        return fail("out of memory");
    response_t r;
    int redirects = 0;
    while (1) {
        if (!http_request(&url, c, &r)) {
            free(c);
            return 1;
        }
        if (!is_redirect(r.status) || r.location[0] == '\0')
            break;
        conn_close(c);
        if (++redirects > MAX_REDIRECTS) {
            free(c);
            return fail("too many redirects");
        }
        say("redirected to %s\n", r.location);
        url_t next;
        if (!url_redirect(&url, r.location, &next)) {
            free(c);
            return 1;
        }
        url = next;
    }

    if (r.status < 200 || r.status > 299) {
        conn_close(c);
        free(c);
        return fail("the server said %d %s; nothing saved", r.status, r.reason);
    }
    if (opt_url) {
        // Before the body: stderr goes out at once, the file only through stdout's buffer
        const char* scheme = url.https ? "https" : "http";
        if (url.port == (url.https ? 443 : 80))
            fprintf(stderr, "wget-url: %s://%s%s\n", scheme, url.host, url.path);
        else
            fprintf(stderr, "wget-url: %s://%s:%u%s\n", scheme, url.host, (unsigned)url.port, url.path);
        fflush(stderr);
    }

    // Where it goes
    char name[256];
    bool to_stdout = out_name != NULL && strcmp(out_name, "-") == 0;
    if (out_name == NULL)
        url_file_name(&asked, name, sizeof(name));
    else
        strlcpy(name, out_name, sizeof(name));
    struct stat existing;
    if (!to_stdout && out_name == NULL && stat(name, &existing) == 0) {
        conn_close(c);
        free(c);
        return fail("%s is already there; nothing saved (use -O %s to replace it)", name, name);
    }

    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.expected = r.has_length ? r.length : 0;
    sink.next_report = PROGRESS_STEP;
    if (to_stdout) {
        sink.file = stdout;
        opt_quiet = true;               // Progress would mix with the page
    } else {
        sink.file = fopen(name, "wb");
        if (sink.file == NULL) {
            int e = errno;
            conn_close(c);
            free(c);
            return fail("%s: %s", name, strerror(e));
        }
    }

    bool ok = r.chunked ? http_body_chunked(c, &sink)
                        : http_body_plain(c, &sink, r.has_length, r.length);
    conn_close(c);
    free(c);

    if (to_stdout) {
        fflush(stdout);
        return ok ? 0 : 1;
    }
    if (fclose(sink.file) != 0 && ok) {
        fail("%s: %s", name, strerror(errno));
        ok = false;
    }
    end_progress();
    if (!ok) {
        remove(name);
        fprintf(stderr, "wget: %s not saved\n", name);
        return 1;
    }
    say("saved %s (%llu bytes)\n", name, (unsigned long long)sink.written);
    return 0;
}
