//
// Realtek ALC269 codec driver: sound on the Acer Aspire 7250
//
// The Aspire 7250 has an AMD Hudson FCH HD Audio controller (1002:4383)
// with one Realtek ALC269 codec (10ec:0269): internal speakers, a
// headphone jack and a microphone jack. The Radeon's HDMI codec sits on its
// own controller and has no analog output.
//
// The controller is plain Intel HDA, so the HDA driver file (sys/drivers/hda,
// built from drivers/hda) runs it, and its generic codec code finds
// the paths from the BIOS's pin defaults (speakers go quiet while
// headphones are in, as on every codec). This file adds what is particular
// to the ALC269, through the codec driver hooks (hda_codec_driver.h):
//
//   - the ALC269VB's vendor coefficients, set on every start as the Linux
//     driver does (alc269_fill_coef in patch_realtek.c): the VB cuts before
//     0x015 and the 0x016 cut need two analog settings, the 0x018 cut its
//     capless ramp-up clock and class D power-on reset; every cut gets the
//     headphone amplifier's power bit (coefficient 4 bit 11). The VA, VC
//     and VD cuts need none of the rest;
//   - EAPD (the external amplifier enable) on every output pin that has
//     one, including any the generic code didn't route, so the speaker
//     amplifier is never left off;
//   - its own lines in the audio report.
//
// Before this was a file the same fixes were built into hda_codec.c. Without
// the file the ALC269 still plays on the generic setup, but a VB cut may be
// quiet or crackle on the speakers.
//
// Built as the driver file hda-10ec0269 (build-drivers.sh), installed in
// <home>/sys/drivers/. The HDA driver loads it by itself at boot when it is
// there; "load <home>/sys/drivers/hda-10ec0269" in the console works too,
// and "unload hda-10ec0269" puts the codec back on the generic setup.
//
// Kernel side: a platform device "hda-10ec0269" is added at load; probe
// registers the codec driver, remove takes it out again.
//

#include <kdrv_module.h>
#include <kernel/arch/x86/drivers/audio/hda_codec_driver.h>

#define DRV_NAME            "hda-10ec0269"

#define ALC269_ID           0x10EC0269u
#define ACER_SUBVENDOR      0x1025u

// HDA verbs and fields used here (the spec's numbers; hda_regs.h is the
// kernel's own and not part of the driver ABI)
#define V_GET_PARAMETER     0xF00
#define V_GET_EAPD          0xF0C
#define V_SET_EAPD          0x70C
#define V_SET_COEF_INDEX    0x5
#define V_SET_PROC_COEF     0x4
#define V_GET_PROC_COEF     0xC
#define PAR_WIDGET_CAPS     0x09
#define PAR_PIN_CAPS        0x0C
#define WCAP_TYPE(c)        (((c) >> 20) & 0xF)
#define WIDGET_PIN          0x4
#define PINCAP_OUTPUT       (1u << 4)
#define PINCAP_EAPD         (1u << 16)
#define EAPD_ENABLE         0x02

#define REALTEK_COEF_NID    0x20

static const struct KernelApi* kapi;

// What the last setup did, for the report (one ALC269 per machine)
static uint32_t coef0;
static uint32_t fixes;              // FIX_* bits
static uint32_t eapd_pins;

#define FIX_ANALOG          0x01    // Coefficients 0x0E / 0x0F
#define FIX_CAPLESS         0x02    // 0x0D bit 10
#define FIX_CLASS_D         0x04    // 0x17 bit 7
#define FIX_HP_AMP          0x08    // 0x04 bit 11

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

// Realtek vendor coefficients: index then value, on node 0x20.
// HDA_LINK_VERB_FAILED from a read.
static uint32_t coef_read(const struct HdaCodecLink* l, uint16_t index) {
    l->verb(l->ctx, REALTEK_COEF_NID, V_SET_COEF_INDEX, index);
    uint32_t v = l->verb(l->ctx, REALTEK_COEF_NID, V_GET_PROC_COEF, 0);
    return v == HDA_LINK_VERB_FAILED ? v : (v & 0xFFFF);
}

static void coef_write(const struct HdaCodecLink* l, uint16_t index, uint16_t value) {
    l->verb(l->ctx, REALTEK_COEF_NID, V_SET_COEF_INDEX, index);
    l->verb(l->ctx, REALTEK_COEF_NID, V_SET_PROC_COEF, value);
}

static bool coef_node_ok(const struct HdaCodecLink* l) {
    return REALTEK_COEF_NID >= l->first_nid && REALTEK_COEF_NID < (uint32_t)l->first_nid + l->node_count;
}

static bool alc269_match(const struct HdaCodecLink* l) {
    return l->vendor_id == ALC269_ID;
}

static void alc269_init(const struct HdaCodecLink* l) {
    fixes = 0;
    coef0 = 0;
    if (!coef_node_ok(l))
        return;
    uint32_t c0 = coef_read(l, 0);
    if (c0 == HDA_LINK_VERB_FAILED)
        return;
    coef0 = c0;

    // The VB cut (coefficient 0 bits 7:4 = 1) needs these on every start
    if ((c0 & 0x00F0) == 0x0010) {
        uint32_t cut = c0 & 0x00FF;
        if (cut < 0x015) {
            coef_write(l, 0x0F, 0x960B);
            coef_write(l, 0x0E, 0x8817);
            fixes |= FIX_ANALOG;
        }
        if (cut == 0x016) {
            coef_write(l, 0x0F, 0x960B);
            coef_write(l, 0x0E, 0x8814);
            fixes |= FIX_ANALOG;
        }
        if (cut == 0x018) {
            uint32_t v = coef_read(l, 0x0D);
            if (v != HDA_LINK_VERB_FAILED && ((v & 0x0C00) >> 10) != 0x1) {
                coef_write(l, 0x0D, (uint16_t)(v | (1u << 10)));    // Capless ramp-up clock
                fixes |= FIX_CAPLESS;
            }
            v = coef_read(l, 0x17);
            if (v != HDA_LINK_VERB_FAILED && ((v & 0x01C0) >> 6) != 0x4) {
                coef_write(l, 0x17, (uint16_t)(v | (1u << 7)));     // Class D power-on reset
                fixes |= FIX_CLASS_D;
            }
        }
        // Headphone amplifier power (coefficient 4 bit 11)
        uint32_t v = coef_read(l, 0x04);
        if (v != HDA_LINK_VERB_FAILED) {
            coef_write(l, 0x04, (uint16_t)(v | (1u << 11)));
            fixes |= FIX_HP_AMP;
        }
    }
}

// EAPD on every output pin that has it: the speaker amplifier hangs off it
static void alc269_ready(const struct HdaCodecLink* l) {
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

static const char* cut_name(uint32_t c0) {
    switch (c0 & 0x00F0) {
        case 0x0010: return "VB";
        case 0x0020: return "VC";
        case 0x0030: return "VD";
        default:     return "VA";
    }
}

static void alc269_report(const struct HdaCodecLink* l, char* out, uint32_t size) {
    uint32_t used = 0;
    uint16_t board = (uint16_t)(l->subsystem >> 16);
    used += (uint32_t)append(out, size, used, "  %s: ALC269%s (coefficient 0 = %04x), board %08x%s, controller %04x:%04x\n",
                             DRV_NAME, cut_name(coef0), (unsigned)coef0, (unsigned)l->subsystem,
                             board == ACER_SUBVENDOR ? " (Acer)" : "", (unsigned)l->pci_vendor,
                             (unsigned)l->pci_device);
    if (fixes == 0)
        used += (uint32_t)append(out, size, used, "  %s: no coefficient fixes needed for this cut\n", DRV_NAME);
    else
        used += (uint32_t)append(out, size, used, "  %s: coefficients set:%s%s%s%s\n", DRV_NAME,
                                 (fixes & FIX_ANALOG) ? " analog (0e/0f)" : "",
                                 (fixes & FIX_CAPLESS) ? " capless clock" : "",
                                 (fixes & FIX_CLASS_D) ? " class D reset" : "",
                                 (fixes & FIX_HP_AMP) ? " headphone amp" : "");
    append(out, size, used, "  %s: EAPD on %u pin%s\n", DRV_NAME, (unsigned)eapd_pins, eapd_pins == 1 ? "" : "s");
}

static const struct HdaCodecDriver alc269_codec = {
    .version = HDA_CODEC_DRIVER_VERSION,
    .size    = sizeof(struct HdaCodecDriver),
    .name    = "Realtek ALC269",
    .flags   = 0,
    .match   = alc269_match,
    .init    = alc269_init,
    .ready   = alc269_ready,
    .report  = alc269_report,
};

//
// The driver file's side: one platform device, bound while loaded
//

static bool registered;

static int alc269_probe(struct Device* dev) {
    (void)dev;
    if (registered || !kapi->hda_codec_register(&alc269_codec))
        return PROBE_FAILED;
    registered = true;
    return PROBE_BOUND;
}

static void alc269_remove(struct Device* dev) {
    (void)dev;
    if (registered)
        kapi->hda_codec_unregister(&alc269_codec);
    registered = false;
}

static const struct DeviceMatch alc269_match_table[] = {
    { .bus = BUS_PLATFORM, .name = DRV_NAME },
    { 0 }
};

static const struct Driver alc269_driver = {
    .name    = DRV_NAME,
    .abi     = DRIVER_ABI,
    .version = "1.0",
    .match   = alc269_match_table,
    .probe   = alc269_probe,
    .remove  = alc269_remove,
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
    return &alc269_driver;
}
