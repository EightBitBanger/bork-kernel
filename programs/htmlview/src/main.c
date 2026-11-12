/*
 * htmlview: a very small HTML viewer.
 *
 *     htmlview [address]
 *
 * One window: a back button and an address box at the top, the page under
 * it, a status line at the bottom. Type an address and press Enter. The page is fetched by
 * wget (`wget -q -u -O - <address>`, started on a pipe the way the console
 * starts programs), http:// or https://, as wget does them. An address that
 * starts with '/' is a file on disk, read directly.
 *
 * What is shown: the text, with headings (h1 h2 bigger, h3-h6 bold),
 * paragraphs, line breaks, list items ("* "), <pre> blocks as they are,
 * <hr> as a line, links in the accent colour, and images. Character
 * entities and UTF-8 are turned into ASCII where there is a near match,
 * '?' otherwise. Colours: the page's background (<body bgcolor>, or
 * background in a style), text colours (style="color:...", <font color>),
 * as names, #rgb, #rrggbb or rgb(); centred text (<center>, align=center,
 * text-align: center). Scripts, styles and tables' layout are dropped. A
 * page that doesn't start with '<' is shown as plain text.
 *
 * Forms: text boxes (<input> text, search, password ...) and buttons
 * (submit, <button>) get a line each. Click a box and type; Enter in it, or
 * a click on a button, sends the form the way a browser does with GET: to
 * its action (the page itself without one) with ?name=value&... Hidden
 * fields and ticked boxes go along. POST forms can't be sent (wget only
 * GETs), and the status line says so.
 *
 * Links: a click follows one. Pointing at one shows where it goes in the
 * status line. Relative addresses are taken against the page's own, and
 * only http:// and https:// (through wget's TLS) are followed; a link
 * on a web page never opens a file on this disk.
 *
 * Images: each <img> gets its own line. Once the page is up they are
 * fetched one after another, the same way as the page (from disk for a
 * page on disk), and decoded by stb_image from the stb library
 * (<home>/sys/lib/stb, os/stblib.h): PNG, JPEG, GIF (first frame), BMP.
 * One wider than the page is shrunk to fit. Until it has come, or when it
 * can't be shown, its alt text stands in for it. Limits: IMAGES_MAX per
 * page, IMAGE_FILE_MAX bytes and IMAGE_MAX_PIXELS pixels each.
 *
 * Back: the button left of the address box, Backspace on the page, or
 * Alt+Left goes to the page shown before (HISTORY_MAX of them are kept).
 *
 * Keys: Enter in the address box loads it, F5 loads it again, Escape stops
 * a load, F6 or Ctrl+L goes to the address box, Tab to the next text box
 * on the page. On the page: Up / Down,
 * Page Up / Page Down (Space), Home / End, and the mouse wheel scroll.
 *
 * Security: the viewer needs what a user program has (files, graphics,
 * IPC, exec). The network bit (SEC_NET) it asks for once, on the first web
 * address, with the desktop's prompt naming only that permission; wget
 * then inherits it and doesn't ask again. The viewer itself never opens a
 * connection, and it never gives wget more than it holds itself.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/image.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/process.h>
#include <os/resource.h>
#include <os/security.h>
#include <os/ui.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

// From the stb library (os/stblib.h): the size of a picture, without decoding it
int stbi_info_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp);
const char* stblib_error(void);         // Why the library didn't load, "" when it did

#ifndef APP_NAME
#define APP_NAME        "htmlview"
#endif
#define APP_TITLE       "HTML viewer"

#define WIN_W           640
#define WIN_H           480
#define WIN_MIN_W       300
#define WIN_MIN_H       200

#define PAD             6
#define SCROLL_W        8
#define WHEEL_LINES     3
#define PAGE_MAX        (1024 * 1024)   // Bytes of a page kept; the rest is cut
#define TITLE_MAX       96
#define STATUS_MAX      160
#define TAG_NAME_MAX    16
#define URL_MAX         1024            // An address, as wget takes it
#define ALT_MAX         64
#define POLL_MS         30              // How often the pipe is read during a load
#define HISTORY_MAX     32              // Pages the back button can go back through
#define BACK_ICON       "ui_back"       // <home>/sys/images/ui_back (bitmaps/buttons/back.png)

#define LINKS_MAX       4000
#define IMAGES_MAX      32
#define IMAGE_FILE_MAX  (2 * 1024 * 1024)
#define IMAGE_MAX_PIXELS (2u * 1024u * 1024u)   // 8 MB decoded
#define IMAGE_KEEP_W    1024            // Kept at most this wide (shown at most the page's width)
#define IMAGE_TINY      2               // This small (spacers, counters): not shown
#define DATA_URI_MAX    (IMAGE_FILE_MAX / 3 * 4 + 64)   // src="data:image/...;base64,..."

// What wget and the viewer may hold: the "user" policy, and the network
#define LAUNCH_MASK     (SEC_FILE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE | SEC_NET)

//
// The document: its text, the link (if any) of each character, and the
// paragraphs that text is split into
//

enum { PK_TEXT, PK_H1, PK_H2, PK_H3, PK_PRE, PK_RULE, PK_IMAGE, PK_INPUT };

typedef struct {
    size_t  start;                      // Into doc_text; ends where the next one starts
    uint8_t kind;                       // PK_*
    bool    gap;                        // Some space above it
    bool    center;                     // Its lines are centred
    int16_t image;                      // PK_IMAGE: into images[]; PK_INPUT: into inputs[]
} para_t;

typedef struct {
    size_t  start, len;                 // Into doc_text
    uint8_t kind;
    bool    center;
    int16_t image;                      // PK_IMAGE: into images[]; PK_INPUT: into inputs[]
    int32_t y, h;                       // Pixels from the top of the page
} line_t;

typedef struct {
    char* url;                          // Where it goes; the href as written when !ok
    bool  ok;                           // An address the viewer can open
} link_t;

typedef enum { IMG_WAITING, IMG_LOADING, IMG_READY, IMG_FAILED } img_state_t;

typedef struct {
    char*       url;                    // NULL: nothing to fetch
    uint16_t    link;                   // The link it is in (0: none, else index + 1)
    char        alt[ALT_MAX];
    char        why[96];                // IMG_FAILED: why, for the status line
    img_state_t state;
    uint32_t*   pixels;                 // IMG_READY: w * h, 0xAARRGGBB
    int32_t     w, h;
    uint32_t*   shown;                  // Scaled to the page, on the page's background
    int32_t     sw, sh;
} image_t;

// Forms: their fields, each a line of its own (text boxes and buttons) or
// nothing to see (hidden ones)
#define FORMS_MAX       16
#define INPUTS_MAX      64
#define FIELD_MAX       64              // A field's name, a button's label, a placeholder
#define INPUT_COLS      20              // A text box's width in characters, without size=

enum { IN_TEXT, IN_SUBMIT, IN_HIDDEN };

typedef struct {
    char* action;                       // As written (NULL: none, the page itself)
    bool  post;                         // method=post: can't be sent
} form_t;

typedef struct {
    int16_t   form;                     // Into forms[]; -1: in none
    uint8_t   type;                     // IN_*
    char      name[FIELD_MAX];          // "" sends nothing
    char      label[FIELD_MAX];         // IN_SUBMIT: what the button says
    char      placeholder[FIELD_MAX];   // IN_TEXT: shown grey while it's empty
    int16_t   cols;                     // IN_TEXT: width in characters
    ui_edit_t edit;                     // The value (IN_TEXT: edited here)
} input_t;

static form_t    forms[FORMS_MAX];
static size_t    form_count;
static int16_t   cur_form = -1;         // The <form> we are in
static input_t   inputs[INPUTS_MAX];
static size_t    input_count;

// Colours of the page; 0 is "not set" (every colour has alpha 0xFF)
static gfx_color_t page_bg;

static char*     doc_text;
static uint16_t* doc_link;              // 0, or index into links[] + 1
static gfx_color_t* doc_color;          // The text colour of each character (0: the default)
static size_t    doc_len, doc_cap;
static para_t*   paras;
static size_t    para_count, para_cap;
static char      doc_title[TITLE_MAX];
static link_t*   links;
static size_t    link_count, link_cap;
static image_t   images[IMAGES_MAX];
static size_t    image_count;
static char      page_base[URL_MAX];   // The address links on the page are taken against

static line_t*   lines;
static size_t    line_count, line_cap;
static int32_t   doc_height;
static int32_t   layout_width = -1;     // The width the lines were made for (-1: make them)
static int32_t   scroll_y;

// The parser's state
static bool      in_pre;
static bool      pre_fresh;             // Just after <pre>: a newline here is dropped
static uint16_t  cur_link;              // The <a> we are in (0: none)
static uint8_t   text_kind = PK_TEXT;   // The kind text after a block closes gets

// Open elements, for what is inherited: colour and centring
#define STACK_MAX   128
typedef struct {
    char        name[TAG_NAME_MAX];
    gfx_color_t color;                  // 0: the parent's (the default at the top)
    bool        center;
} elem_t;
static elem_t    open_elems[STACK_MAX];
static size_t    open_count;

static gfx_color_t cur_color(void) {
    return open_count > 0 ? open_elems[open_count - 1].color : 0;
}

static bool cur_center(void) {
    return open_count > 0 && open_elems[open_count - 1].center;
}

//
// The window
//

static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         dirty;
static ui_edit_t    address_box;
static bool         address_focus = true;
static bool         address_drag;
static char         status[STATUS_MAX];
static bool         status_error;
static char         page_status[STATUS_MAX];   // What the status says once the images are in
static char         shown_address[URL_MAX];    // What the page on screen came from
static uint16_t     hover_link;                // Under the pointer (0: none)
static uint16_t     press_link;                // Where the button went down
static int16_t      focus_input = -1;          // The text box typed into (-1: none)
static bool         input_drag;                // Selecting in it with the mouse
static int16_t      press_button = -1;         // The form button the mouse went down on
static int          back_icon = -1;            // The back button's picture (-1: a "<" instead)
static bool         back_hover, back_press;
static char*        history[HISTORY_MAX];      // Pages shown before this one, oldest first
static size_t       history_count;
static bool         request_back;              // The next start_load is the back button's
static bool         loading_back;              // The load under way is: its page leaves history

// A load in progress: wget's end of the pipe is its stdout and stderr
static int          fetch_pipe = -1;
static int          fetch_image = -1;          // Into images[]; -1: the page itself
static char*        raw;
static size_t       raw_len, raw_cap;
static bool         raw_cut;
static char         fetch_address[URL_MAX];
static bool         net_refused;               // Said no for this page: its images don't ask again

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background     = GFX_RGB(255, 255, 255);
    theme.client.text           = GFX_RGB(20, 20, 20);
    theme.client.text_soft      = GFX_RGB(80, 80, 80);
    theme.client.text_muted     = GFX_RGB(120, 120, 120);
    theme.client.text_danger    = GFX_RGB(190, 30, 30);
    theme.client.accent         = GFX_RGB(40, 100, 200);
    theme.client.divider_soft   = GFX_RGB(200, 200, 200);
    theme.button.fill           = GFX_RGB(236, 236, 236);
    theme.button.border         = GFX_RGB(140, 140, 140);
    theme.button.border_active  = GFX_RGB(40, 100, 200);
    theme.edit.background       = GFX_COLOR_WHITE;
    theme.edit.border           = GFX_RGB(140, 140, 140);
    theme.edit.text             = GFX_RGB(20, 20, 20);
    theme.edit.cursor           = GFX_RGB(20, 20, 20);
    theme.edit.selection        = GFX_RGB(180, 205, 240);
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

// A short form of what wget (or the tls library) said went wrong, for the
// status line: "404 Not Found", "Timed out", "Untrusted certificate" ...
static void pithy(const char* why, char* out, size_t size) {
    static const struct {
        const char* has;
        const char* say;
    } table[] = {
        { "no such host", "Host not found" },
        { "timed out", "Timed out" },
        { "didn't answer", "Timed out" },
        { "stopped sending", "Timed out" },
        { "stopped answering", "Timed out" },
        { "not connected to a network", "No network" },
        { "no route", "No route to host" },
        { "connection refused", "Connection refused" },
        { "reset the connection", "Connection reset" },
        { "DNS server", "DNS failed" },
        { "no network permission", "No network permission" },
        { "may not use the network", "No network permission" },
        { "isn't from a trusted root", "Untrusted certificate" },
        { "has expired", "Certificate expired (check the clock)" },
        { "is for another name", "Certificate is for another site" },
        { "certificate", "Certificate error" },
        { "newer TLS", "Site needs TLS 1.3" },
        { "TLS version", "Unsupported TLS version" },
        { "tls library", "TLS library missing" },
        { "in common", "No common cipher" },
        { "ended the handshake", "TLS handshake failed" },
        { "tampered", "TLS error" },
        { "TLS handshake or record", "TLS error" },
        { "no clock", "No clock" },
        { "random numbers", "No randomness" },
        { "too many redirects", "Too many redirects" },
        { "redirect", "Bad redirect" },
        { "connection ended", "Connection dropped" },
        { "closed the connection", "Connection dropped" },
        { "the connection ended", "Connection dropped" },
        { "isn't an HTTP server", "Not an HTTP server" },
        { "bad status line", "Bad server reply" },
        { "bad chunk", "Bad server reply" },
        { "headers are too long", "Bad server reply" },
        { "host name", "Bad address" },
        { "bad port", "Bad address" },
        { "only http", "Unsupported address" },
        { "out of memory", "Out of memory" },
        { "cancelled", "Cancelled" },
    };
    while (*why == ' ')
        why++;
    if (strncmp(why, "wget: ", 6) == 0)
        why += 6;
    // "the server said 404 Not Found; nothing saved": the status itself
    const char* said = strstr(why, "the server said ");
    if (said != NULL) {
        said += 16;
        size_t n = strcspn(said, ";");
        snprintf(out, size, "%.*s", (int)(n < 40 ? n : 40), said);
        return;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strstr(why, table[i].has) != NULL) {
            strlcpy(out, table[i].say, size);
            return;
        }
    }
    // Something else: as it is, capitalised, cut short
    snprintf(out, size, "%.48s", why);
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
}

static void set_status(bool error, const char* text) {
    strlcpy(status, text, sizeof(status));
    status_error = error;
    dirty = true;
}

//
// Building the document
//

static void doc_clear(void) {
    for (size_t i = 0; i < link_count; i++)
        free(links[i].url);
    for (size_t i = 0; i < image_count; i++) {
        free(images[i].url);
        free(images[i].pixels);
        free(images[i].shown);
    }
    memset(images, 0, sizeof(images));
    image_count = 0;
    for (size_t i = 0; i < form_count; i++)
        free(forms[i].action);
    memset(forms, 0, sizeof(forms));
    form_count = 0;
    cur_form = -1;
    input_count = 0;
    focus_input = press_button = -1;
    input_drag = false;
    open_count = 0;
    page_bg = 0;
    link_count = 0;
    doc_len = 0;
    para_count = 0;
    line_count = 0;
    doc_height = 0;
    doc_title[0] = '\0';
    layout_width = -1;
    scroll_y = 0;
    hover_link = press_link = 0;
    in_pre = pre_fresh = false;
    cur_link = 0;
    text_kind = PK_TEXT;
}

static bool grow(void** p, size_t* cap, size_t need, size_t item) {
    if (need <= *cap)
        return true;
    size_t n = *cap ? *cap : 256;
    while (n < need)
        n *= 2;
    void* q = realloc(*p, n * item);
    if (q == NULL)
        return false;
    *p = q;
    *cap = n;
    return true;
}

static size_t para_len(void) {
    return para_count == 0 ? 0 : doc_len - paras[para_count - 1].start;
}

// Start a paragraph of `kind`. An empty one just open is reused.
static void new_para(uint8_t kind, bool gap) {
    if (para_count > 0 && para_len() == 0) {
        para_t* p = &paras[para_count - 1];
        p->kind = kind;
        p->gap = p->gap || gap;
        p->center = cur_center();
        return;
    }
    // The space a paragraph ended with goes
    if (para_count > 0 && doc_len > paras[para_count - 1].start && doc_text[doc_len - 1] == ' ')
        doc_len--;
    size_t old_cap = para_cap;
    if (!grow((void**)&paras, &para_cap, para_count + 1, sizeof(para_t))) {
        para_cap = old_cap;
        return;                         // Out of memory: the text runs on in this one
    }
    paras[para_count].start = doc_len;
    paras[para_count].kind = kind;
    paras[para_count].gap = gap;
    paras[para_count].center = cur_center();
    paras[para_count].image = -1;
    para_count++;
}

static void put_raw(char c) {
    if (para_count == 0)
        new_para(PK_TEXT, false);
    if (para_count == 0)
        return;
    size_t cap = doc_cap;
    if (!grow((void**)&doc_text, &cap, doc_len + 1, 1))
        return;
    size_t cap2 = doc_cap;
    if (!grow((void**)&doc_link, &cap2, doc_len + 1, sizeof(uint16_t)))
        return;
    size_t cap3 = doc_cap;
    if (!grow((void**)&doc_color, &cap3, doc_len + 1, sizeof(gfx_color_t)))
        return;
    doc_cap = cap < cap2 ? cap : cap2;
    if (cap3 < doc_cap)
        doc_cap = cap3;
    doc_text[doc_len] = c;
    doc_link[doc_len] = cur_link;
    doc_color[doc_len] = cur_color();
    doc_len++;
}

// One character of page text: spaces fold into one outside <pre>
static void text_char(char c) {
    if (in_pre) {
        if (c == '\r')
            return;
        if (pre_fresh) {
            pre_fresh = false;
            if (c == '\n')
                return;
        }
        if (c == '\t') {
            for (int i = 0; i < 4; i++)
                put_raw(' ');
            return;
        }
        if (c == '\n' || (unsigned char)c >= ' ')
            put_raw(c);
        return;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
        if (para_len() > 0 && doc_text[doc_len - 1] != ' ')
            put_raw(' ');
        return;
    }
    if ((unsigned char)c >= ' ')
        put_raw(c);
}

// A Unicode character as the ASCII drawn for it
static const char* cp_ascii(uint32_t cp, char* one) {
    if (cp == '\n' || cp == '\t' || cp == '\r' || (cp >= ' ' && cp < 0x7F)) {
        one[0] = (char)cp;
        one[1] = '\0';
        return one;
    }
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F)
        return " ";
    // Latin-1 letters: without their accent
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) {
        one[0] = latin1[cp - 0xC0];
        one[1] = '\0';
        return one;
    }
    switch (cp) {
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD:
            return "";
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0xB4:
            return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033:
            return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            return "-";
        case 0x2026: return "...";
        case 0x2022: case 0xB7: case 0x25CF: return "*";
        case 0xA9:   return "(c)";
        case 0xAE:   return "(R)";
        case 0x2122: return "(TM)";
        case 0xAB:   return "<<";
        case 0xBB:   return ">>";
        case 0x2039: return "<";
        case 0x203A: return ">";
        case 0xD7:   return "x";
        case 0x2192: return "->";
        case 0x2190: return "<-";
        case 0xB0:   return "deg";
        case 0x20AC: return "EUR";
        // Letters that look like Latin ones (used for effect, as in "D\u0245RKW\u018eB")
        case 0x245: case 0x39B: case 0x1D27: return "A";
        case 0x18E: case 0x2203: return "E";
        case 0x250: return "a";
        case 0x1DD: case 0x259: return "e";
        case 0x186: return "C";
        case 0x2C6F: return "A";
        case 0xA3:   return "GBP";
        default:     return "?";
    }
}

typedef void (*put_fn)(char c);

static void put_cp(uint32_t cp, put_fn put) {
    char one[2];
    for (const char* s = cp_ascii(cp, one); *s != '\0'; s++)
        put(*s);
}

// One UTF-8 character at *p (p < e): its code point; *p moves past it.
// A bad sequence is one byte, U+FFFD.
static uint32_t utf8_next(const char** p, const char* e) {
    const unsigned char* s = (const unsigned char*)*p;
    unsigned char c = s[0];
    int more;
    uint32_t cp;
    if (c < 0x80) {
        *p += 1;
        return c;
    } else if (c >= 0xC2 && c <= 0xDF) {
        more = 1; cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        more = 2; cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        more = 3; cp = c & 0x07;
    } else {
        *p += 1;
        return 0xFFFD;
    }
    if ((const char*)s + 1 + more > e) {    // Not all of it is there
        *p += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= more; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += 1 + more;
    return cp;
}

static const struct {
    const char* name;
    uint32_t    cp;
} entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "bull", 0x2022 },
    { "middot", 0xB7 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "times", 0xD7 },
    { "rarr", 0x2192 }, { "larr", 0x2190 }, { "deg", 0xB0 }, { "euro", 0x20AC },
    { "pound", 0xA3 }, { "shy", 0xAD }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "thinsp", 0x2009 },
};

// "&name;" or "&#123;" / "&#x7B;" at p (*p == '&'): its code point and
// length, or 0 when it isn't one (the '&' is then just a character)
static size_t entity_at(const char* p, const char* e, uint32_t* cp) {
    const char* q = p + 1;
    char name[12];
    size_t n = 0;
    if (q < e && *q == '#') {
        q++;
        int base = 10;
        if (q < e && (*q == 'x' || *q == 'X')) {
            base = 16;
            q++;
        }
        uint32_t v = 0;
        size_t digits = 0;
        while (q < e && digits < 8 && isxdigit((unsigned char)*q) && (base == 16 || isdigit((unsigned char)*q))) {
            v = v * (uint32_t)base + (uint32_t)(isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
            q++;
            digits++;
        }
        if (digits == 0 || q >= e || *q != ';')
            return 0;
        *cp = (v == 0 || v > 0x10FFFF) ? 0xFFFD : v;
        return (size_t)(q + 1 - p);
    }
    while (q < e && n < sizeof(name) - 1 && isalnum((unsigned char)*q))
        name[n++] = *q++;
    name[n] = '\0';
    if (n == 0 || q >= e || *q != ';')
        return 0;
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        if (strcmp(entities[i].name, name) == 0) {
            *cp = entities[i].cp;
            return (size_t)(q + 1 - p);
        }
    }
    // "&eacute;", "&Uuml;" ...: the letter without its accent
    static const char* const accents[] = { "acute", "grave", "uml", "circ", "tilde", "cedil", "ring", NULL };
    for (size_t i = 0; accents[i] != NULL; i++) {
        size_t a = strlen(accents[i]);
        if (n == a + 1 && isalpha((unsigned char)name[0]) && strcmp(name + 1, accents[i]) == 0) {
            *cp = (unsigned char)name[0];
            return (size_t)(q + 1 - p);
        }
    }
    return 0;
}

// Text between tags (or all of a plain text page): entities (when
// `entities_on`) and UTF-8 turned into characters for `put`
static void decode_text(const char* p, const char* e, bool entities_on, put_fn put) {
    while (p < e) {
        if (*p == '&' && entities_on) {
            uint32_t cp;
            size_t n = entity_at(p, e, &cp);
            if (n > 0) {
                put_cp(cp, put);
                p += n;
                continue;
            }
        }
        put_cp(utf8_next(&p, e), put);
    }
}

// The <title>, spaces folded
static size_t title_len;
static void title_char(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        if (title_len == 0 || doc_title[title_len - 1] == ' ')
            return;
        c = ' ';
    }
    if ((unsigned char)c < ' ' || title_len + 1 >= sizeof(doc_title))
        return;
    doc_title[title_len++] = c;
    doc_title[title_len] = '\0';
}

// `needle` (lower case) in [p, e), ignoring case
static const char* find_ci(const char* p, const char* e, const char* needle) {
    size_t n = strlen(needle);
    for (; p + n <= e; p++) {
        size_t i = 0;
        while (i < n && tolower((unsigned char)p[i]) == needle[i])
            i++;
        if (i == n)
            return p;
    }
    return NULL;
}

static bool name_is(const char* name, const char* const* list) {
    for (; *list != NULL; list++)
        if (strcmp(name, *list) == 0)
            return true;
    return false;
}

//
// Addresses
//

// Length of "http://host:port" at the start of `url` (0: none)
static size_t origin_len(const char* url) {
    const char* p = strstr(url, "://");
    if (p == NULL)
        return 0;
    p += 3;
    p += strcspn(p, "/?#");
    return (size_t)(p - url);
}

// "." and ".." taken out of a path that starts with '/' (its ?query kept)
static void remove_dots(char* path, size_t size) {
    char query[URL_MAX];
    char* q = strchr(path, '?');
    strlcpy(query, q != NULL ? q : "", sizeof(query));
    if (q != NULL)
        *q = '\0';

    char out[URL_MAX];
    size_t o = 0;
    const char* p = path;
    while (*p == '/') {
        const char* seg = p + 1;
        size_t n = strcspn(seg, "/");
        bool last = seg[n] == '\0';
        if (n == 1 && seg[0] == '.') {
            if (last)
                out[o++] = '/';
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[--o] != '/')
                ;
            if (last)
                out[o++] = '/';
        } else {
            out[o++] = '/';
            memcpy(out + o, seg, n);
            o += n;
        }
        p = seg + n;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
    snprintf(path, size, "%s%s", out, query);
}

// `href` (as written on the page, entities decoded) taken against `base`:
// the address to open, in out. false for one the viewer can't open (other
// schemes, "#here" on the same page, too long). A link from a web page
// stays on the web: only a page read from disk has links on disk.
static bool resolve_url(const char* base, const char* href, char* out, size_t size) {
    // Spaces around it go; spaces, controls and non-ASCII inside are %XX
    char h[URL_MAX];
    size_t n = 0;
    while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r')
        href++;
    for (; *href != '\0'; href++) {
        unsigned char c = (unsigned char)*href;
        if (c == '\n' || c == '\r' || c == '\t')
            continue;
        if (n + 4 >= sizeof(h))
            return false;
        if (c <= ' ' || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\') {
            static const char hex[] = "0123456789ABCDEF";
            h[n++] = '%';
            h[n++] = hex[c >> 4];
            h[n++] = hex[c & 15];
        } else {
            h[n++] = (char)c;
        }
    }
    while (n >= 3 && memcmp(h + n - 3, "%20", 3) == 0)
        n -= 3;
    h[n] = '\0';
    h[strcspn(h, "#")] = '\0';          // The fragment is for the browser
    if (h[0] == '\0')
        return false;                   // "#here", or nothing

    char result[URL_MAX * 2];
    size_t scheme = strspn(h, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
    bool local = base[0] == '/';
    if (scheme > 0 && h[scheme] == ':' && isalpha((unsigned char)h[0])) {
        if (strncasecmp(h, "http://", 7) != 0 && strncasecmp(h, "https://", 8) != 0)
            return false;               // mailto:, javascript:, data:, file:, ...
        strlcpy(result, h, sizeof(result));
    } else if (h[0] == '/' && h[1] == '/') {
        // "//host/x": the page's own scheme
        snprintf(result, sizeof(result), "%s%s", strncasecmp(base, "https:", 6) == 0 ? "https:" : "http:", h);
    } else {
        size_t olen = local ? 0 : origin_len(base);
        if (!local && olen == 0)
            return false;
        // The base's path: "/" when it has none
        char path[URL_MAX * 2];
        const char* bpath = base + olen;
        size_t bquery = strcspn(bpath, "?#");
        if (h[0] == '/') {
            strlcpy(path, h, sizeof(path));
        } else if (h[0] == '?') {
            snprintf(path, sizeof(path), "%.*s%s", (int)bquery, bquery > 0 ? bpath : "/", h);
        } else {
            // Up to and with the last '/' of the base's path
            size_t keep = 0;
            for (size_t i = 0; i < bquery; i++)
                if (bpath[i] == '/')
                    keep = i + 1;
            if (keep == 0)
                snprintf(path, sizeof(path), "/%s", h);
            else
                snprintf(path, sizeof(path), "%.*s%s", (int)keep, bpath, h);
        }
        if (path[0] != '/' || strlen(path) >= URL_MAX)
            return false;
        remove_dots(path, sizeof(path));
        snprintf(result, sizeof(result), "%.*s%s", (int)olen, base, path);
    }
    if (strlen(result) >= size || strlen(result) >= URL_MAX)
        return false;
    strlcpy(out, result, size);
    return true;
}

//
// Attributes
//

// Where decode_attr's characters go
static char*  attr_out;
static size_t attr_len, attr_size;
static void attr_char(char c) {
    if (attr_len + 1 < attr_size) {
        attr_out[attr_len++] = c;
        attr_out[attr_len] = '\0';
    }
}

// The value of attribute `name` (lower case) in a tag's [p, e), entities
// decoded: as bytes when `raw_bytes` (an address), else as display text
static bool get_attr(const char* p, const char* e, const char* name, bool raw_bytes, char* out, size_t size) {
    size_t want = strlen(name);
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == '/'))
            p++;
        const char* ns = p;
        while (p < e && !isspace((unsigned char)*p) && *p != '=' && *p != '/')
            p++;
        if (p == ns) {
            if (p < e)
                p++;
            continue;
        }
        size_t nl = (size_t)(p - ns);
        while (p < e && isspace((unsigned char)*p))
            p++;
        const char* vs = p;
        const char* ve = p;
        if (p < e && *p == '=') {
            p++;
            while (p < e && isspace((unsigned char)*p))
                p++;
            if (p < e && (*p == '"' || *p == '\'')) {
                char quote = *p++;
                vs = p;
                while (p < e && *p != quote)
                    p++;
                ve = p;
                if (p < e)
                    p++;
            } else {
                vs = p;
                while (p < e && !isspace((unsigned char)*p))
                    p++;
                ve = p;
            }
        }
        if (nl == want && strncasecmp(ns, name, want) == 0) {
            attr_out = out;
            attr_len = 0;
            attr_size = size;
            out[0] = '\0';
            if (!raw_bytes) {
                decode_text(vs, ve, true, attr_char);
            } else {
                while (vs < ve) {
                    uint32_t cp;
                    size_t n = *vs == '&' ? entity_at(vs, ve, &cp) : 0;
                    if (n > 0 && cp < 0x80) {
                        attr_char((char)cp);
                        vs += n;
                    } else {
                        attr_char(*vs++);
                    }
                }
            }
            return true;
        }
    }
    return false;
}

//
// Colours and style=""
//

static const struct {
    const char* name;
    uint32_t    rgb;
} color_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "lime", 0x00FF00 },
    { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF },
    { "aqua", 0x00FFFF }, { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "olive", 0x808000 },
    { "teal", 0x008080 }, { "navy", 0x000080 }, { "purple", 0x800080 }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "darkgray", 0xA9A9A9 },
    { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC },
    { "crimson", 0xDC143C }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 },
    { "tomato", 0xFF6347 }, { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "lightblue", 0xADD8E6 },
    { "darkorange", 0xFF8C00 }, { "orangered", 0xFF4500 }, { "royalblue", 0x4169E1 },
    { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F }, { "lightyellow", 0xFFFFE0 },
    { "lightgreen", 0x90EE90 }, { "darkviolet", 0x9400D3 }, { "snow", 0xFFFAFA },
};

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A colour as HTML and CSS write it: a name, #rgb, #rrggbb (#rgba and
// #rrggbbaa without the alpha), rgb(r, g, b), or bare hex as old bgcolor=
// attributes have it. false for anything else (transparent, inherit ...).
static bool parse_color(const char* v, gfx_color_t* out) {
    while (*v == ' ' || *v == '\t')
        v++;
    char t[48];
    size_t n = 0;
    while (v[n] != '\0' && v[n] != ' ' && v[n] != '\t' && v[n] != ';' && n + 1 < sizeof(t)) {
        t[n] = (char)tolower((unsigned char)v[n]);
        n++;
    }
    t[n] = '\0';
    if (strncmp(t, "rgb", 3) == 0) {
        const char* p = strchr(v, '(');
        if (p == NULL)
            return false;
        int c[3];
        for (int i = 0; i < 3; i++) {
            p++;
            while (*p == ' ')
                p++;
            if (!isdigit((unsigned char)*p))
                return false;
            long x = strtol(p, (char**)&p, 10);
            if (*p == '.')                          // 12.5: the whole part
                while (isdigit((unsigned char)*++p)) {}
            if (*p == '%') {
                x = x * 255 / 100;
                p++;
            }
            c[i] = x < 0 ? 0 : x > 255 ? 255 : (int)x;
            while (*p == ' ')
                p++;
            if (i < 2 && *p != ',' && *p != ' ')
                return false;
            if (*p != ',')
                p--;
        }
        *out = GFX_RGB(c[0], c[1], c[2]);
        return true;
    }
    for (size_t i = 0; i < sizeof(color_names) / sizeof(color_names[0]); i++) {
        if (strcmp(t, color_names[i].name) == 0) {
            uint32_t rgb = color_names[i].rgb;
            *out = GFX_RGB(rgb >> 16, rgb >> 8, rgb);
            return true;
        }
    }
    const char* h = t[0] == '#' ? t + 1 : t;
    size_t hl = strlen(h);
    for (size_t i = 0; i < hl; i++)
        if (hex_digit(h[i]) < 0)
            return false;
    if (hl == 3 || hl == 4) {
        int r = hex_digit(h[0]), g = hex_digit(h[1]), b = hex_digit(h[2]);
        *out = GFX_RGB(r * 17, g * 17, b * 17);
        return true;
    }
    if (hl == 6 || hl == 8) {
        *out = GFX_RGB(hex_digit(h[0]) * 16 + hex_digit(h[1]), hex_digit(h[2]) * 16 + hex_digit(h[3]),
                       hex_digit(h[4]) * 16 + hex_digit(h[5]));
        return true;
    }
    return false;
}

// The value of property `prop` (lower case) in a style attribute's text,
// "!important" left off. false when it isn't there.
static bool css_value(const char* style, const char* prop, char* out, size_t size) {
    size_t want = strlen(prop);
    const char* p = style;
    bool found = false;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';')
            p++;
        const char* name = p;
        while (*p != '\0' && *p != ':' && *p != ';')
            p++;
        const char* name_end = p;
        while (name_end > name && isspace((unsigned char)name_end[-1]))
            name_end--;
        if (*p != ':') {
            continue;
        }
        p++;
        const char* value = p;
        while (*p != '\0' && *p != ';')
            p++;
        if ((size_t)(name_end - name) == want && strncasecmp(name, prop, want) == 0) {
            while (value < p && isspace((unsigned char)*value))
                value++;
            size_t len = (size_t)(p - value);
            if (len >= size)
                len = size - 1;
            memcpy(out, value, len);
            out[len] = '\0';
            char* imp = strstr(out, "!");
            if (imp != NULL)
                *imp = '\0';
            len = strlen(out);
            while (len > 0 && isspace((unsigned char)out[len - 1]))
                out[--len] = '\0';
            found = true;               // The last one counts, as in CSS
        }
    }
    return found;
}

// The first colour among the words of a "background:" shorthand
static bool background_color(const char* v, gfx_color_t* out) {
    const char* p = v;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',')
            p++;
        if (*p == '\0')
            break;
        if (parse_color(p, out))
            return true;
        // Past this word (and a ( ... ) in it, like url(...))
        int depth = 0;
        while (*p != '\0' && (depth > 0 || (*p != ' ' && *p != ','))) {
            if (*p == '(') depth++;
            if (*p == ')' && depth > 0) depth--;
            p++;
        }
    }
    return false;
}

// A background colour set on an element: bgcolor=, or background(-color)
// in its style
static bool element_background(const char* attrs, const char* attrs_end, const char* style, gfx_color_t* out) {
    char v[128];
    if (style[0] != '\0') {
        if (css_value(style, "background-color", v, sizeof(v)) && parse_color(v, out))
            return true;
        if (css_value(style, "background", v, sizeof(v)) && background_color(v, out))
            return true;
    }
    return get_attr(attrs, attrs_end, "bgcolor", true, v, sizeof(v)) && parse_color(v, out);
}

// Elements with no end tag, never on the stack
static const char* const void_tags[] = {
    "br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "param",
    "source", "track", "wbr", "keygen", "frame", "isindex", NULL
};
// One of these opening while the same is open closes that one (<p>a<p>b)
static const char* const self_closing_tags[] = {
    "p", "li", "dt", "dd", "tr", "td", "th", "option", NULL
};

static bool name_is(const char* name, const char* const* list);

// An element opens: what it gives its content
static void elem_open(const char* name, const char* attrs, const char* attrs_end) {
    if (name_is(name, self_closing_tags) && open_count > 0 && strcmp(open_elems[open_count - 1].name, name) == 0)
        open_count--;
    if (open_count >= STACK_MAX)
        return;
    elem_t el;
    memset(&el, 0, sizeof(el));
    strlcpy(el.name, name, sizeof(el.name));
    el.color = cur_color();
    el.center = cur_center();

    char style[512] = "";
    get_attr(attrs, attrs_end, "style", true, style, sizeof(style));
    char v[128];
    gfx_color_t c;
    if (css_value(style, "color", v, sizeof(v)) && parse_color(v, &c))
        el.color = c;
    else if (strcmp(name, "font") == 0 && get_attr(attrs, attrs_end, "color", true, v, sizeof(v)) && parse_color(v, &c))
        el.color = c;
    else if (strcmp(name, "body") == 0 && get_attr(attrs, attrs_end, "text", true, v, sizeof(v)) && parse_color(v, &c))
        el.color = c;

    if (strcmp(name, "center") == 0)
        el.center = true;
    if (get_attr(attrs, attrs_end, "align", true, v, sizeof(v)))
        el.center = strcasecmp(v, "center") == 0 || strcasecmp(v, "middle") == 0;
    if (css_value(style, "text-align", v, sizeof(v)))
        el.center = strncasecmp(v, "center", 6) == 0;

    // The page's background is the body's (or the html element's)
    if ((strcmp(name, "body") == 0 || (strcmp(name, "html") == 0 && page_bg == 0)) &&
        element_background(attrs, attrs_end, style, &c))
        page_bg = c;

    open_elems[open_count++] = el;
}

// An end tag: back to before the element it closes (a stray one does nothing)
static void elem_close(const char* name) {
    for (size_t i = open_count; i > 0; i--) {
        if (strcmp(open_elems[i - 1].name, name) == 0) {
            open_count = i - 1;
            return;
        }
    }
}

//
// Forms
//

static void add_form(const char* attrs, const char* attrs_end) {
    cur_form = -1;
    if (form_count >= FORMS_MAX)
        return;
    form_t* f = &forms[form_count];
    memset(f, 0, sizeof(*f));
    char v[URL_MAX];
    if (get_attr(attrs, attrs_end, "action", true, v, sizeof(v)))
        f->action = strdup(v);
    if (get_attr(attrs, attrs_end, "method", true, v, sizeof(v)))
        f->post = strcasecmp(v, "post") == 0;
    cur_form = (int16_t)form_count++;
}

// A field: a line of its own for a box or a button
static void add_input(uint8_t type, const char* attrs, const char* attrs_end, const char* label, bool secret) {
    if (input_count >= INPUTS_MAX)
        return;
    input_t* in = &inputs[input_count];
    memset(in, 0, sizeof(*in));
    in->form = cur_form;
    in->type = type;
    char value[UI_EDIT_MAX] = "";
    get_attr(attrs, attrs_end, "name", true, in->name, sizeof(in->name));
    get_attr(attrs, attrs_end, "value", type == IN_HIDDEN, value, sizeof(value));
    get_attr(attrs, attrs_end, "placeholder", false, in->placeholder, sizeof(in->placeholder));
    if (in->placeholder[0] == '\0')
        get_attr(attrs, attrs_end, "aria-label", false, in->placeholder, sizeof(in->placeholder));
    ui_edit_init(&in->edit, value, UI_EDIT_MAX - 1, secret ? UI_EDIT_SECRET : 0);
    in->edit.anchor = in->edit.cursor;  // Not all selected: a click puts the cursor there
    in->cols = INPUT_COLS;
    char size[16];
    if (get_attr(attrs, attrs_end, "size", true, size, sizeof(size))) {
        int n = atoi(size);
        if (n > 0)
            in->cols = (int16_t)(n > 80 ? 80 : n < 2 ? 2 : n);
    }
    if (type == IN_SUBMIT)
        strlcpy(in->label, label != NULL && label[0] != '\0' ? label : value[0] != '\0' ? value : "Submit",
                sizeof(in->label));
    if (type == IN_HIDDEN) {
        input_count++;
        return;
    }
    new_para(PK_INPUT, false);
    if (para_count == 0 || paras[para_count - 1].kind != PK_INPUT)
        return;
    paras[para_count - 1].image = (int16_t)input_count;
    put_raw('#');                       // Keeps it from being reused; not drawn
    input_count++;
    new_para(text_kind, false);
}

static void handle_input(const char* attrs, const char* attrs_end) {
    char type[24] = "text";
    get_attr(attrs, attrs_end, "type", true, type, sizeof(type));
    for (char* t = type; *t; t++)
        *t = (char)tolower((unsigned char)*t);
    if (strcmp(type, "hidden") == 0) {
        add_input(IN_HIDDEN, attrs, attrs_end, NULL, false);
    } else if (strcmp(type, "submit") == 0 || strcmp(type, "image") == 0) {
        char alt[FIELD_MAX] = "";
        if (type[0] == 'i')
            get_attr(attrs, attrs_end, "alt", false, alt, sizeof(alt));
        add_input(IN_SUBMIT, attrs, attrs_end, alt, false);
    } else if (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) {
        // Not drawn; one that starts ticked is sent as it is ("on" without a value)
        char dummy[4];
        if (get_attr(attrs, attrs_end, "checked", true, dummy, sizeof(dummy))) {
            size_t before = input_count;
            add_input(IN_HIDDEN, attrs, attrs_end, NULL, false);
            if (input_count > before && inputs[before].edit.len == 0)
                ui_edit_init(&inputs[before].edit, "on", UI_EDIT_MAX - 1, 0);
        }
    } else if (strcmp(type, "button") == 0 || strcmp(type, "reset") == 0 || strcmp(type, "file") == 0 ||
               strcmp(type, "range") == 0 || strcmp(type, "color") == 0) {
        // Nothing the viewer can do with these
    } else {
        // text, search, email, url, tel, number, password, date ...: a box
        add_input(IN_TEXT, attrs, attrs_end, NULL, strcmp(type, "password") == 0);
    }
}

//
// Tags
//

// Blocks that start a new line, and those that also leave a gap
static const char* const block_tags[] = {
    "div", "section", "article", "header", "footer", "nav", "main", "aside",
    "center", "address", "fieldset", "figure", "figcaption", "tr", "caption", "body",
    "details", "summary", NULL
};
static const char* const gap_tags[] = {
    "p", "ul", "ol", "dl", "blockquote", "table", NULL
};
// Their content is never shown
static const char* const skip_tags[] = {
    "script", "style", "template", "svg", "math", "object", "iframe", "select", NULL
};

// <a href>: a new link; its index + 1, or 0 when there's no more room
static uint16_t add_link(const char* href) {
    if (link_count >= LINKS_MAX)
        return 0;
    size_t old = link_cap;
    if (!grow((void**)&links, &link_cap, link_count + 1, sizeof(link_t))) {
        link_cap = old;
        return 0;
    }
    char url[URL_MAX];
    bool ok = resolve_url(page_base, href, url, sizeof(url));
    char* copy = strdup(ok ? url : href);
    if (copy == NULL)
        return 0;
    links[link_count].url = copy;
    links[link_count].ok = ok;
    link_count++;
    return (uint16_t)link_count;
}

// <img>: a paragraph of its own, holding it
static void add_image(const char* attrs, const char* attrs_end) {
    if (image_count >= IMAGES_MAX)
        return;
    image_t* img = &images[image_count];
    memset(img, 0, sizeof(*img));
    char url[URL_MAX];
    get_attr(attrs, attrs_end, "alt", false, img->alt, sizeof(img->alt));
    // src may be a whole picture ("data:image/png;base64,..."): room for one
    char* src = (char*)malloc(DATA_URI_MAX);
    if (src != NULL && get_attr(attrs, attrs_end, "src", true, src, DATA_URI_MAX)) {
        if (strncasecmp(src, "data:", 5) == 0)
            img->url = strdup(src);     // Decoded when its turn comes (next_image)
        else if (resolve_url(page_base, src, url, sizeof(url)))
            img->url = strdup(url);
    }
    free(src);
    img->state = img->url != NULL ? IMG_WAITING : IMG_FAILED;
    img->link = cur_link;
    if (img->state == IMG_FAILED && img->alt[0] == '\0')
        return;                         // Nothing to show at all

    new_para(PK_IMAGE, false);
    if (para_count == 0 || paras[para_count - 1].kind != PK_IMAGE) {
        free(img->url);
        img->url = NULL;
        return;
    }
    paras[para_count - 1].image = (int16_t)image_count;
    put_raw('#');                       // Keeps it from being reused; not drawn
    image_count++;
    new_para(text_kind, false);
}

static void handle_tag(const char* name, bool closing, const char* attrs, const char* attrs_end) {
    // What the element passes on to its content: before its own line starts
    // (an opening tag), or after its content ends (a closing one)
    if (closing)
        elem_close(name);
    else if (!name_is(name, void_tags) && !name_is(name, skip_tags) &&
             strcmp(name, "title") != 0 && strcmp(name, "textarea") != 0 && strcmp(name, "button") != 0)
        elem_open(name, attrs, attrs_end);

    if (strcmp(name, "form") == 0) {
        if (closing)
            cur_form = -1;
        else
            add_form(attrs, attrs_end);
        new_para(PK_TEXT, false);
        text_kind = PK_TEXT;
    } else if (strcmp(name, "input") == 0) {
        if (!closing)
            handle_input(attrs, attrs_end);
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
        uint8_t kind = name[1] == '1' ? PK_H1 : name[1] == '2' ? PK_H2 : PK_H3;
        new_para(closing ? PK_TEXT : kind, true);
        text_kind = closing ? PK_TEXT : kind;
    } else if (strcmp(name, "br") == 0) {
        if (in_pre)
            text_char('\n');
        else {
            // A line break inside a paragraph: a new one, with no gap. Two
            // in a row make an empty line.
            if (para_count > 0 && para_len() == 0)
                put_raw(' ');
            new_para(text_kind, false);
        }
    } else if (strcmp(name, "pre") == 0) {
        // The newline before </pre> doesn't make an empty line
        if (closing && in_pre && para_len() > 0 && doc_text[doc_len - 1] == '\n')
            doc_len--;
        in_pre = !closing;
        pre_fresh = !closing;
        new_para(closing ? PK_TEXT : PK_PRE, true);
    } else if (strcmp(name, "hr") == 0) {
        new_para(PK_RULE, true);
        put_raw('-');                   // Keeps it from being reused; not drawn
        new_para(PK_TEXT, true);
    } else if (strcmp(name, "li") == 0 || strcmp(name, "dt") == 0 || strcmp(name, "dd") == 0) {
        new_para(PK_TEXT, false);
        if (!closing) {
            const char* lead = name[1] == 'i' ? "  * " : name[1] == 'd' ? "    " : "";
            uint16_t link = cur_link;
            cur_link = 0;
            for (; *lead; lead++)
                put_raw(*lead);
            cur_link = link;
        }
    } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
        if (!closing && para_len() > 0)
            text_char(' ');
    } else if (strcmp(name, "a") == 0) {
        cur_link = 0;
        char href[URL_MAX];
        if (!closing && get_attr(attrs, attrs_end, "href", true, href, sizeof(href)))
            cur_link = add_link(href);
    } else if (strcmp(name, "img") == 0) {
        if (!closing)
            add_image(attrs, attrs_end);
    } else if (name_is(name, gap_tags)) {
        new_para(PK_TEXT, true);
        text_kind = PK_TEXT;
    } else if (name_is(name, block_tags)) {
        new_para(PK_TEXT, false);
        text_kind = PK_TEXT;
    }
}

// Past the next '>' from p (quotes respected), or e
static const char* after_tag(const char* p, const char* e) {
    char quote = 0;
    for (; p < e; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p + 1;
        }
    }
    return e;
}

static void parse_html(const char* p, const char* e) {
    while (p < e) {
        const char* lt = memchr(p, '<', (size_t)(e - p));
        if (lt == NULL) {
            decode_text(p, e, true, text_char);
            break;
        }
        decode_text(p, lt, true, text_char);
        const char* q = lt + 1;

        // <!-- comment -->, <!DOCTYPE>, <?xml?>
        if (q < e && *q == '!') {
            if (e - q >= 3 && q[1] == '-' && q[2] == '-') {
                const char* end = find_ci(q + 3, e, "-->");
                p = end != NULL ? end + 3 : e;
            } else {
                p = after_tag(q, e);
            }
            continue;
        }
        if (q < e && *q == '?') {
            p = after_tag(q, e);
            continue;
        }

        bool closing = q < e && *q == '/';
        if (closing)
            q++;
        char name[TAG_NAME_MAX];
        size_t n = 0;
        while (q < e && isalnum((unsigned char)*q)) {
            if (n < sizeof(name) - 1)
                name[n++] = (char)tolower((unsigned char)*q);
            q++;
        }
        name[n] = '\0';
        if (n == 0 || !isalpha((unsigned char)name[0])) {
            // "a < b": just a character
            text_char('<');
            p = lt + 1;
            continue;
        }
        const char* attrs = q;
        p = after_tag(q, e);
        const char* attrs_end = p > attrs && p[-1] == '>' ? p - 1 : p;
        handle_tag(name, closing, attrs, attrs_end);

        // Elements whose content isn't markup: to their end tag
        bool title = strcmp(name, "title") == 0;
        bool textarea = strcmp(name, "textarea") == 0;
        bool button = strcmp(name, "button") == 0;
        if (!closing && (title || textarea || button || name_is(name, skip_tags))) {
            char end_tag[TAG_NAME_MAX + 2];
            snprintf(end_tag, sizeof(end_tag), "</%s", name);
            const char* end = find_ci(p, e, end_tag);
            const char* content_end = end != NULL ? end : e;
            if (title) {
                title_len = 0;
                doc_title[0] = '\0';
                decode_text(p, content_end, true, title_char);
                if (title_len > 0 && doc_title[title_len - 1] == ' ')
                    doc_title[--title_len] = '\0';
            } else if (textarea) {
                decode_text(p, content_end, true, text_char);
            } else if (button) {
                // Its words (tags inside left out) are the label
                char type[16] = "submit";
                get_attr(attrs, attrs_end, "type", true, type, sizeof(type));
                if (strcasecmp(type, "submit") == 0) {
                    title_len = 0;
                    char saved[TITLE_MAX];
                    strlcpy(saved, doc_title, sizeof(saved));
                    doc_title[0] = '\0';
                    const char* a = p;
                    while (a < content_end) {
                        const char* lt2 = memchr(a, '<', (size_t)(content_end - a));
                        const char* stop = lt2 != NULL ? lt2 : content_end;
                        decode_text(a, stop, true, title_char);
                        a = lt2 != NULL ? after_tag(lt2, content_end) : content_end;
                    }
                    char label[FIELD_MAX];
                    const char* l = doc_title;
                    while (*l == ' ')
                        l++;
                    strlcpy(label, l, sizeof(label));
                    size_t ll = strlen(label);
                    while (ll > 0 && label[ll - 1] == ' ')
                        label[--ll] = '\0';
                    strlcpy(doc_title, saved, sizeof(doc_title));
                    title_len = strlen(doc_title);
                    add_input(IN_SUBMIT, attrs, attrs_end, label, false);
                }
            }
            p = end != NULL ? after_tag(end, e) : e;
        }
    }
    new_para(PK_TEXT, false);           // Trims the last paragraph's space
}

static void parse_plain(const char* p, const char* e) {
    in_pre = true;
    new_para(PK_PRE, false);
    decode_text(p, e, false, text_char);
    while (para_len() > 0 && doc_text[doc_len - 1] == '\n')
        doc_len--;
    in_pre = false;
    new_para(PK_TEXT, false);
}

static void next_image(void);

// The page in raw[0..raw_len) becomes the document; its images start coming
static void show_page(const char* address) {
    doc_clear();
    strlcpy(page_base, address, sizeof(page_base));
    const char* p = raw;
    const char* e = raw + raw_len;
    // A UTF-8 byte order mark
    if (e - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    const char* first = p;
    while (first < e && isspace((unsigned char)*first))
        first++;
    if (first < e && *first == '<')
        parse_html(p, e);
    else
        parse_plain(p, e);

    // History: the page that was up goes on it, unless this is the back
    // button's load, which takes its own page off
    if (loading_back) {
        if (history_count > 0)
            free(history[--history_count]);
        loading_back = false;
    } else if (shown_address[0] != '\0' && strcmp(shown_address, address) != 0) {
        if (history_count == HISTORY_MAX) {
            free(history[0]);
            memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(history[0]));
            history_count--;
        }
        char* copy = strdup(shown_address);
        if (copy != NULL)
            history[history_count++] = copy;
    }
    strlcpy(shown_address, address, sizeof(shown_address));
    char title[TITLE_MAX + 32];
    if (doc_title[0] != '\0')
        snprintf(title, sizeof(title), "%s - " APP_TITLE, doc_title);
    else
        snprintf(title, sizeof(title), APP_TITLE);
    wm_window_set_title(main_win, title);

    char size[24];
    if (raw_len < 1024)
        snprintf(size, sizeof(size), "%lu bytes", (unsigned long)raw_len);
    else
        snprintf(size, sizeof(size), "%lu KB", (unsigned long)((raw_len + 512) / 1024));
    if (raw_len == 0)
        snprintf(page_status, sizeof(page_status), "Empty page");
    else
        snprintf(page_status, sizeof(page_status), "%.80s  (%s%s)", doc_title[0] ? doc_title : "Done", size,
                 raw_cut ? ", cut short" : "");
    set_status(false, page_status);
    next_image();
}

//
// Loading
//

static bool raw_append(const void* data, size_t n) {
    size_t limit = fetch_image >= 0 ? IMAGE_FILE_MAX : PAGE_MAX;
    if (raw_len + n > limit) {
        n = limit - raw_len;
        raw_cut = true;
    }
    if (!grow((void**)&raw, &raw_cap, raw_len + n + 1, 1)) {
        raw_cut = true;
        return false;
    }
    memcpy(raw + raw_len, data, n);
    raw_len += n;
    raw[raw_len] = '\0';
    return !raw_cut;
}

// Stop what wget is fetching. An image it was on waits to be fetched again.
static void stop_fetch(void) {
    if (fetch_pipe >= 0) {
        ipc_close(fetch_pipe);          // wget's next write fails and it ends
        fetch_pipe = -1;
    }
    if (fetch_image >= 0 && (size_t)fetch_image < image_count && images[fetch_image].state == IMG_LOADING)
        images[fetch_image].state = IMG_WAITING;
    fetch_image = -1;
}

static void draw_main(void);

// The network bit, asked for once; wget inherits it. false: the user said no.
static bool ensure_network(void) {
    if (sec_has(SEC_NET) || !sec_has(SEC_ELEVATE))
        return true;                    // Without SEC_ELEVATE wget says what's wrong
    if (net_refused)
        return false;
    set_status(false, "Waiting for network permission...");
    draw_main();
    if (sec_elevate_bits("Let the HTML viewer fetch web pages with wget", SEC_NET) == 0)
        return true;
    net_refused = true;
    set_status(true, errno == EACCES ? "Network permission denied" : "Can't ask for network permission");
    return false;
}

// Start `wget -q -O - url` on a pipe: fetch_pipe. false (status says why).
static bool spawn_wget(const char* url) {
    char wget[PATH_MAX];
    if (os_programpath("wget", wget, sizeof(wget)) == NULL) {
        set_status(true, "wget not installed");
        return false;
    }
    if (!ensure_network())
        return false;
    if (!grow((void**)&raw, &raw_cap, 1, 1)) {
        set_status(true, "Out of memory");
        return false;
    }

    int ends[2];
    if (ipc_pipe(ends) < 0) {
        char text[STATUS_MAX];
        snprintf(text, sizeof(text), "Can't start wget: %s", strerror(errno));
        set_status(true, text);
        return false;
    }
    char address[URL_MAX];
    strlcpy(address, url, sizeof(address));
    char* argv[] = { wget, "-q", "-u", "-O", "-", address, NULL };
    sec_mask_t mask = sec_get() & LAUNCH_MASK;
    if (program_needs_elevation(wget, mask) == 1)
        mask |= SPAWN_ELEVATE;
    pid_t pid = spawn_tty(wget, argv, NULL, mask, ends[1]);
    int err = errno;
    ipc_close(ends[1]);                 // wget holds its own
    if (pid < 0) {
        ipc_close(ends[0]);
        if (err != ECANCELED) {
            char text[STATUS_MAX];
            snprintf(text, sizeof(text), "Can't start wget: %s", strerror(err));
            set_status(true, text);
        }
        return false;
    }
    fetch_pipe = ends[0];
    raw_len = 0;
    raw_cut = false;
    raw[0] = '\0';
    return true;
}

// A file on disk into raw. false (status says why, unless `quiet`).
static bool read_file(const char* path, bool quiet) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (quiet)
            return false;
        char text[STATUS_MAX];
        snprintf(text, sizeof(text), "%s: %s", path, strerror(errno));
        set_status(true, text);
        return false;
    }
    if (!grow((void**)&raw, &raw_cap, 1, 1)) {
        close(fd);
        set_status(true, "Out of memory");
        return false;
    }
    raw_len = 0;
    raw_cut = false;
    raw[0] = '\0';
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (!raw_append(buf, (size_t)n))
            break;
    close(fd);
    return true;
}

//
// Images
//

// Nearest-pixel copy of src (sw x sh) at dw x dh; `over`: alpha blended
// onto that colour (opaque result), else as it is
static uint32_t* scale_pixels(const uint32_t* src, int32_t sw, int32_t sh, int32_t dw, int32_t dh,
                              bool blend, uint32_t over) {
    uint32_t* out = (uint32_t*)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (out == NULL)
        return NULL;
    uint32_t br = (over >> 16) & 0xFF, bg = (over >> 8) & 0xFF, bb = over & 0xFF;
    for (int32_t y = 0; y < dh; y++) {
        const uint32_t* row = src + (size_t)((int64_t)y * sh / dh) * (size_t)sw;
        for (int32_t x = 0; x < dw; x++) {
            uint32_t p = row[(int64_t)x * sw / dw];
            if (blend) {
                uint32_t a = p >> 24;
                uint32_t r = (((p >> 16) & 0xFF) * a + br * (255 - a)) / 255;
                uint32_t g = (((p >> 8) & 0xFF) * a + bg * (255 - a)) / 255;
                uint32_t b = ((p & 0xFF) * a + bb * (255 - a)) / 255;
                p = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            out[(size_t)y * (size_t)dw + (size_t)x] = p;
        }
    }
    return out;
}

// raw holds image `i`'s file: decode it
static void image_failed(image_t* img, const char* why) {
    img->state = IMG_FAILED;
    strlcpy(img->why, why, sizeof(img->why));
}

static void image_decode(image_t* img) {
    int w, h, comp;
    char why[sizeof(img->why)];
    if (raw_cut) {
        image_failed(img, "Over 2 MB");
        return;
    }
    if (raw_len == 0) {
        image_failed(img, "Empty");
        return;
    }
    // The size first, so a huge one is turned down before it is decoded
    bitmap_t bm;
    bool sized = stbi_info_from_memory((const unsigned char*)raw, (int)raw_len, &w, &h, &comp) != 0;
    if (sized && (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > IMAGE_MAX_PIXELS)) {
        snprintf(why, sizeof(why), "Too big (%dx%d)", w, h);
        image_failed(img, why);
        return;
    }
    if (!sized || bitmap_decode(raw, raw_len, &bm) < 0) {
        // stb_image's reason, or why the stb library didn't load
        if (!sized)
            bitmap_decode(raw, raw_len, &bm);   // Fails at once; only for bitmap_error()
        const char* reason = bitmap_error();
        if (reason == NULL || reason[0] == '\0')
            reason = stblib_error();
        if (raw[0] == '<' || raw[0] == ' ' || raw[0] == '\n')
            snprintf(why, sizeof(why), "Not an image");
        else if (reason != NULL && strstr(reason, "stb") != NULL)
            snprintf(why, sizeof(why), "Image library missing");
        else if (reason != NULL && reason[0] != '\0')
            snprintf(why, sizeof(why), "Can't decode (%.32s)", reason);
        else
            snprintf(why, sizeof(why), "Unsupported format");
        image_failed(img, why);
        return;
    }
    if (bm.width > IMAGE_KEEP_W) {
        int32_t kh = (int32_t)((int64_t)bm.height * IMAGE_KEEP_W / bm.width);
        if (kh < 1)
            kh = 1;
        uint32_t* small = scale_pixels(bm.pixels, bm.width, bm.height, IMAGE_KEEP_W, kh, false, 0);
        if (small == NULL) {
            bitmap_free(&bm);
            image_failed(img, "Out of memory");
            return;
        }
        bitmap_free(&bm);
        bm.pixels = small;
        bm.width = IMAGE_KEEP_W;
        bm.height = kh;
    }
    img->pixels = bm.pixels;
    img->w = bm.width;
    img->h = bm.height;
    img->state = IMG_READY;
    layout_width = -1;                  // Its line changes height
    dirty = true;
}

// What the status line says while images come in
static void image_progress(void) {
    size_t total = 0, done = 0;
    for (size_t i = 0; i < image_count; i++) {
        if (images[i].url == NULL)
            continue;
        total++;
        if (images[i].state == IMG_READY || images[i].state == IMG_FAILED)
            done++;
    }
    char text[STATUS_MAX];
    snprintf(text, sizeof(text), "Loading images: %lu of %lu ...", (unsigned long)done + 1, (unsigned long)total);
    set_status(false, text);
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// "data:image/png;base64,iVBOR..." into raw. false (img->why says why).
static bool read_data_uri(image_t* img) {
    const char* comma = strchr(img->url, ',');
    if (comma == NULL) {
        image_failed(img, "Empty data: image");
        return false;
    }
    if (find_ci(img->url, comma, ";base64") == NULL) {
        image_failed(img, "Bad data: image");
        return false;
    }
    raw_len = 0;
    raw_cut = false;
    uint32_t bits = 0;
    int have = 0;
    for (const char* p = comma + 1; *p != '\0'; p++) {
        int v = base64_value(*p);
        if (v < 0)
            continue;                   // '=', spaces
        bits = (bits << 6) | (uint32_t)v;
        have += 6;
        if (have >= 8) {
            have -= 8;
            char byte = (char)((bits >> have) & 0xFF);
            if (!raw_append(&byte, 1))
                break;
        }
    }
    return true;
}

// The status line once the images are done: the page's, and what didn't come
static void images_finished(void) {
    size_t shown = 0, failed = 0;  // Of those with an address
    const image_t* first = NULL;
    for (size_t i = 0; i < image_count; i++) {
        if (images[i].url == NULL)
            continue;
        if (images[i].state == IMG_READY) {
            shown++;
        } else if (images[i].state == IMG_FAILED) {
            failed++;
            if (first == NULL)
                first = &images[i];
        }
    }
    if (status_error)
        return;                         // Something the user should still read
    if (failed == 0 || first == NULL) {
        set_status(false, page_status);
        return;
    }
    // The first failure: its file name and why
    const char* name = first->url;
    if (strncasecmp(name, "data:", 5) == 0) {
        name = "data: image";
    } else {
        const char* slash = strrchr(name, '/');
        if (slash != NULL && slash[1] != '\0')
            name = slash + 1;
    }
    char more[24] = "";
    if (failed > 1)
        snprintf(more, sizeof(more), " (+%lu more)", (unsigned long)(failed - 1));
    char text[STATUS_MAX];
    snprintf(text, sizeof(text), "%lu/%lu images. %.24s: %s%s", (unsigned long)shown,
             (unsigned long)(shown + failed), name, first->why[0] ? first->why : "Not shown", more);
    set_status(false, text);
}

// Fetch the next image that's waiting, or say the page is done
static void next_image(void) {
    if (fetch_pipe >= 0)
        return;
    for (size_t i = 0; i < image_count; i++) {
        image_t* img = &images[i];
        if (img->state != IMG_WAITING)
            continue;
        img->state = IMG_LOADING;
        if (strncasecmp(img->url, "data:", 5) == 0) {
            // In the page itself
            fetch_image = (int)i;
            if (grow((void**)&raw, &raw_cap, 1, 1) && read_data_uri(img))
                image_decode(img);
            else if (img->state != IMG_FAILED)
                image_failed(img, "Out of memory");
            fetch_image = -1;
            continue;
        }
        if (img->url[0] == '/') {
            // A page on disk: its images are files too
            fetch_image = (int)i;
            if (read_file(img->url, true))
                image_decode(img);
            else
                image_failed(img, errno == ENOENT ? "Not found" : strerror(errno));
            fetch_image = -1;
            continue;
        }
        if (!spawn_wget(img->url)) {
            image_failed(img, status_error ? status : "wget didn't start");
            status_error = false;       // Said once, for the image, at the end
            continue;
        }
        fetch_image = (int)i;
        image_progress();
        return;
    }
    images_finished();
}

// wget has finished (its end of the pipe is gone): show what it sent
static void fetch_done(void) {
    int image = fetch_image;
    if (fetch_pipe >= 0) {
        ipc_close(fetch_pipe);
        fetch_pipe = -1;
    }
    fetch_image = -1;
    raw[raw_len] = '\0';

    // With -q, all wget says is errors, "wget: ..." on stderr, which is the
    // same pipe. Before the page: nothing else came. After some of it: the
    // page was cut short.
    const char* prefix = "wget: ";
    size_t plen = strlen(prefix);

    // -u: "wget-url: <address after redirects>" comes first. The page's
    // links and images are taken against that, not what was typed.
    char final_url[URL_MAX] = "";
    const char* url_prefix = "wget-url: ";
    size_t ulen = strlen(url_prefix);
    if (raw_len >= ulen && memcmp(raw, url_prefix, ulen) == 0) {
        char* nl = memchr(raw, '\n', raw_len);
        if (nl != NULL) {
            size_t n = (size_t)(nl - raw) - ulen;
            if (n < sizeof(final_url)) {
                memcpy(final_url, raw + ulen, n);
                final_url[n] = '\0';
            }
            size_t skip = (size_t)(nl - raw) + 1;
            memmove(raw, raw + skip, raw_len - skip + 1);
            raw_len -= skip;
        }
    }
    bool failed = raw_len >= plen && memcmp(raw, prefix, plen) == 0;

    if (image >= 0) {
        if (failed) {
            size_t n = strcspn(raw + plen, "\n");
            char said[STATUS_MAX];
            snprintf(said, sizeof(said), "%.*s", (int)(n < STATUS_MAX - 1 ? n : STATUS_MAX - 1), raw + plen);
            char why[sizeof(images[image].why)];
            pithy(said, why, sizeof(why));
            image_failed(&images[image], why);
        } else
            image_decode(&images[image]);
        dirty = true;
        next_image();
        return;
    }

    if (failed) {
        char said[STATUS_MAX], why[64], text[STATUS_MAX];
        size_t n = strcspn(raw + plen, "\n");
        snprintf(said, sizeof(said), "%.*s", (int)(n < STATUS_MAX - 1 ? n : STATUS_MAX - 1), raw + plen);
        pithy(said, why, sizeof(why));
        snprintf(text, sizeof(text), "Failed: %s", why);
        set_status(true, text);
        next_image();                   // The page still shown may have some to come
        return;
    }
    // "\nwget: ..." near the end: the page as far as it came, and why it stopped
    char warning[STATUS_MAX] = "";
    size_t from = raw_len > 400 ? raw_len - 400 : 0;
    for (size_t i = raw_len; i-- > from;) {
        if (raw[i] == '\n' && raw_len - i - 1 >= plen && memcmp(raw + i + 1, prefix, plen) == 0) {
            size_t n = strcspn(raw + i + 1 + plen, "\n");
            char said[STATUS_MAX], why[64];
            snprintf(said, sizeof(said), "%.*s", (int)(n < STATUS_MAX - 1 ? n : STATUS_MAX - 1), raw + i + 1 + plen);
            pithy(said, why, sizeof(why));
            snprintf(warning, sizeof(warning), "Partial page: %s", why);
            raw_len = i;
            break;
        }
    }
    raw[raw_len] = '\0';
    status_error = false;
    if (final_url[0] != '\0' && strcmp(final_url, fetch_address) != 0) {
        strlcpy(fetch_address, final_url, sizeof(fetch_address));
        ui_edit_init(&address_box, fetch_address, UI_EDIT_MAX - 1, 0);
        address_box.anchor = address_box.cursor;
    }
    show_page(fetch_address);
    if (warning[0] != '\0') {
        strlcpy(page_status, warning, sizeof(page_status));
        set_status(true, warning);
    }
}

// Whatever wget has written so far
static void fetch_pump(void) {
    static char buf[IPC_MSG_MAX];
    while (fetch_pipe >= 0) {
        ssize_t n = ipc_recv(fetch_pipe, buf, sizeof(buf), NULL);
        if (n > 0) {
            if (!raw_append(buf, (size_t)n)) {
                fetch_done();           // Too big: what we have is shown
                return;
            }
        } else if (n < 0 && errno == EAGAIN) {
            return;
        } else {
            fetch_done();               // EPIPE: it ended and everything was read
            return;
        }
    }
}

static void start_load(const char* typed) {
    loading_back = request_back;
    request_back = false;
    stop_fetch();
    net_refused = false;                // Asked for by the user: the prompt may come again

    // Spaces around it go
    char address[URL_MAX];
    while (*typed == ' ')
        typed++;
    strlcpy(address, typed, sizeof(address));
    size_t len = strlen(address);
    while (len > 0 && address[len - 1] == ' ')
        address[--len] = '\0';
    if (len == 0)
        return;

    // Always with a scheme, so wget never takes it for an option
    if (address[0] != '/' && strstr(address, "://") == NULL) {
        char full[URL_MAX + 8];
        snprintf(full, sizeof(full), "http://%s", address);
        strlcpy(address, full, sizeof(address));
    }
    ui_edit_init(&address_box, address, UI_EDIT_MAX - 1, 0);
    address_box.anchor = address_box.cursor;

    if (address[0] == '/') {
        if (read_file(address, false)) {
            status_error = false;
            show_page(address);
        }
        return;
    }

    status_error = false;
    if (!spawn_wget(address)) {
        next_image();                   // What the page on screen still had to fetch
        return;
    }
    strlcpy(fetch_address, address, sizeof(fetch_address));
    char text[STATUS_MAX];
    snprintf(text, sizeof(text), "Loading %s ...", address);
    set_status(false, text);
}

// Escape: nothing more comes for this page
static void stop_all(void) {
    stop_fetch();
    for (size_t i = 0; i < image_count; i++)
        if (images[i].state == IMG_WAITING)
            images[i].state = IMG_FAILED;
    set_status(false, "Stopped");
    dirty = true;
}

// For a query: letters, digits and "*-._" as they are, space as '+', the
// rest %XX (application/x-www-form-urlencoded)
static bool form_encode(char* out, size_t size, size_t* len, const char* text) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*len + 4 >= size)
            return false;
        if (isalnum(*p) || *p == '*' || *p == '-' || *p == '.' || *p == '_')
            out[(*len)++] = (char)*p;
        else if (*p == ' ')
            out[(*len)++] = '+';
        else {
            out[(*len)++] = '%';
            out[(*len)++] = hex[*p >> 4];
            out[(*len)++] = hex[*p & 15];
        }
    }
    out[*len] = '\0';
    return true;
}

// Send form `f` as a browser does with GET. `button`: the inputs[] button
// pressed, -1 for Enter in a box (the form's first button goes along).
static void submit_form(int16_t f, int16_t button) {
    if (f < 0 || (size_t)f >= form_count) {
        set_status(true, "Not in a form");
        return;
    }
    const form_t* fm = &forms[f];
    if (fm->post) {
        set_status(true, "POST forms aren't supported");
        return;
    }
    char url[URL_MAX];
    if (fm->action == NULL || fm->action[0] == '\0' || !resolve_url(page_base, fm->action, url, sizeof(url))) {
        if (fm->action != NULL && fm->action[0] != '\0' && fm->action[0] != '#') {
            char text[STATUS_MAX];
            snprintf(text, sizeof(text), "Bad form address: %.60s", fm->action);
            set_status(true, text);
            return;
        }
        strlcpy(url, page_base, sizeof(url));       // No action: the page itself
    }
    if (url[0] == '/') {
        set_status(true, "Forms need a web address");
        return;
    }
    size_t len = strcspn(url, "?#");                // GET replaces the query
    url[len] = '\0';
    bool first = true;
    bool ok = true;
    if (button < 0) {
        for (size_t i = 0; i < input_count; i++) {
            if (inputs[i].form == f && inputs[i].type == IN_SUBMIT) {
                button = (int16_t)i;
                break;
            }
        }
    }
    for (size_t i = 0; i < input_count && ok; i++) {
        const input_t* in = &inputs[i];
        if (in->form != f || in->name[0] == '\0')
            continue;
        if (in->type == IN_SUBMIT && (int16_t)i != button)
            continue;
        if (len + 2 >= sizeof(url)) {
            ok = false;
            break;
        }
        url[len++] = first ? '?' : '&';
        first = false;
        ok = form_encode(url, sizeof(url), &len, in->name);
        if (ok && len + 2 < sizeof(url)) {
            url[len++] = '=';
            url[len] = '\0';
            ok = form_encode(url, sizeof(url), &len, in->edit.text);
        } else {
            ok = false;
        }
    }
    if (!ok) {
        set_status(true, "Form data too long");
        return;
    }
    start_load(url);
}

// The page shown before this one
static void go_back(void) {
    if (history_count == 0) {
        set_status(false, "Nothing to go back to");
        dirty = true;
        return;
    }
    char url[URL_MAX];
    strlcpy(url, history[history_count - 1], sizeof(url));
    request_back = true;
    start_load(url);
    dirty = true;
}

static void follow_link(uint16_t link) {
    if (link == 0 || link > link_count)
        return;
    const link_t* l = &links[link - 1];
    if (!l->ok) {
        char text[STATUS_MAX];
        snprintf(text, sizeof(text), "Can't open %.80s", l->url);
        set_status(true, text);
        return;
    }
    char url[URL_MAX];
    strlcpy(url, l->url, sizeof(url));  // start_load clears links[]
    start_load(url);
}

//
// Layout and drawing
//

typedef struct {
    int32_t width, height;
    int32_t bar_h;                      // The address bar
    int32_t status_h;
    gfx_rect_t page;                    // Where the text goes (scroll bar beside it)
    int32_t cw, lh;                     // Font cell at scale 1
} metrics_t;

static metrics_t get_metrics(gfx_surface_t* s) {
    metrics_t m;
    wm_window_get_size(main_win, &m.width, &m.height);
    m.cw = gfx_char_width(s);
    m.lh = gfx_line_height(s);
    if (m.cw < 1) m.cw = 1;
    if (m.lh < 1) m.lh = 1;
    m.bar_h = m.lh + 14;
    m.status_h = m.lh + 6;
    int32_t h = m.height - m.bar_h - m.status_h;
    m.page = gfx_rect(0, m.bar_h, m.width - SCROLL_W, h > 0 ? h : 0);
    return m;
}

static int kind_scale(uint8_t kind) {
    return kind == PK_H1 || kind == PK_H2 ? 2 : 1;
}

static bool layout_center;              // The paragraph being laid out is centred

static gfx_color_t page_background(void) {
    return page_bg != 0 ? page_bg : theme.client.background;
}

// Text with no colour of its own: the theme's, or black / white, whichever
// shows on a background the page chose
static gfx_color_t default_text(bool soft) {
    if (page_bg == 0)
        return soft ? theme.client.text_soft : theme.client.text;
    uint32_t r = (page_bg >> 16) & 0xFF, g = (page_bg >> 8) & 0xFF, b = page_bg & 0xFF;
    bool dark = (r * 299 + g * 587 + b * 114) / 1000 < 128;
    return dark ? GFX_RGB(235, 235, 235) : GFX_RGB(0, 0, 0);
}

// A field's box or button, in pixels
static int32_t input_width(const input_t* in, const metrics_t* m) {
    int32_t text_w = m->page.w - 2 * PAD;
    int32_t w = in->type == IN_SUBMIT ? (int32_t)strlen(in->label) * m->cw + 16 : in->cols * m->cw + 10;
    return w < text_w ? w : text_w > 20 ? text_w : 20;
}

static void add_line(size_t start, size_t len, uint8_t kind, int32_t h) {
    size_t old = line_cap;
    if (!grow((void**)&lines, &line_cap, line_count + 1, sizeof(line_t))) {
        line_cap = old;
        return;
    }
    lines[line_count].start = start;
    lines[line_count].len = len;
    lines[line_count].kind = kind;
    lines[line_count].center = layout_center;
    lines[line_count].image = -1;
    lines[line_count].y = doc_height;
    lines[line_count].h = h;
    line_count++;
    doc_height += h;
}

// Break [s, e) into lines of at most `cols`: after a space when one is near
static void wrap_words(size_t s, size_t e, size_t cols, uint8_t kind, int32_t h) {
    size_t first = line_count;
    while (s < e) {
        while (s < e && doc_text[s] == ' ')
            s++;
        if (s >= e)
            break;
        if (e - s <= cols) {
            add_line(s, e - s, kind, h);
            break;
        }
        size_t cut = s + cols;
        size_t k = cut;
        while (k > s && doc_text[k] != ' ')
            k--;
        if (k > s) {
            add_line(s, k - s, kind, h);
            s = k + 1;
        } else {
            add_line(s, cols, kind, h);
            s = cut;
        }
    }
    if (line_count == first)
        add_line(s, 0, kind, h);        // Only spaces: an empty line (<br><br>)
}

// An image's line: the picture fitted to the page width, or its alt text
static void layout_image(int16_t index, int32_t text_w, int32_t lh) {
    image_t* img = &images[index];
    size_t before = line_count;
    if (img->state == IMG_READY) {
        if (img->w <= IMAGE_TINY && img->h <= IMAGE_TINY)
            return;
        int32_t dw = img->w < text_w ? img->w : text_w;
        int32_t dh = (int32_t)((int64_t)img->h * dw / img->w);
        if (dh < 1)
            dh = 1;
        if (img->shown == NULL || img->sw != dw || img->sh != dh) {
            free(img->shown);
            img->shown = scale_pixels(img->pixels, img->w, img->h, dw, dh, true, page_background());
            img->sw = dw;
            img->sh = dh;
        }
        if (img->shown == NULL)
            return;
        add_line(0, 0, PK_IMAGE, dh + 4);
    } else if (img->state == IMG_FAILED && img->alt[0] == '\0') {
        return;
    } else {
        add_line(0, 0, PK_IMAGE, lh);
    }
    if (line_count > before)
        lines[line_count - 1].image = index;
}

static void layout(gfx_surface_t* s, const metrics_t* m) {
    int32_t text_w = m->page.w - 2 * PAD;
    if (text_w < m->cw)
        text_w = m->cw;
    layout_width = m->page.w;
    line_count = 0;
    doc_height = PAD;

    gfx_save(s);
    gfx_set_text_scale(s, 2);
    int32_t lh2 = gfx_line_height(s);
    gfx_restore(s);

    for (size_t i = 0; i < para_count; i++) {
        const para_t* p = &paras[i];
        size_t start = p->start;
        size_t end = i + 1 < para_count ? paras[i + 1].start : doc_len;
        // Empty: the one left open at the end. One in the middle is a
        // <br> on its own, an empty line.
        if (end == start && p->kind != PK_RULE && i + 1 == para_count)
            continue;
        if (p->gap && line_count > 0)
            doc_height += m->lh / 2 + 2;
        int scale = kind_scale(p->kind);
        int32_t h = scale == 2 ? lh2 + 2 : m->lh + 2;
        layout_center = p->center;
        size_t cols = (size_t)(text_w / (m->cw * scale));
        if (cols < 1)
            cols = 1;

        if (p->kind == PK_IMAGE) {
            if (p->image >= 0 && (size_t)p->image < image_count)
                layout_image(p->image, text_w, m->lh + 2);
        } else if (p->kind == PK_INPUT) {
            if (p->image >= 0 && (size_t)p->image < input_count) {
                add_line(start, 0, PK_INPUT, m->lh + 14);
                lines[line_count - 1].image = p->image;
            }
        } else if (p->kind == PK_RULE) {
            add_line(start, 0, PK_RULE, m->lh);
        } else if (p->kind == PK_PRE) {
            // Its own line breaks; a line too long is cut onto the next
            size_t a = start;
            while (a <= end) {
                const char* nl = memchr(doc_text + a, '\n', end - a);
                size_t b = nl != NULL ? (size_t)(nl - doc_text) : end;
                if (b == a)
                    add_line(a, 0, PK_PRE, h);
                for (size_t c = a; c < b; c += cols)
                    add_line(c, b - c < cols ? b - c : cols, PK_PRE, h);
                if (nl == NULL)
                    break;
                a = b + 1;
            }
        } else {
            wrap_words(start, end, cols, p->kind, h);
        }
    }
    doc_height += PAD;
    layout_center = false;
}

// How wide a line is drawn, and where it starts (from the page's left
// margin): centred lines are moved in
static int32_t line_width(const line_t* l, const metrics_t* m) {
    if (l->kind == PK_IMAGE) {
        const image_t* img = &images[l->image];
        return img->state == IMG_READY && img->shown != NULL ? img->sw
             : (int32_t)(strlen(img->alt[0] ? img->alt : "image") + 2) * m->cw;
    }
    if (l->kind == PK_INPUT)
        return input_width(&inputs[l->image], m);
    if (l->kind == PK_RULE)
        return m->page.w - 2 * PAD;
    return (int32_t)l->len * m->cw * kind_scale(l->kind);
}

static int32_t line_offset(const line_t* l, const metrics_t* m) {
    if (!l->center)
        return 0;
    int32_t free_w = m->page.w - 2 * PAD - line_width(l, m);
    return free_w > 0 ? free_w / 2 : 0;
}

// A field's box or button on the window (its line's top at y)
static gfx_rect_t input_rect(const line_t* l, int32_t y, const metrics_t* m) {
    return gfx_rect(m->page.x + PAD + line_offset(l, m), y + 3, input_width(&inputs[l->image], m), m->lh + 8);
}

// The y in the window where a line's top is drawn
static int32_t line_y(const line_t* l, const metrics_t* m) {
    return m->page.y + l->y - scroll_y + 1;
}

// Where a text box goes, for drawing and for clicks
static void place_input(gfx_surface_t* s, const line_t* l, const metrics_t* m) {
    gfx_rect_t r = input_rect(l, line_y(l, m), m);
    ui_edit_place(&inputs[l->image].edit, r.x + r.w / 2, gfx_text_y(s, r.y, r.h), r.w, r.w, m->page);
}

static int32_t max_scroll(const metrics_t* m) {
    int32_t v = doc_height - m->page.h;
    return v > 0 ? v : 0;
}

static void clamp_scroll(const metrics_t* m) {
    if (scroll_y > max_scroll(m))
        scroll_y = max_scroll(m);
    if (scroll_y < 0)
        scroll_y = 0;
}

static gfx_color_t link_color(uint16_t link) {
    return link == hover_link ? theme.button.border_active : theme.client.accent;
}

static void draw_image_line(gfx_surface_t* s, const line_t* l, int32_t x, int32_t y) {
    const image_t* img = &images[l->image];
    if (img->state == IMG_READY && img->shown != NULL) {
        gfx_draw_pixels(s, img->shown, img->sw, img->sh, 0, x, y + 1);
        if (img->link != 0 && img->link == hover_link)
            gfx_draw_rect(s, x - 1, y, img->sw + 2, img->sh + 2, theme.client.accent);
        return;
    }
    // Not here (yet): "[alt]"
    char text[ALT_MAX + 16];
    snprintf(text, sizeof(text), "[%s]", img->alt[0] ? img->alt : "image");
    gfx_draw_text(s, x, y, text, img->link ? link_color(img->link) : theme.client.text_muted);
}

static void draw_input_line(gfx_surface_t* s, const line_t* l, int32_t y, const metrics_t* m) {
    input_t* in = &inputs[l->image];
    gfx_rect_t r = input_rect(l, y, m);
    if (in->type == IN_SUBMIT) {
        bool down = press_button == l->image;
        gfx_fill_rect(s, r.x, r.y, r.w, r.h, down ? theme.button.fill_pressed : theme.button.fill);
        gfx_draw_rect(s, r.x, r.y, r.w, r.h, down ? theme.button.border_pressed : theme.button.border);
        int32_t tw = gfx_text_width(s, in->label);
        gfx_draw_text(s, r.x + (r.w - tw) / 2, gfx_text_y(s, r.y, r.h), in->label, theme.button.text);
        return;
    }
    bool focused = focus_input == l->image;
    place_input(s, l, m);
    ui_edit_style_t st = {
        .background = theme.edit.background,
        .border     = focused ? theme.button.border_active : theme.edit.border,
        .text       = theme.edit.text,
        .cursor     = focused ? theme.edit.cursor : theme.edit.background,
        .selection  = focused ? theme.edit.selection : theme.edit.background,
    };
    ui_edit_paint(&in->edit, s, &st);
    if (in->edit.len == 0 && !focused && in->placeholder[0] != '\0') {
        gfx_save(s);
        gfx_rect_t inner = gfx_rect(r.x + 2, r.y, r.w - 4, r.h);
        gfx_intersect_clip(s, &inner);
        gfx_draw_text(s, in->edit.rect.x + 4, gfx_text_y(s, r.y, r.h), in->placeholder, theme.client.text_muted);
        gfx_restore(s);
    }
}

static void draw_line(gfx_surface_t* s, const line_t* l, int32_t x, int32_t y, const metrics_t* m) {
    if (l->kind == PK_INPUT) {
        draw_input_line(s, l, y, m);
        return;
    }
    x += line_offset(l, m);
    if (l->kind == PK_IMAGE) {
        draw_image_line(s, l, x, y);
        return;
    }
    if (l->kind == PK_RULE) {
        gfx_draw_hline(s, x, y + l->h / 2, m->page.w - 2 * PAD, theme.client.divider_soft);
        return;
    }
    gfx_save(s);
    gfx_set_text_scale(s, kind_scale(l->kind));
    int32_t cw = gfx_char_width(s);
    bool bold = l->kind == PK_H3;
    size_t i = 0;
    while (i < l->len) {
        // A run of characters with the same link and colour
        uint16_t a = doc_link[l->start + i];
        gfx_color_t own = doc_color[l->start + i];
        size_t j = i + 1;
        while (j < l->len && doc_link[l->start + j] == a && doc_color[l->start + j] == own)
            j++;
        gfx_color_t color = a != 0 ? link_color(a)
                          : own != 0 ? own : default_text(l->kind == PK_PRE);
        int32_t rx = x + (int32_t)i * cw;
        gfx_draw_text_n(s, rx, y, doc_text + l->start + i, j - i, color);
        if (bold)
            gfx_draw_text_n(s, rx + 1, y, doc_text + l->start + i, j - i, color);
        if (a != 0)
            gfx_draw_hline(s, rx, y + gfx_line_height(s) - 1, (int32_t)(j - i) * cw, color);
        i = j;
    }
    gfx_restore(s);
}

static void draw_page(gfx_surface_t* s, const metrics_t* m) {
    const gfx_rect_t* r = &m->page;
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, page_background());
    gfx_save(s);
    gfx_intersect_clip(s, r);

    if (para_count == 0 && fetch_pipe < 0) {
        const char* hint = "Type an address above (http:// or https://) and press Enter.";
        gfx_draw_text(s, r->x + PAD, r->y + PAD, hint, theme.client.text_muted);
    }
    for (size_t i = 0; i < line_count; i++) {
        const line_t* l = &lines[i];
        int32_t y = r->y + l->y - scroll_y;
        if (y + l->h <= r->y)
            continue;
        if (y >= r->y + r->h)
            break;
        draw_line(s, l, r->x + PAD, y + 1, m);
    }
    gfx_restore(s);

    // Scroll bar: just where the view is
    int32_t bx = r->x + r->w;
    gfx_fill_rect(s, bx, r->y, SCROLL_W, r->h, theme.button.fill);
    if (doc_height > r->h && r->h > 0) {
        int32_t th = (int32_t)((int64_t)r->h * r->h / doc_height);
        if (th < 12) th = 12;
        if (th > r->h) th = r->h;
        int32_t ty = (int32_t)((int64_t)(r->h - th) * scroll_y / (max_scroll(m) ? max_scroll(m) : 1));
        gfx_fill_rect(s, bx + 2, r->y + ty + 1, SCROLL_W - 4, th - 2, theme.button.border);
    }
}

// The back button, square, at the left of the bar
static gfx_rect_t back_rect(const metrics_t* m) {
    int32_t side = m->bar_h - 8;
    return gfx_rect(PAD - 2, 4, side, side);
}

static bool in_back(const metrics_t* m, int32_t x, int32_t y) {
    gfx_rect_t b = back_rect(m);
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static gfx_rect_t address_rect(gfx_surface_t* s, const metrics_t* m) {
    (void)s;
    gfx_rect_t b = back_rect(m);
    int32_t x = b.x + b.w + PAD;
    return gfx_rect(x, 4, m->width - x - PAD, m->bar_h - 8);
}

static void draw_back(gfx_surface_t* s, const metrics_t* m) {
    gfx_rect_t b = back_rect(m);
    bool can = history_count > 0;
    gfx_color_t fill = can && back_hover ? (back_press ? theme.button.fill_pressed : theme.edit.selection)
                                         : theme.edit.background;
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, fill);
    gfx_save(s);
    gfx_intersect_clip(s, &b);
    resource_info_t ri;
    if (back_icon > 0 && resource_info(back_icon, &ri) == 0) {
        gfx_draw_resource(s, back_icon, b.x + (b.w - (int32_t)ri.width) / 2, b.y + (b.h - (int32_t)ri.height) / 2);
        if (!can) {
            // Greyed out: nothing to go back to
            gfx_set_blend(s, GFX_BLEND_ALPHA);
            gfx_fill_rect(s, b.x, b.y, b.w, b.h, (theme.edit.background & 0x00FFFFFFu) | 0xA0000000u);
        }
    } else {
        gfx_draw_text(s, b.x + (b.w - gfx_text_width(s, "<")) / 2, gfx_text_y(s, b.y, b.h), "<",
                      can ? theme.edit.text : theme.client.text_muted);
    }
    gfx_restore(s);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, can && back_hover ? theme.button.border_active : theme.edit.border);
}

static void draw_bar(gfx_surface_t* s, const metrics_t* m) {
    gfx_fill_rect(s, 0, 0, m->width, m->bar_h, theme.button.fill);
    draw_back(s, m);
    gfx_rect_t r = address_rect(s, m);
    ui_edit_place(&address_box, r.x + r.w / 2, gfx_text_y(s, r.y, r.h), r.w, r.w, gfx_rect(0, 0, m->width, m->bar_h));
    ui_edit_style_t st = {
        .background = theme.edit.background,
        .border     = address_focus ? theme.button.border_active : theme.edit.border,
        .text       = theme.edit.text,
        .cursor     = address_focus ? theme.edit.cursor : theme.edit.background,
        .selection  = address_focus ? theme.edit.selection : theme.edit.background,
    };
    ui_edit_paint(&address_box, s, &st);
    gfx_draw_hline(s, 0, m->bar_h - 1, m->width, theme.client.divider_soft);
}

static void draw_status(gfx_surface_t* s, const metrics_t* m) {
    int32_t top = m->height - m->status_h;
    gfx_fill_rect(s, 0, top, m->width, m->status_h, theme.button.fill);
    gfx_draw_hline(s, 0, top, m->width, theme.client.divider_soft);
    size_t fit = (size_t)((m->width - 2 * PAD) / m->cw);
    char text[STATUS_MAX];
    gfx_color_t color = status_error ? theme.client.text_danger : theme.client.text_soft;
    if (back_hover && history_count > 0) {
        char where[STATUS_MAX];
        snprintf(where, sizeof(where), "Back to %s", history[history_count - 1]);
        ui_fit_text(where, fit, text, sizeof(text));
        color = theme.client.accent;
    } else if (hover_link != 0 && hover_link <= link_count) {
        // Where the link under the pointer goes
        ui_fit_text(links[hover_link - 1].url, fit, text, sizeof(text));
        color = theme.client.accent;
    } else {
        ui_fit_text(status, fit, text, sizeof(text));
    }
    gfx_draw_text(s, PAD, gfx_text_y(s, top, m->status_h), text, color);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    if (layout_width != m.page.w)
        layout(s, &m);
    clamp_scroll(&m);

    draw_bar(s, &m);
    draw_page(s, &m);
    draw_status(s, &m);
    wm_window_commit(main_win, NULL);
}

//
// Events
//

// The line at (x, y) in the window, NULL for none
static const line_t* line_at(const metrics_t* m, int32_t x, int32_t y) {
    const gfx_rect_t* r = &m->page;
    if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h)
        return NULL;
    int32_t dy = y - r->y + scroll_y - 1;
    size_t lo = 0, hi = line_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lines[mid].y + lines[mid].h <= dy)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= line_count || lines[lo].y > dy)
        return NULL;
    return &lines[lo];
}

// The form field at (x, y): its line, or NULL
static const line_t* input_at(int32_t x, int32_t y) {
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return NULL;
    metrics_t m = get_metrics(s);
    const line_t* l = line_at(&m, x, y);
    if (l == NULL || l->kind != PK_INPUT)
        return NULL;
    gfx_rect_t r = input_rect(l, line_y(l, &m), &m);
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h ? l : NULL;
}

// The link at (x, y) in the window: index + 1, or 0
static uint16_t link_at(int32_t x, int32_t y) {
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return 0;
    metrics_t m = get_metrics(s);
    const gfx_rect_t* r = &m.page;
    if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h)
        return 0;
    int32_t dy = y - r->y + scroll_y - 1;
    // Lines are in order of y: halve to the one holding dy
    size_t lo = 0, hi = line_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lines[mid].y + lines[mid].h <= dy)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= line_count || lines[lo].y > dy)
        return 0;
    const line_t* l = &lines[lo];
    if (l->kind == PK_INPUT)
        return 0;
    int32_t dx = x - r->x - PAD - line_offset(l, &m);
    if (dx < 0)
        return 0;
    if (l->kind == PK_IMAGE) {
        const image_t* img = &images[l->image];
        int32_t w = img->state == IMG_READY ? img->sw : (int32_t)(strlen(img->alt[0] ? img->alt : "image") + 2) * m.cw;
        return dx < w ? img->link : 0;
    }
    if (l->kind == PK_RULE)
        return 0;
    size_t col = (size_t)(dx / (m.cw * kind_scale(l->kind)));
    return col < l->len ? doc_link[l->start + col] : 0;
}

static void scroll_by(int32_t dy) {
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics(s);
    scroll_y += dy;
    clamp_scroll(&m);
    dirty = true;
}

static void focus_address(void) {
    address_focus = true;
    focus_input = -1;
    ui_edit_select_all(&address_box);
    dirty = true;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    gfx_surface_t* s = wm_window_surface(main_win);
    int32_t lh = s != NULL ? gfx_line_height(s) + 2 : 10;
    int32_t page_h = s != NULL ? get_metrics(s).page.h - lh : 100;

    if (key == KEY_F6 || ((ev->mods & INPUT_MOD_CTRL) && (key == 'l' || key == 'L' || key == 12))) {
        focus_address();
        return;
    }
    if (key == KEY_F5) {
        char again[URL_MAX];
        strlcpy(again, shown_address[0] ? shown_address : address_box.text, sizeof(again));
        start_load(again);
        return;
    }
    if (key == KEY_ESCAPE && fetch_pipe >= 0) {
        stop_all();
        return;
    }

    if ((ev->mods & INPUT_MOD_ALT) && key == KEY_LEFT) {
        go_back();
        return;
    }
    if (key == KEY_TAB) {
        // To the next text box on the page (the first after the last)
        int16_t start = focus_input;
        for (size_t k = 1; k <= input_count; k++) {
            size_t i = (size_t)(start + (int)k + (int)input_count) % input_count;
            if (inputs[i].type == IN_TEXT && i < input_count) {
                bool shown = false;
                for (size_t j = 0; j < line_count && !shown; j++)
                    shown = lines[j].kind == PK_INPUT && lines[j].image == (int16_t)i;
                if (!shown)
                    continue;
                focus_input = (int16_t)i;
                address_focus = false;
                ui_edit_select_all(&inputs[i].edit);
                dirty = true;
                return;
            }
        }
        return;
    }

    if (!address_focus && focus_input >= 0 && (size_t)focus_input < input_count) {
        input_t* in = &inputs[focus_input];
        switch (ui_edit_key(&in->edit, key, ev->mods)) {
            case UI_EDIT_ACCEPT:
                submit_form(in->form, -1);
                break;
            case UI_EDIT_CANCEL:
                focus_input = -1;
                break;
            case UI_EDIT_IGNORED:
                break;
            default:
                break;
        }
        dirty = true;
        return;
    }

    if (address_focus) {
        switch (ui_edit_key(&address_box, key, ev->mods)) {
            case UI_EDIT_ACCEPT: {
                char typed[URL_MAX];
                strlcpy(typed, address_box.text, sizeof(typed));
                start_load(typed);
                address_focus = false;
                break;
            }
            case UI_EDIT_CANCEL:
                address_focus = false;
                break;
            default:
                break;
        }
        dirty = true;
        return;
    }

    switch (key) {
        case KEY_UP:        scroll_by(-lh);           break;
        case KEY_DOWN:      scroll_by(lh);            break;
        case KEY_PAGE_UP:   scroll_by(-page_h);       break;
        case KEY_PAGE_DOWN:
        case ' ':           scroll_by(page_h);        break;
        case KEY_HOME:      scroll_by(-doc_height);   break;
        case KEY_END:       scroll_by(doc_height);    break;
        case KEY_BACKSPACE: go_back();                break;
        default:            break;
    }
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    address_drag = false;
    input_drag = false;
    press_link = 0;
    press_button = -1;
    back_press = false;
    const line_t* field = NULL;
    gfx_surface_t* bs = wm_window_surface(main_win);
    if (bs != NULL) {
        metrics_t bm = get_metrics(bs);
        if (in_back(&bm, ev->x, ev->y)) {
            back_press = history_count > 0;
            dirty = true;
            return;
        }
    }
    if (ui_edit_click(&address_box, ev->x, ev->y, ev->mods, ev->clicks)) {
        address_focus = true;
        address_drag = true;
        focus_input = -1;
    } else if ((field = input_at(ev->x, ev->y)) != NULL) {
        address_focus = false;
        input_t* in = &inputs[field->image];
        if (in->type == IN_SUBMIT) {
            press_button = field->image;
        } else {
            gfx_surface_t* s = wm_window_surface(main_win);
            if (s != NULL) {
                metrics_t m = get_metrics(s);
                place_input(s, field, &m);
            }
            focus_input = field->image;
            ui_edit_click(&in->edit, ev->x, ev->y, ev->mods, ev->clicks);
            input_drag = true;
        }
    } else {
        address_focus = false;
        focus_input = -1;
        press_link = link_at(ev->x, ev->y);
    }
    dirty = true;
}

// A link is followed when the button comes up on the one it went down on
static void main_mouse_up(const wm_event_t* ev) {
    address_drag = false;
    input_drag = false;
    if (ev->button == INPUT_BTN_LEFT && back_press) {
        back_press = false;
        dirty = true;
        gfx_surface_t* s = wm_window_surface(main_win);
        if (s != NULL) {
            metrics_t m = get_metrics(s);
            if (in_back(&m, ev->x, ev->y))
                go_back();
        }
        return;
    }
    if (ev->button == INPUT_BTN_LEFT && press_button >= 0) {
        // A button: sent when the mouse comes up on it
        int16_t b = press_button;
        press_button = -1;
        dirty = true;
        const line_t* l = input_at(ev->x, ev->y);
        if (l != NULL && l->image == b)
            submit_form(inputs[b].form, b);
        return;
    }
    if (ev->button != INPUT_BTN_LEFT || press_link == 0)
        return;
    uint16_t link = link_at(ev->x, ev->y);
    bool same = link == press_link;
    press_link = 0;
    if (same)
        follow_link(link);
}

static void main_mouse_move(const wm_event_t* ev) {
    if (address_drag && (ev->buttons & INPUT_BTN_LEFT)) {
        ui_edit_drag(&address_box, ev->x);
        dirty = true;
        return;
    }
    if (input_drag && (ev->buttons & INPUT_BTN_LEFT) && focus_input >= 0) {
        ui_edit_drag(&inputs[focus_input].edit, ev->x);
        dirty = true;
        return;
    }
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s != NULL) {
        metrics_t m = get_metrics(s);
        bool over = in_back(&m, ev->x, ev->y);
        if (over != back_hover) {
            back_hover = over;
            dirty = true;
        }
    }
    uint16_t link = link_at(ev->x, ev->y);
    if (link != hover_link) {
        hover_link = link;
        dirty = true;
    }
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);           break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);    break;
        case WM_EVENT_MOUSE_UP:     main_mouse_up(ev);      break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);    break;
        case WM_EVENT_MOUSE_LEAVE:
            if (hover_link != 0 || back_hover) {
                hover_link = 0;
                back_hover = false;
                dirty = true;
            }
            break;
        case WM_EVENT_MOUSE_WHEEL: {
            gfx_surface_t* s = wm_window_surface(main_win);
            int32_t lh = s != NULL ? gfx_line_height(s) + 2 : 10;
            scroll_by(ev->wheel * WHEEL_LINES * lh);
            hover_link = link_at(ev->x, ev->y);
            break;
        }
        case WM_EVENT_RESIZED:
        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            dirty = true;
            break;
        case WM_EVENT_CLOSE:
            running = false;
            break;
        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

int main(int argc, char** argv) {
    load_theme();
    back_icon = resource_open_system(BACK_ICON);
    ui_edit_init(&address_box, argc > 1 ? argv[1] : "", UI_EDIT_MAX - 1, 0);
    set_status(false, "Ready");

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = APP_TITLE;
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = WIN_W;
    d.height     = WIN_H;
    d.min_width  = WIN_MIN_W;
    d.min_height = WIN_MIN_H;
    d.style      = WM_STYLE_RESIZABLE;

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    draw_main();
    wm_window_focus(main_win);
    if (argc > 1)
        start_load(argv[1]);

    wm_event_t ev;
    while (running) {
        int32_t timeout = fetch_pipe >= 0 ? POLL_MS : dirty ? 0 : -1;
        int r = wm_wait_event(&ev, timeout);
        if (r < 0)
            break;                      // The dwm went away
        if (fetch_pipe >= 0)
            fetch_pump();
        if (r == 0) {
            if (dirty)
                draw_main();
            continue;
        }
        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            for (size_t i = 0; i < image_count; i++) {
                free(images[i].shown);  // Made on the old background
                images[i].shown = NULL;
            }
            layout_width = -1;          // The font may have changed too
            dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    stop_fetch();
    wm_disconnect();
    return 0;
}
