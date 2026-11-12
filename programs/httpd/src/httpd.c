#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <os/net.h>
#include <os/os.h>
#include <os/security.h>

//
// httpd: a small web server, to try the network (and wget) on one machine
//
//   httpd [-p port] [-a] [folder]
//
//   port    default 8080 (a port below 1024 needs administrator)
//   -a      answer other machines too; without it only this machine
//           (127.x.x.x) is served and anyone else is turned away
//   folder  what it serves, default the working directory
//
// Then, in another console window:
//
//   wget localhost:8080/            the folder's index.html, else a listing
//   wget localhost:8080/notes.txt   one file
//
// Over loopback (127.0.0.1) this needs no network adapter at all; another
// machine on the same network can fetch from this one's address too
// (ifconfig in the kernel console shows it).
//
// GET (and HEAD) only, one connection at a time, Connection: close. Only
// files under the folder are served: a path with ".." or a backslash in it
// is refused, and nothing under <home>/sys (the registry hives live there)
// is ever served. A client gets REQUEST_TIMEOUT_MS for its whole request,
// so a slow one can't hold the server. Names and paths in a listing are
// HTML-escaped. Press Enter in its window to stop it.
//
// Needs SEC_NET; it asks the user for that bit alone (not administrator),
// and gives up writing files, starting programs and asking for more once
// it listens.
//

#define DEFAULT_PORT        8080
#define REQUEST_MAX         2048
#define PATH_LEN            512
#define CHUNK               4096
#define REQUEST_TIMEOUT_MS  5000            // For the whole request, not per read
#define POLL_MS             250
#define LISTING_MAX         16384

static int fail(const char* what) {
    fprintf(stderr, "httpd: %s\n", what);
    return 1;
}

// What httpd keeps once it is serving: file access, its console window and
// the network. Administration, the whole screen and input go. A file's mask
// is the clearance a process needs to reach it, and files in the "user"
// group policy ask for file read and write, graphics, IPC, exec and elevate
// (POLICY_USER_PERMISSIONS), so all of those stay: without any one of them
// every request would get a 404. httpd itself never opens a file to write.
// A port below 1024 needs admin to listen on: that is dropped right after
// the listen (main).
#define HTTPD_FILE_CLEARANCE (SEC_FILE_READ | SEC_FILE_WRITE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE)
#define HTTPD_KEEPS (HTTPD_FILE_CLEARANCE | SEC_CONSOLE | SEC_NET)

static bool get_network_permission(bool need_admin) {
    if (sec_has(SEC_NET) && (!need_admin || sec_is_admin()))
        return true;
    if (!sec_has(SEC_ELEVATE)) {
        fail("no network permission, and this program can't ask for it");
        return false;
    }
    // The network bit only, unless the port needs admin too
    sec_mask_t want = SEC_NET | (need_admin ? SEC_ADMIN : 0);
    if (sec_elevate_bits(need_admin ? "Let httpd serve files on a port below 1024"
                                    : "Let httpd use the network to serve files", want) < 0) {
        fail(errno == EACCES ? "cancelled" : "can't ask for network permission");
        return false;
    }
    return true;
}

static void drop_permissions(void) {
    sec_drop(sec_get() & ~HTTPD_KEEPS);
}

// A line typed in our window (Enter): time to stop
static bool stop_requested(void) {
    int ready = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &ready) != 0 || ready <= 0)
        return false;
    char line[64];
    ssize_t taken = read(STDIN_FILENO, line, sizeof(line));    // Whatever it was
    (void)taken;
    return true;
}

static const char* content_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (dot == NULL)                                         return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".c") || !strcasecmp(dot, ".h") || !strcasecmp(dot, ".sh"))
                                                             return "text/plain";
    if (!strcasecmp(dot, ".png"))                            return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".bmp"))                            return "image/bmp";
    if (!strcasecmp(dot, ".css"))                            return "text/css";
    if (!strcasecmp(dot, ".js"))                             return "text/javascript";
    return "application/octet-stream";
}

static void send_text(int c, int status, const char* reason, const char* type, const char* body, bool head_only) {
    char header[256];
    size_t length = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     status, reason, type, (unsigned)length);
    net_send_all(c, header, (size_t)n);
    if (!head_only)
        net_send_all(c, body, length);
}

static void send_error(int c, int status, const char* reason, bool head_only) {
    char body[128];
    snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1></body></html>\n", status, reason);
    send_text(c, status, reason, "text/html", body, head_only);
}

// %XX -> byte, in place. false for a bad escape or a NUL.
static bool url_decode(char* s) {
    char* o = s;
    for (char* p = s; *p != '\0'; p++) {
        if (*p == '%') {
            int hi = p[1], lo = hi ? p[2] : 0;
            #define HEX(ch) ((ch) >= '0' && (ch) <= '9' ? (ch) - '0' : (ch) >= 'a' && (ch) <= 'f' ? (ch) - 'a' + 10 : \
                             (ch) >= 'A' && (ch) <= 'F' ? (ch) - 'A' + 10 : -1)
            int h = HEX(hi), l = HEX(lo);
            #undef HEX
            if (h < 0 || l < 0 || (h == 0 && l == 0))
                return false;
            *o++ = (char)(h * 16 + l);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return true;
}

// Text into HTML: & < > " ' as entities, control characters as '?'. Into
// out (always terminated); the length written.
static size_t html_escape(const char* in, char* out, size_t size) {
    size_t o = 0;
    for (; *in != '\0'; in++) {
        const char* rep = NULL;
        char one[2] = { *in, '\0' };
        switch (*in) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:
                if ((unsigned char)*in < 0x20 || *in == 0x7F)
                    one[0] = '?';
                rep = one;
        }
        size_t n = strlen(rep);
        if (o + n + 1 > size)
            break;
        memcpy(out + o, rep, n);
        o += n;
    }
    if (size > 0)
        out[o] = '\0';
    return o;
}

// Append to a listing; false (nothing added) when it doesn't fit whole
static bool body_add(char* body, size_t size, size_t* n, const char* text) {
    size_t length = strlen(text);
    if (*n + length + 1 > size)
        return false;
    memcpy(body + *n, text, length + 1);
    *n += length;
    return true;
}

static void send_listing(int c, const char* dir_path, const char* url_path, bool head_only) {
    static char body[LISTING_MAX];
    static const char end[] = "</ul></body></html>\n";
    char url[PATH_LEN * 6], name[256 * 6], line[sizeof(url) + 2 * sizeof(name) + 64];
    size_t n = 0;
    body[0] = '\0';
    html_escape(url_path, url, sizeof(url));
    snprintf(line, sizeof(line), "<html><body><h1>%s</h1><ul>\n", url);
    body_add(body, sizeof(body) - sizeof(end), &n, line);
    DIR* dir = opendir(dir_path);
    if (dir != NULL) {
        struct dirent* e;
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            html_escape(e->d_name, name, sizeof(name));
            snprintf(line, sizeof(line), "<li><a href=\"%s%s\">%s</a></li>\n", url, name, name);
            // Room is kept for the end: a listing too long is cut, never overrun
            if (!body_add(body, sizeof(body) - sizeof(end), &n, line))
                break;
        }
        closedir(dir);
    }
    body_add(body, sizeof(body), &n, end);
    send_text(c, 200, "OK", "text/html", body, head_only);
}

// True when `path` is <home>/sys or inside it: never served
static bool in_system_folder(const char* path) {
    char sys[PATH_LEN], full[PATH_LEN * 2];
    if (os_syspath(NULL, sys, sizeof(sys)) == NULL || os_abspath(path, full, sizeof(full)) == NULL)
        return false;
    size_t n = strlen(sys);
    return strncasecmp(full, sys, n) == 0 && (full[n] == '\0' || full[n] == '/');
}

static void send_file(int c, const char* path, bool head_only) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        send_error(c, errno == ENOENT ? 404 : 403, errno == ENOENT ? "Not Found" : "Forbidden", head_only);
        return;
    }
    struct stat st;
    long size = (stat(path, &st) == 0) ? (long)st.st_size : -1;
    char header[256];
    int h;
    if (size >= 0)
        h = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n", content_type(path), size);
    else
        h = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                     content_type(path));
    if (net_send_all(c, header, (size_t)h) < 0 || head_only) {
        fclose(f);
        return;
    }
    static char chunk[CHUNK];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
        if (net_send_all(c, chunk, r) < 0)
            break;
    fclose(f);
}

static void serve(int c, const char* root, uint32_t peer) {
    char request[REQUEST_MAX];
    size_t got = 0;
    uint64_t deadline = os_time_ms() + REQUEST_TIMEOUT_MS;
    while (got + 1 < sizeof(request)) {
        uint64_t now = os_time_ms();
        if (now >= deadline)
            return;
        ssize_t n = net_recv_wait(c, request + got, sizeof(request) - 1 - got, (int32_t)(deadline - now));
        if (n <= 0)
            return;
        got += (size_t)n;
        request[got] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL || strstr(request, "\n\n") != NULL)
            break;
    }
    request[got] = '\0';

    // "GET /path HTTP/1.1"
    char method[8], target[PATH_LEN];
    size_t ml = strcspn(request, " \r\n");
    const char* t = request + ml;
    size_t tl = (*t == ' ') ? strcspn(t + 1, " \r\n") : 0;
    if (ml == 0 || ml >= sizeof(method) || tl == 0 || tl >= sizeof(target)) {
        send_error(c, 400, "Bad Request", false);
        return;
    }
    memcpy(method, request, ml);
    method[ml] = '\0';
    memcpy(target, t + 1, tl);
    target[tl] = '\0';
    char peer_text[16];
    net_format_ip(peer, peer_text);
    // The log line: nothing from the client reaches the terminal as a
    // control character
    char shown[PATH_LEN];
    size_t k = 0;
    for (; target[k] != '\0' && k + 1 < sizeof(shown); k++)
        shown[k] = ((unsigned char)target[k] < 0x20 || target[k] == 0x7F) ? '?' : target[k];
    shown[k] = '\0';
    printf("%s %s %s\n", peer_text, method, shown);
    fflush(stdout);

    bool head_only = strcmp(method, "HEAD") == 0;
    if (!head_only && strcmp(method, "GET") != 0) {
        send_error(c, 405, "Method Not Allowed", false);
        return;
    }
    target[strcspn(target, "?#")] = '\0';
    if (target[0] != '/' || !url_decode(target) || strstr(target, "..") != NULL || strchr(target, '\\') != NULL) {
        send_error(c, 400, "Bad Request", head_only);
        return;
    }

    char path[PATH_LEN * 2];
    if ((size_t)snprintf(path, sizeof(path), "%s%s", root, target) >= sizeof(path)) {
        send_error(c, 414, "URI Too Long", head_only);
        return;
    }
    // Control characters (a decoded %0D%0A) never go back out in a header
    for (const char* p = target; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) {
            send_error(c, 400, "Bad Request", head_only);
            return;
        }
    }
    if (in_system_folder(path)) {
        send_error(c, 403, "Forbidden", head_only);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        send_error(c, 404, "Not Found", head_only);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        size_t len = strlen(target);
        if (target[len - 1] != '/') {
            // "/dir" -> "/dir/", so relative links in it work
            char header[PATH_LEN + 128];
            int n = snprintf(header, sizeof(header), "HTTP/1.1 301 Moved Permanently\r\nLocation: %s/\r\n"
                             "Content-Length: 0\r\nConnection: close\r\n\r\n", target);
            net_send_all(c, header, (size_t)n);
            return;
        }
        char index[PATH_LEN * 2 + 16];
        snprintf(index, sizeof(index), "%sindex.html", path);
        if (stat(index, &st) == 0 && !S_ISDIR(st.st_mode))
            send_file(c, index, head_only);
        else
            send_listing(c, path, target, head_only);
        return;
    }
    send_file(c, path, head_only);
}

int main(int argc, char** argv) {
    unsigned long port = DEFAULT_PORT;
    const char* folder = NULL;
    bool everyone = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            everyone = true;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char* end;
            port = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || port == 0 || port > 65535)
                return fail("bad port");
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: httpd [-p port] [-a] [folder]\n");
            return 2;
        } else {
            folder = argv[i];
        }
    }

    char root[PATH_LEN];
    if (folder != NULL)
        strlcpy(root, folder, sizeof(root));
    else if (getcwd(root, sizeof(root)) == NULL)
        return fail("can't tell the working directory");
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/')
        root[--rl] = '\0';
    if (rl == 1 && root[0] == '/')
        root[0] = '\0';                 // "/" + "/path"
    struct stat st;
    if (stat(root[0] ? root : "/", &st) != 0 || !S_ISDIR(st.st_mode))
        return fail("that folder isn't there");

    if (in_system_folder(root[0] ? root : "/"))
        return fail("the system folder (<home>/sys) is never served");

    if (!get_network_permission(port < 1024))
        return 1;

    int listener = net_listen((uint16_t)port, 4);
    drop_permissions();
    if (listener < 0) {
        fprintf(stderr, "httpd: can't listen on port %lu: %s\n", port, strerror(errno));
        return 1;
    }
    printf("serving %s on port %lu to %s: try  wget localhost:%lu/  from another console\n"
           "press Enter here to stop\n", root[0] ? root : "/", port,
           everyone ? "every machine that can reach this one" : "this machine only (-a for others)", port);
    fflush(stdout);

    while (!stop_requested()) {
        uint32_t peer = 0;
        int c = net_accept_wait(listener, &peer, NULL, POLL_MS);
        if (c < 0) {
            if (errno == ETIMEDOUT || errno == EAGAIN)
                continue;
            fprintf(stderr, "httpd: accept: %s\n", strerror(errno));
            break;
        }
        if (everyone || (peer >> 24) == 127)
            serve(c, root, peer);
        net_close(c);
    }
    net_close(listener);
    printf("stopped\n");
    return 0;
}
