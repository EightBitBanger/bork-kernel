//
// Realtek ALC221 codec driver: sound on the HP ProDesk 400 G1 SFF
//
// The ProDesk 400 G1 (and the 600 G1, EliteDesk 800 G1 and other HP
// business desktops of that time) has an Intel 8 Series / C220 HD Audio
// controller (8086:8c20) with one Realtek
// ALC221 codec (10ec:0221): a mono internal speaker, a rear line out and
// line in, and front headphone and microphone jacks. The Haswell CPU's
// display audio controller (8086:0c0c) holds only the HDMI / DisplayPort
// codec and gets no /dev/audio.
//
// The controller is plain Intel HDA, so the HDA driver file (sys/drivers/hda,
// built from drivers/hda) runs it as it is and its generic codec
// code finds the paths from the pin defaults the BIOS sets. This file adds
// what is particular to the codec and the machine, through the codec
// driver hooks (hda_codec_driver.h):
//
//   - desktop jack rules: the internal speaker goes quiet while anything is
//     plugged into the rear line out or the front headphone jack, and the
//     rear line out goes quiet while headphones are in (the way HP's own
//     Windows driver and Linux both behave on these machines);
//   - a fallback pin layout when the BIOS leaves every output pin "not
//     connected" (seen after some BIOS updates / CMOS resets on HP desktops):
//     the one Linux uses for HP's ALC221 desktops (patch_realtek.c, the
//     "HP Workstation" pin quirk), rear line out 0x14, internal speaker
//     0x17, front microphone 0x18, rear line in 0x19, front headphone 0x21;
//   - EAPD (the external amplifier enable) on every output pin that has
//     one, including those the generic code didn't route, so the speaker
//     amplifier is never left off;
//   - its own lines in the audio report (play -i).
//
// Built as the driver file hda-10ec0221 (build-drivers.sh), installed in
// <home>/sys/drivers/. The HDA driver loads it by itself at boot when it is
// there; "load <home>/sys/drivers/hda-10ec0221" in the console works too,
// and "unload hda-10ec0221" puts the codec back on the generic setup.
//
// Kernel side: a platform device "hda-10ec0221" is added at load; probe
// registers the codec driver, remove takes it out again.
//

#include <kdrv_module.h>
#include <kernel/arch/x86/drivers/audio/hda_codec_driver.h>

#define DRV_NAME            "hda-10ec0221"

#define ALC221_ID           0x10EC0221u
#define HP_SUBVENDOR        0x103Cu

// HDA verbs and fields used here (the spec's numbers; hda_regs.h is the
// kernel's own and not part of the driver ABI)
#define V_GET_PARAMETER     0xF00
#define V_GET_EAPD          0xF0C
#define V_SET_EAPD          0x70C
#define V_SET_COEF_INDEX    0x5
#define V_GET_PROC_COEF     0xC
#define PAR_WIDGET_CAPS     0x09
#define PAR_PIN_CAPS        0x0C
#define WCAP_TYPE(c)        (((c) >> 20) & 0xF)
#define WIDGET_PIN          0x4
#define PINCAP_OUTPUT       (1u << 4)
#define PINCAP_EAPD         (1u << 16)
#define EAPD_ENABLE         0x02

#define CFG_PORT(c)         (((c) >> 30) & 0x3)
#define CFG_DEVICE(c)       (((c) >> 20) & 0xF)
#define PORT_NONE           1
#define DEV_LINE_OUT        0x0
#define DEV_SPEAKER         0x1
#define DEV_HP_OUT          0x2

#define REALTEK_COEF_NID    0x20

static const struct KernelApi* kapi;

struct PinDefault {
    uint8_t  nid;
    uint32_t config;
};

// HP's ALC221 desktop layout (Linux: SND_HDA_PIN_QUIRK 10ec0221 / 103c
// "HP Workstation")
static const struct PinDefault hp_desktop_pins[] = {
    { 0x14, 0x01014020 },   // Rear line out, green jack
    { 0x17, 0x90170110 },   // Internal speaker
    { 0x18, 0x02a11030 },   // Front microphone jack
    { 0x19, 0x0181303f },   // Rear line in, blue jack
    { 0x21, 0x0221102f },   // Front headphone jack
    { 0, 0 }
};

// What the last setup did, for the report (one ALC221 per machine)
static bool     used_fallback;
static uint32_t eapd_pins;
static uint32_t coef0;

static bool is_pin(const struct HdaCodecLink* l, uint8_t nid, uint32_t* pin_caps) {
    uint32_t caps = l->verb(l->ctx, nid, V_GET_PARAMETER, PAR_WIDGET_CAPS);
    if (caps == HDA_LINK_VERB_FAILED || WCAP_TYPE(caps) != WIDGET_PIN)
        return false;
    if (pin_caps != NULL) {
        uint32_t pc = l->verb(l->ctx, nid, V_GET_PARAMETER, PAR_PIN_CAPS);
        *pin_caps = (pc == HDA_LINK_VERB_FAILED) ? 0 : pc;
    }
    return true;
}

static bool alc221_match(const struct HdaCodecLink* l) {
    return l->vendor_id == ALC221_ID;
}

// No output the BIOS calls connected: use HP's layout. A BIOS that set
// anything up is left alone; it knows the board.
static void alc221_pins(const struct HdaCodecLink* l) {
    used_fallback = false;
    uint32_t end = (uint32_t)l->first_nid + l->node_count;
    for (uint32_t nid = l->first_nid; nid < end && nid < 0x80; nid++) {
        uint32_t cfg = l->pin_config(l->ctx, (uint8_t)nid);
        if (cfg == 0 || CFG_PORT(cfg) == PORT_NONE)
            continue;
        uint32_t dev = CFG_DEVICE(cfg);
        if (dev == DEV_LINE_OUT || dev == DEV_SPEAKER || dev == DEV_HP_OUT)
            return;
    }
    for (const struct PinDefault* p = hp_desktop_pins; p->nid != 0; p++) {
        if (l->set_pin_config(l->ctx, p->nid, p->config))
            used_fallback = true;
    }
    if (used_fallback)
        kapi->log("BIOS left every output unconnected: HP desktop pin layout used");
}

static void alc221_init(const struct HdaCodecLink* l) {
    // Coefficient 0 says which cut of the chip this is; only reported
    l->verb(l->ctx, REALTEK_COEF_NID, V_SET_COEF_INDEX, 0);
    uint32_t v = l->verb(l->ctx, REALTEK_COEF_NID, V_GET_PROC_COEF, 0);
    coef0 = (v == HDA_LINK_VERB_FAILED) ? 0 : (v & 0xFFFF);
}

// EAPD on every output pin that has it: the speaker amplifier on these
// boards hangs off it
static void alc221_ready(const struct HdaCodecLink* l) {
    eapd_pins = 0;
    uint32_t end = (uint32_t)l->first_nid + l->node_count;
    for (uint32_t nid = l->first_nid; nid < end && nid < 0x80; nid++) {
        uint32_t pc = 0;
        if (!is_pin(l, (uint8_t)nid, &pc) || !(pc & PINCAP_OUTPUT) || !(pc & PINCAP_EAPD))
            continue;
        uint32_t e = l->verb(l->ctx, (uint8_t)nid, V_GET_EAPD, 0);
        if (e == HDA_LINK_VERB_FAILED)
            e = 0;
        l->verb(l->ctx, (uint8_t)nid, V_SET_EAPD, (e & 0xFF) | EAPD_ENABLE);
        eapd_pins++;
    }
}

static int append(char* out, uint32_t size, uint32_t used, const char* format, ...) {
    if (used >= size)
        return 0;
    va_list args;
    va_start(args, format);
    int n = kapi->vsnprintf(out + used, size - used, format, args);
    va_end(args);
    return n > 0 ? n : 0;
}

static void alc221_report(const struct HdaCodecLink* l, char* out, uint32_t size) {
    uint32_t used = 0;
    uint16_t board = (uint16_t)(l->subsystem >> 16);
    used += (uint32_t)append(out, size, used, "  %s: ALC221 cut %04x, board %08x%s, controller %04x:%04x\n",
                             DRV_NAME, (unsigned)coef0, (unsigned)l->subsystem,
                             board == HP_SUBVENDOR ? " (HP)" : "", (unsigned)l->pci_vendor,
                             (unsigned)l->pci_device);
    append(out, size, used, "  %s: pins from %s, EAPD on %u pin%s\n", DRV_NAME,
           used_fallback ? "the HP desktop layout (BIOS had none)" : "the BIOS",
           (unsigned)eapd_pins, eapd_pins == 1 ? "" : "s");
}

static const struct HdaCodecDriver alc221_codec = {
    .version = HDA_CODEC_DRIVER_VERSION,
    .size    = sizeof(struct HdaCodecDriver),
    .name    = "Realtek ALC221",
    .flags   = HDA_CODEC_LINE_MUTES_SPEAKER | HDA_CODEC_HP_MUTES_LINE,
    .match   = alc221_match,
    .pins    = alc221_pins,
    .init    = alc221_init,
    .ready   = alc221_ready,
    .report  = alc221_report,
};

//
// The driver file's side: one platform device, bound while loaded
//

static bool registered;

static int alc221_probe(struct Device* dev) {
    (void)dev;
    if (registered || !kapi->hda_codec_register(&alc221_codec))
        return PROBE_FAILED;
    registered = true;
    return PROBE_BOUND;
}

static void alc221_remove(struct Device* dev) {
    (void)dev;
    if (registered)
        kapi->hda_codec_unregister(&alc221_codec);
    registered = false;
}

static const struct DeviceMatch alc221_match_table[] = {
    { .bus = BUS_PLATFORM, .name = DRV_NAME },
    { 0 }
};

static const struct Driver alc221_driver = {
    .name    = DRV_NAME,
    .abi     = DRIVER_ABI,
    .version = "1.0",
    .match   = alc221_match_table,
    .probe   = alc221_probe,
    .remove  = alc221_remove,
};

KDRV_ENTRY(api) {
    kapi = api;
    // A kernel without codec driver support (older than KAPI 3, or no HDA)
    if (api->version < 3 || api->size < offsetof(struct KernelApi, hda_codec_unregister) + sizeof(void*) ||
        api->hda_codec_register == NULL || api->hda_codec_unregister == NULL)
        return NULL;
    // The device stays when the file is unloaded: reuse it on a reload
    if (api->device_find(DRV_NAME) == NULL && api->device_add_platform(DRV_NAME) == NULL)
        return NULL;
    return &alc221_driver;
}
