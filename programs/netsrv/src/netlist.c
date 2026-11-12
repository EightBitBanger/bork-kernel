/*
 * Scan results into the list the popup shows (netlist.h)
 */

#include <string.h>
#include "netlist.h"

void netlist_escape(const uint8_t* ssid, uint32_t length, char* out) {
    static const char hex[] = "0123456789abcdef";
    uint32_t n = 0;
    if (length > WLAN_SSID_MAX)
        length = WLAN_SSID_MAX;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t c = ssid[i];
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out[n++] = (char)c;
        } else {
            out[n++] = '\\';
            out[n++] = 'x';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

uint8_t netlist_bars(int8_t rssi) {
    if (rssi <= WLAN_RSSI_UNKNOWN) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -77) return 2;
    if (rssi >= -88) return 1;
    return 0;
}

// Strongest first. Levels not heard go last; then by name, hidden last;
// then by address, so the order never depends on the driver's
static int compare(const netlist_entry_t* a, const netlist_entry_t* b) {
    int ra = a->rssi <= WLAN_RSSI_UNKNOWN ? -1000 : a->rssi;
    int rb = b->rssi <= WLAN_RSSI_UNKNOWN ? -1000 : b->rssi;
    if (ra != rb)
        return rb - ra;
    if (a->hidden != b->hidden)
        return a->hidden ? 1 : -1;
    int byname = strcmp(a->name, b->name);
    if (byname != 0)
        return byname;
    return memcmp(a->bssid, b->bssid, sizeof(a->bssid));
}

// Insertion sort: 32 at most, and stable
static void sort(netlist_t* l) {
    for (uint32_t i = 1; i < l->count; i++) {
        netlist_entry_t e = l->entry[i];
        uint32_t j = i;
        while (j > 0 && compare(&l->entry[j - 1], &e) > 0) {
            l->entry[j] = l->entry[j - 1];
            j--;
        }
        l->entry[j] = e;
    }
}

static bool is_hidden(const struct WlanBss* b) {
    if (b->ssid_length == 0 || (b->flags & WLAN_BSS_HIDDEN))
        return true;
    for (uint32_t i = 0; i < b->ssid_length && i < WLAN_SSID_MAX; i++)
        if (b->ssid[i] != 0)
            return false;
    return true;            // All zero bytes: a hidden name of that length
}

void netlist_build(const struct WlanScanResults* r, netlist_t* out) {
    memset(out, 0, sizeof(*out));
    uint32_t n = r->count < WLAN_SCAN_MAX ? r->count : WLAN_SCAN_MAX;

    for (uint32_t i = 0; i < n; i++) {
        const struct WlanBss* b = &r->bss[i];
        bool hidden = is_hidden(b);
        uint32_t length = b->ssid_length < WLAN_SSID_MAX ? b->ssid_length : WLAN_SSID_MAX;

        // Another access point of a network already listed: keep the stronger
        netlist_entry_t* same = NULL;
        if (!hidden) {
            char name[NETLIST_NAME_MAX];
            netlist_escape(b->ssid, length, name);
            for (uint32_t k = 0; k < out->count && same == NULL; k++)
                if (!out->entry[k].hidden && strcmp(out->entry[k].name, name) == 0)
                    same = &out->entry[k];
        }

        netlist_entry_t* e = same;
        if (e != NULL) {
            if (e->aps < 255)
                e->aps++;
            if (b->rssi <= e->rssi)
                continue;
        } else {
            e = &out->entry[out->count++];
            e->aps = 1;
        }
        e->hidden   = hidden;
        e->rssi     = b->rssi;
        e->bars     = netlist_bars(b->rssi);
        e->channel  = b->channel;
        e->security = b->security;
        memcpy(e->bssid, b->bssid, sizeof(e->bssid));
        e->ssid_length = hidden ? 0 : (uint8_t)length;
        memset(e->ssid, 0, sizeof(e->ssid));
        if (!hidden)
            memcpy(e->ssid, b->ssid, length);
        if (hidden)
            strcpy(e->name, NETLIST_HIDDEN);
        else
            netlist_escape(b->ssid, length, e->name);
    }
    sort(out);
}
