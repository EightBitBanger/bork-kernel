#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kshim.h"
#include "hda.h"
#include "hda_regs.h"

//
// HDA codecs: reading the widget graph, finding the outputs, routing them
//
// Inputs (microphones, line in) are found the same way from the ADCs and
// routed only while something records (hda_codec_capture).
//
// Generic first: every pin the BIOS marked as a connected speaker,
// headphone or line out gets a path back to an analog DAC through the
// mixers and selectors between them, preferring the DAC the pins before it
// already use, so one stream reaches them all. Every amplifier on a path is
// unmuted at 0 dB except the volume one (the DAC's own when it has steps),
// and mixer inputs off the path (the ALC269's analog loopback from the
// microphones) are muted. Speakers go quiet while headphones are plugged in,
// when the headphone pin can tell.
//
// EAPD (the external amplifier power) goes on for every output pin on a
// path that has it.
//
// A codec driver (hda_codec_driver.h, usually from a driver file) can claim
// a codec: its pins hook fixes the BIOS's pin defaults before the paths
// are found, init / ready run around every setup, and its flags change
// the jack rules (HDA_CODEC_LINE_MUTES_SPEAKER). Everything else is the
// generic code above. Vendor coefficients live there too: the Realtek
// ALC269's (Acer Aspire 7250) in the driver file hda-10ec0269, the ALC221's
// in hda-10ec0221.
//
// The codec's answers are untrusted like any device's: node numbers and
// connection entries are range checked before use, and loops in the graph
// end at HDA_MAX_PATH.
//


#define VERB(cad, nid, v, p)    HDA_VERB((cad), (nid), (v), (p))
#define VERB4(cad, nid, v, p)   HDA_VERB4((cad), (nid), (v), (p))

static uint32_t codec_cmd(struct HdaController* hc, hda_verb_fn verb, uint32_t command) {
    uint32_t response = 0;
    if (!verb(hc, command, &response))
        return 0xFFFFFFFFu;
    return response;
}

static uint32_t codec_param(struct HdaController* hc, hda_verb_fn verb, uint8_t cad, uint8_t nid, uint8_t param) {
    return codec_cmd(hc, verb, VERB(cad, nid, HDA_GET_PARAMETER, param));
}

static void codec_delay_ms(uint32_t ms) {
    uint64_t start = timer_get_ms();
    while (timer_get_ms() - start < ms + 1)
        io_wait();
}

static bool codec_nid_ok(const struct HdaCodec* c, uint32_t nid) {
    return nid >= c->first_nid && nid < (uint32_t)c->first_nid + c->node_count &&
           nid < HDA_MAX_WIDGETS && c->w[nid].caps != 0;
}

static uint32_t codec_type(const struct HdaCodec* c, uint8_t nid) {
    return HDA_WCAP_TYPE(c->w[nid].caps);
}

//
// Reading the graph
//

// The connection list of `nid`: short entries (8 bits, 4 per answer) or
// long ones (16 bits, 2 per answer); bit 7 / 15 marks the end of a range
static void codec_read_connections(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t nid) {
    struct HdaWidget* w = &c->w[nid];
    w->conn_count = 0;
    if (!(w->caps & HDA_WCAP_CONN_LIST))
        return;

    uint32_t len_par = codec_param(hc, verb, c->addr, nid, HDA_PAR_CONN_LEN);
    if (len_par == 0xFFFFFFFFu)
        return;
    bool     long_form = (len_par & 0x80) != 0;
    uint32_t length    = len_par & 0x7F;
    uint32_t per       = long_form ? 2 : 4;
    uint32_t bits      = long_form ? 16 : 8;
    uint32_t range_bit = long_form ? 0x8000u : 0x80u;
    uint32_t mask      = long_form ? 0x7FFFu : 0x7Fu;

    uint32_t previous = 0;
    for (uint32_t i = 0; i < length && w->conn_count < HDA_MAX_CONN; i += per) {
        uint32_t r = codec_cmd(hc, verb, VERB(c->addr, nid, HDA_GET_CONN_LIST, i));
        for (uint32_t j = 0; j < per && i + j < length && w->conn_count < HDA_MAX_CONN; j++) {
            uint32_t entry = (r >> (j * bits)) & (long_form ? 0xFFFFu : 0xFFu);
            uint32_t value = entry & mask;
            if ((entry & range_bit) && previous != 0 && value > previous) {
                // previous+1 .. value (previous was added already)
                for (uint32_t n = previous + 1; n <= value && w->conn_count < HDA_MAX_CONN; n++)
                    w->conn[w->conn_count++] = (uint8_t)(n < 256 ? n : 0);
            } else {
                w->conn[w->conn_count++] = (uint8_t)(value < 256 ? value : 0);
            }
            previous = value;
        }
    }
}

//
// Codec drivers (hda_codec_driver.h): the link their hooks talk through
//

struct HdaLinkCtx {
    struct HdaController* hc;
    hda_verb_fn      verb;
    struct HdaCodec* c;
    bool             pins;          // set_pin_config allowed (the pins hook)
};

static uint32_t link_verb(void* ctx, uint8_t nid, uint32_t v, uint32_t payload) {
    struct HdaLinkCtx* x = (struct HdaLinkCtx*)ctx;
    uint32_t command = (v <= 0xF) ? VERB4(x->c->addr, nid, v, payload & 0xFFFF)
                                  : VERB(x->c->addr, nid, v & 0xFFF, payload & 0xFF);
    return codec_cmd(x->hc, x->verb, command);
}

static uint32_t link_pin_config(void* ctx, uint8_t nid) {
    struct HdaLinkCtx* x = (struct HdaLinkCtx*)ctx;
    if (!codec_nid_ok(x->c, nid) || codec_type(x->c, nid) != HDA_WIDGET_PIN)
        return 0;
    return x->c->w[nid].config;
}

static bool link_set_pin_config(void* ctx, uint8_t nid, uint32_t config) {
    struct HdaLinkCtx* x = (struct HdaLinkCtx*)ctx;
    if (!x->pins || !codec_nid_ok(x->c, nid) || codec_type(x->c, nid) != HDA_WIDGET_PIN)
        return false;
    if (x->c->w[nid].config != config)
        x->c->pins_changed++;
    x->c->w[nid].config = config;
    return true;
}

static uint8_t codec_sense_pin(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t pin);

static uint8_t link_pin_sense(void* ctx, uint8_t nid) {
    struct HdaLinkCtx* x = (struct HdaLinkCtx*)ctx;
    if (!codec_nid_ok(x->c, nid) || codec_type(x->c, nid) != HDA_WIDGET_PIN ||
        !(x->c->w[nid].pin_caps & HDA_PINCAP_PRESENCE))
        return AUDIO_JACK_UNKNOWN;
    return codec_sense_pin(x->hc, x->verb, x->c, nid);
}

static void codec_link_make(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, bool pins,
                            struct HdaLinkCtx* x, struct HdaCodecLink* l) {
    x->hc = hc;
    x->verb = verb;
    x->c = c;
    x->pins = pins;
    memset(l, 0, sizeof(*l));
    l->version        = HDA_CODEC_LINK_VERSION;
    l->size           = sizeof(*l);
    l->vendor_id      = c->vendor_id;
    l->revision       = c->revision;
    l->subsystem      = c->subsystem;
    l->addr           = c->addr;
    l->afg            = c->afg;
    l->first_nid      = c->first_nid;
    l->node_count     = c->node_count;
    l->pci_vendor     = c->pci_vendor;
    l->pci_device     = c->pci_device;
    l->pci_subvendor  = c->pci_subvendor;
    l->pci_subdevice  = c->pci_subdevice;
    l->ctx            = x;
    l->verb           = link_verb;
    l->pin_config     = link_pin_config;
    l->set_pin_config = link_set_pin_config;
    l->pin_sense      = link_pin_sense;
    l->delay_ms       = codec_delay_ms;
}

// Calls one of c->drv's optional hooks
#define CODEC_DRV_HOOK(hc, verb, c, hook) do {                          \
        if ((c)->drv != NULL && (c)->drv->hook != NULL) {               \
            struct HdaLinkCtx x_;                                       \
            struct HdaCodecLink l_;                                     \
            codec_link_make((hc), (verb), (c), false, &x_, &l_);        \
            (c)->drv->hook(&l_);                                        \
        }                                                               \
    } while (0)

bool hda_codec_driver_match(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                            const struct HdaCodecDriver* d) {
    if (d == NULL || d->match == NULL)
        return false;
    struct HdaLinkCtx x;
    struct HdaCodecLink l;
    codec_link_make(hc, verb, c, false, &x, &l);
    return d->match(&l);
}

void hda_codec_driver_release(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    CODEC_DRV_HOOK(hc, verb, c, release);
}

//
// Inputs: microphones and line in
//
// Every analog input pin the BIOS marked as a connected microphone or line
// in gets the shortest path from an ADC to it through the mixers and
// selectors between (so the ALC269's capture mixer 0x23 is taken straight
// to the pin, not round through the playback loopback mixer 0x0b). A
// digital microphone pin (the ALC269's 0x12) is a pin like the others here.
//

static bool codec_input_pin(const struct HdaCodec* c, uint32_t nid, uint8_t* kind) {
    const struct HdaWidget* w = &c->w[nid];
    if (w->caps == 0 || HDA_WCAP_TYPE(w->caps) != HDA_WIDGET_PIN)
        return false;
    if (!(w->pin_caps & HDA_PINCAP_INPUT) || (w->pin_caps & (HDA_PINCAP_HDMI | HDA_PINCAP_DP)))
        return false;
    uint32_t port = HDA_CFG_PORT(w->config), dev = HDA_CFG_DEVICE(w->config);
    if (port == HDA_PORT_NONE)
        return false;
    if (dev == HDA_DEV_MIC_IN)
        *kind = (port == HDA_PORT_FIXED) ? AUDIO_IN_INTERNAL_MIC : AUDIO_IN_MIC;
    else if (dev == HDA_DEV_LINE_IN)
        *kind = AUDIO_IN_LINE;
    else
        return false;
    return true;
}

// Depth-first from `adc` to `pin` within `max` nodes
static bool codec_search_in(const struct HdaCodec* c, uint8_t adc, uint8_t pin, uint32_t max,
                            struct HdaInPath* out) {
    struct HdaInPath p;
    memset(&p, 0, sizeof(p));
    uint8_t stack_idx[HDA_MAX_PATH];
    p.nid[0] = adc;
    p.length = 1;
    stack_idx[0] = 0;
    while (p.length > 0) {
        uint32_t depth = p.length - 1;
        uint8_t  cur   = p.nid[depth];
        const struct HdaWidget* cw = &c->w[cur];
        if (stack_idx[depth] >= cw->conn_count || depth + 1 >= max) {
            p.length--;
            continue;
        }
        uint8_t idx  = stack_idx[depth]++;
        uint8_t next = cw->conn[idx];
        if (!codec_nid_ok(c, next))
            continue;
        bool loop = false;
        for (uint32_t k = 0; k < p.length; k++)
            loop = loop || p.nid[k] == next;
        if (loop)
            continue;
        p.sel[depth] = idx;
        if (next == pin) {
            p.nid[p.length++] = next;
            *out = p;
            return true;
        }
        uint32_t nt = codec_type(c, next);
        if (nt == HDA_WIDGET_MIXER || nt == HDA_WIDGET_SELECTOR) {
            p.nid[p.length] = next;
            stack_idx[p.length] = 0;
            p.length++;
        }
    }
    return false;
}

// The capture gain: an input amp with steps nearest the ADC, not the pin's
// (that one is the microphone boost)
static void codec_in_gain(const struct HdaCodec* c, struct HdaInPath* p) {
    for (uint32_t k = 0; k + 1 < p->length; k++) {
        const struct HdaWidget* w = &c->w[p->nid[k]];
        if ((w->caps & HDA_WCAP_IN_AMP) && HDA_AMPCAP_STEPS(w->amp_in_caps) > 0) {
            uint32_t type = HDA_WCAP_TYPE(w->caps);
            p->gain_nid   = p->nid[k];
            p->gain_index = (type == HDA_WIDGET_MIXER || w->conn_count > 1) ? p->sel[k] : 0;
            return;
        }
    }
}

static void codec_find_inputs(struct HdaCodec* c) {
    c->mic_present = AUDIO_JACK_UNKNOWN;
    static const uint8_t order[3] = { AUDIO_IN_INTERNAL_MIC, AUDIO_IN_MIC, AUDIO_IN_LINE };
    uint32_t start = c->first_nid, end = (uint32_t)c->first_nid + c->node_count;
    for (uint32_t o = 0; o < 3; o++) {
        for (uint32_t pin = start; pin < end && c->in_count < HDA_MAX_INPUTS; pin++) {
            uint8_t kind;
            if (!codec_input_pin(c, pin, &kind) || kind != order[o])
                continue;
            struct HdaInPath best;
            bool found = false;
            // Shortest first: two nodes (ADC, pin), then three ...
            for (uint32_t max = 2; max <= HDA_MAX_PATH && !found; max++) {
                for (uint32_t adc = start; adc < end && !found; adc++) {
                    const struct HdaWidget* aw = &c->w[adc];
                    if (aw->caps == 0 || HDA_WCAP_TYPE(aw->caps) != HDA_WIDGET_INPUT ||
                        (aw->caps & HDA_WCAP_DIGITAL))
                        continue;
                    found = codec_search_in(c, (uint8_t)adc, (uint8_t)pin, max, &best);
                }
            }
            if (!found)
                continue;
            const struct HdaWidget* pw = &c->w[pin];
            best.kind   = kind;
            best.detect = (pw->pin_caps & HDA_PINCAP_PRESENCE) &&
                          !(HDA_CFG_MISC(pw->config) & HDA_CFG_MISC_NO_DETECT) &&
                          HDA_CFG_PORT(pw->config) != HDA_PORT_FIXED;
            codec_in_gain(c, &best);
            c->in[c->in_count++] = best;
            c->inputs |= kind;
        }
    }
    if (c->in_count == 0)
        return;
    c->in_rates = 0x7FF;
    c->in_bits  = 0x1F;
    c->in_has_gain = true;
    for (uint32_t i = 0; i < c->in_count; i++) {
        uint32_t pcm = c->w[c->in[i].nid[0]].pcm;
        c->in_rates &= pcm & 0x7FF;
        c->in_bits  &= (pcm >> 16) & 0x1F;
        if (c->in[i].gain_nid == 0)
            c->in_has_gain = false;
    }
    if (c->in_rates == 0 || c->in_bits == 0) {
        // Paths that share no format: keep the first only
        c->in_count = 1;
        c->inputs   = c->in[0].kind;
        uint32_t pcm = c->w[c->in[0].nid[0]].pcm;
        c->in_rates = pcm & 0x7FF;
        c->in_bits  = (pcm >> 16) & 0x1F;
        c->in_has_gain = c->in[0].gain_nid != 0;
    }
}

bool hda_codec_read(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr) {
    memset(c, 0, sizeof(*c));
    c->addr = addr;
    c->hp_present = AUDIO_JACK_UNKNOWN;
    c->lo_present = AUDIO_JACK_UNKNOWN;

    c->vendor_id = codec_param(hc, verb, addr, 0, HDA_PAR_VENDOR_ID);
    if (c->vendor_id == 0xFFFFFFFFu || c->vendor_id == 0) {
        c->why = "no answer";
        return false;
    }
    c->revision = codec_param(hc, verb, addr, 0, HDA_PAR_REVISION_ID);

    // Function groups under the root
    uint32_t nodes = codec_param(hc, verb, addr, 0, HDA_PAR_NODE_COUNT);
    uint32_t start = (nodes >> 16) & 0xFF, count = nodes & 0xFF;
    for (uint32_t n = start; n < start + count && n < 256 && c->afg == 0; n++) {
        uint32_t type = codec_param(hc, verb, addr, (uint8_t)n, HDA_PAR_FG_TYPE);
        if (type != 0xFFFFFFFFu && (type & 0xFF) == HDA_FG_TYPE_AUDIO)
            c->afg = (uint8_t)n;
    }
    if (c->afg == 0) {
        c->why = "no audio function group";
        return false;
    }

    c->subsystem        = codec_cmd(hc, verb, VERB(addr, c->afg, HDA_GET_SUBSYSTEM_ID, 0));
    c->afg_amp_in_caps  = codec_param(hc, verb, addr, c->afg, HDA_PAR_AMP_IN_CAPS);
    c->afg_amp_out_caps = codec_param(hc, verb, addr, c->afg, HDA_PAR_AMP_OUT_CAPS);
    c->afg_pcm          = codec_param(hc, verb, addr, c->afg, HDA_PAR_PCM);

    // Power the group up before reading the widgets: some codecs answer
    // nothing useful in D3
    codec_cmd(hc, verb, VERB(addr, c->afg, HDA_SET_POWER_STATE, 0));
    codec_delay_ms(10);

    nodes = codec_param(hc, verb, addr, c->afg, HDA_PAR_NODE_COUNT);
    start = (nodes >> 16) & 0xFF;
    count = nodes & 0xFF;
    if (nodes == 0xFFFFFFFFu || start == 0 || start >= HDA_MAX_WIDGETS) {
        c->why = "bad widget range";
        return false;
    }
    if (start + count > HDA_MAX_WIDGETS)
        count = HDA_MAX_WIDGETS - start;
    c->first_nid  = (uint8_t)start;
    c->node_count = (uint8_t)count;

    for (uint32_t nid = start; nid < start + count; nid++) {
        struct HdaWidget* w = &c->w[nid];
        uint32_t caps = codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_WIDGET_CAPS);
        if (caps == 0xFFFFFFFFu)
            caps = 0;
        w->caps = caps;
        if (caps == 0)
            continue;
        uint32_t type = HDA_WCAP_TYPE(caps);

        bool own_amps = (caps & HDA_WCAP_AMP_OVERRIDE) != 0;
        if (caps & HDA_WCAP_IN_AMP)
            w->amp_in_caps = own_amps ? codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_AMP_IN_CAPS) : c->afg_amp_in_caps;
        if (caps & HDA_WCAP_OUT_AMP)
            w->amp_out_caps = own_amps ? codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_AMP_OUT_CAPS) : c->afg_amp_out_caps;

        if (type == HDA_WIDGET_PIN) {
            w->pin_caps = codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_PIN_CAPS);
            w->config   = codec_cmd(hc, verb, VERB(addr, nid, HDA_GET_CONFIG_DEFAULT, 0));
        }
        if (type == HDA_WIDGET_OUTPUT || type == HDA_WIDGET_INPUT) {
            uint32_t pcm = codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_PCM);
            w->pcm = (pcm != 0 && pcm != 0xFFFFFFFFu) ? pcm : c->afg_pcm;
        }
        codec_read_connections(hc, verb, c, (uint8_t)nid);
    }
    return true;
}

bool hda_codec_parse(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr) {
    return hda_codec_read(hc, verb, c, addr) && hda_codec_route(hc, verb, c);
}

bool hda_codec_route(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    // Everything found before (a second route after a driver came or went)
    memset(c->out, 0, sizeof(c->out));
    memset(c->in, 0, sizeof(c->in));
    c->out_count = c->in_count = 0;
    c->outputs = c->inputs = 0;
    c->hp_present = c->lo_present = c->mic_present = AUDIO_JACK_UNKNOWN;
    c->why = NULL;
    c->flags = 0;
    c->pins_changed = 0;
    if (c->drv != NULL) {
        c->flags = c->drv->flags;
        if (c->drv->pins != NULL) {
            struct HdaLinkCtx x;
            struct HdaCodecLink link;
            codec_link_make(hc, verb, c, true, &x, &link);
            c->drv->pins(&link);
        }
    }
    uint32_t start = c->first_nid, count = c->node_count;

    if (c->drv != NULL && c->drv->name[0] != '\0') {
        memcpy(c->name, c->drv->name, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
    } else if ((c->vendor_id >> 16) == 0x10EC)
        snprintf(c->name, sizeof(c->name), "Realtek ALC%x", (unsigned)(c->vendor_id & 0xFFFF));
    else
        snprintf(c->name, sizeof(c->name), "Codec %04x:%04x", (unsigned)(c->vendor_id >> 16),
                 (unsigned)(c->vendor_id & 0xFFFF));

    //
    // Output pins, best first: speakers, then headphones, then line outs
    //
    static const uint8_t order[3] = { HDA_DEV_SPEAKER, HDA_DEV_HP_OUT, HDA_DEV_LINE_OUT };
    uint8_t used_dac = 0;
    for (uint32_t o = 0; o < 3; o++) {
        for (uint32_t nid = start; nid < start + count && c->out_count < HDA_MAX_OUTPUTS; nid++) {
            const struct HdaWidget* w = &c->w[nid];
            if (w->caps == 0 || HDA_WCAP_TYPE(w->caps) != HDA_WIDGET_PIN)
                continue;
            if (!(w->pin_caps & HDA_PINCAP_OUTPUT) || (w->pin_caps & (HDA_PINCAP_HDMI | HDA_PINCAP_DP)) ||
                (w->caps & HDA_WCAP_DIGITAL))
                continue;
            if (HDA_CFG_PORT(w->config) == HDA_PORT_NONE || HDA_CFG_DEVICE(w->config) != order[o])
                continue;

            // Depth-first back to a DAC: the one used already if it can be
            // reached, else any analog one
            struct HdaPath best;
            bool found = false;
            for (int pass = 0; pass < 2 && !found; pass++) {
                if (pass == 0 && used_dac == 0)
                    continue;
                struct HdaPath p;
                memset(&p, 0, sizeof(p));
                uint8_t stack_idx[HDA_MAX_PATH];
                p.nid[0] = (uint8_t)nid;
                p.length = 1;
                stack_idx[0] = 0;
                while (p.length > 0 && !found) {
                    uint32_t depth = p.length - 1;
                    uint8_t  cur   = p.nid[depth];
                    const struct HdaWidget* cw = &c->w[cur];
                    if (stack_idx[depth] >= cw->conn_count || depth + 1 >= HDA_MAX_PATH) {
                        p.length--;     // Dead end: back up
                        continue;
                    }
                    uint8_t idx  = stack_idx[depth]++;
                    uint8_t next = cw->conn[idx];
                    if (!codec_nid_ok(c, next))
                        continue;
                    bool loop = false;
                    for (uint32_t k = 0; k < p.length; k++)
                        loop = loop || p.nid[k] == next;
                    if (loop)
                        continue;
                    uint32_t nt = codec_type(c, next);
                    p.sel[depth] = idx;
                    if (nt == HDA_WIDGET_OUTPUT) {
                        if ((c->w[next].caps & HDA_WCAP_DIGITAL) || (pass == 0 && next != used_dac))
                            continue;
                        p.nid[p.length++] = next;
                        found = true;
                    } else if (nt == HDA_WIDGET_MIXER || nt == HDA_WIDGET_SELECTOR) {
                        p.nid[p.length] = next;
                        stack_idx[p.length] = 0;
                        p.length++;
                    }
                }
                if (found)
                    best = p;
            }
            if (!found)
                continue;

            best.kind = (order[o] == HDA_DEV_SPEAKER) ? AUDIO_OUT_SPEAKER :
                        (order[o] == HDA_DEV_HP_OUT) ? AUDIO_OUT_HEADPHONE : AUDIO_OUT_LINE;
            best.detect = (w->pin_caps & HDA_PINCAP_PRESENCE) &&
                          !(HDA_CFG_MISC(w->config) & HDA_CFG_MISC_NO_DETECT) &&
                          HDA_CFG_PORT(w->config) != HDA_PORT_FIXED;
            // Volume: the amp with steps nearest the DAC
            for (int k = (int)best.length - 1; k >= 0 && best.vol_nid == 0; k--) {
                const struct HdaWidget* vw = &c->w[best.nid[k]];
                if ((vw->caps & HDA_WCAP_OUT_AMP) && HDA_AMPCAP_STEPS(vw->amp_out_caps) > 0)
                    best.vol_nid = best.nid[k];
            }
            if (used_dac == 0)
                used_dac = best.nid[best.length - 1];
            c->out[c->out_count++] = best;
            c->outputs |= best.kind;
        }
    }
    if (c->out_count == 0) {
        c->why = "no analog output pin with a path to a DAC";
        return false;
    }

    // What every DAC used can take
    c->rates = 0x7FF;
    c->bits  = 0x1F;
    c->has_volume = true;
    for (uint32_t i = 0; i < c->out_count; i++) {
        uint32_t pcm = c->w[c->out[i].nid[c->out[i].length - 1]].pcm;
        c->rates &= pcm & 0x7FF;
        c->bits  &= (pcm >> 16) & 0x1F;
        if (c->out[i].vol_nid == 0)
            c->has_volume = false;
    }

    codec_find_inputs(c);
    return true;
}

//
// Setting it up
//

static void codec_set_amp(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t nid,
                          bool output, uint8_t index, uint32_t gain_mute) {
    uint32_t payload = (output ? HDA_AMP_OUT : HDA_AMP_IN) | HDA_AMP_LEFT | HDA_AMP_RIGHT |
                       HDA_AMP_INDEX(index) | gain_mute;
    codec_cmd(hc, verb, VERB4(c->addr, nid, HDA_SET_AMP, payload));
}

// 0 dB, or the top step if the amp has no 0 dB point
static uint32_t codec_amp_0db(uint32_t caps) {
    uint32_t steps = HDA_AMPCAP_STEPS(caps), offset = HDA_AMPCAP_OFFSET(caps);
    return offset <= steps ? offset : steps;
}

static void codec_pin_output(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                             struct HdaPath* p, bool on) {
    uint8_t pin = p->nid[0];
    uint32_t ctl = 0;
    if (on) {
        ctl = HDA_PINCTL_OUT;
        if (p->kind == AUDIO_OUT_HEADPHONE && (c->w[pin].pin_caps & HDA_PINCAP_HP_DRIVE))
            ctl |= HDA_PINCTL_HP;
    }
    codec_cmd(hc, verb, VERB(c->addr, pin, HDA_SET_PIN_CTL, ctl));
    p->enabled = on;
}

void hda_codec_setup(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    codec_cmd(hc, verb, VERB(c->addr, c->afg, HDA_SET_POWER_STATE, 0));
    codec_delay_ms(10);
    CODEC_DRV_HOOK(hc, verb, c, init);

    // Mixer inputs nobody routes: muted, so the analog loopback (microphone
    // straight to the speakers) stays out
    for (uint32_t nid = c->first_nid; nid < (uint32_t)c->first_nid + c->node_count; nid++) {
        const struct HdaWidget* w = &c->w[nid];
        if (w->caps == 0 || HDA_WCAP_TYPE(w->caps) != HDA_WIDGET_MIXER || !(w->caps & HDA_WCAP_IN_AMP))
            continue;
        for (uint32_t i = 0; i < w->conn_count; i++)
            codec_set_amp(hc, verb, c, (uint8_t)nid, false, (uint8_t)i, HDA_AMP_MUTE);
    }

    for (uint32_t i = 0; i < c->out_count; i++) {
        struct HdaPath* p = &c->out[i];
        for (uint32_t k = 0; k < p->length; k++) {
            uint8_t nid = p->nid[k];
            const struct HdaWidget* w = &c->w[nid];
            uint32_t type = HDA_WCAP_TYPE(w->caps);

            if (w->caps & HDA_WCAP_POWER)
                codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_POWER_STATE, 0));

            bool last = (k + 1 == p->length);
            if (!last) {
                if (type == HDA_WIDGET_MIXER) {
                    if (w->caps & HDA_WCAP_IN_AMP)
                        codec_set_amp(hc, verb, c, nid, false, p->sel[k], codec_amp_0db(w->amp_in_caps));
                } else if (w->conn_count > 1) {
                    codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_CONN_SELECT, p->sel[k]));
                    if (w->caps & HDA_WCAP_IN_AMP)
                        codec_set_amp(hc, verb, c, nid, false, p->sel[k], codec_amp_0db(w->amp_in_caps));
                } else if (w->caps & HDA_WCAP_IN_AMP) {
                    codec_set_amp(hc, verb, c, nid, false, 0, codec_amp_0db(w->amp_in_caps));
                }
            }
            if ((w->caps & HDA_WCAP_OUT_AMP) && nid != p->vol_nid)
                codec_set_amp(hc, verb, c, nid, true, 0, codec_amp_0db(w->amp_out_caps));

            if (type == HDA_WIDGET_PIN && (w->pin_caps & HDA_PINCAP_EAPD)) {
                uint32_t eapd = codec_cmd(hc, verb, VERB(c->addr, nid, HDA_GET_EAPD, 0));
                if (eapd == 0xFFFFFFFFu)
                    eapd = 0;
                codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_EAPD, (eapd & 0xFF) | HDA_EAPD_ENABLE));
            }
        }
        codec_pin_output(hc, verb, c, p, true);
    }
    c->hp_present = c->lo_present = AUDIO_JACK_UNKNOWN;
    hda_codec_jack(hc, verb, c);
    CODEC_DRV_HOOK(hc, verb, c, ready);
}

void hda_codec_stream(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t tag, uint16_t format) {
    for (uint32_t i = 0; i < c->out_count; i++) {
        uint8_t dac = c->out[i].nid[c->out[i].length - 1];
        bool seen = false;
        for (uint32_t j = 0; j < i; j++)
            seen = seen || c->out[j].nid[c->out[j].length - 1] == dac;
        if (seen)
            continue;
        if (tag != 0)
            codec_cmd(hc, verb, VERB4(c->addr, dac, HDA_SET_FORMAT, format));
        codec_cmd(hc, verb, VERB(c->addr, dac, HDA_SET_STREAM_ID, (uint32_t)(tag & 0xF) << 4));
    }
}

// The amps on a path other than the volume one, at 0 dB or muted. The
// volume amp often can't mute (the ALC269's DACs have steps and no mute
// bit; the mute is on the mixer input and the pin), so mute goes on every
// amp along the way that can.
static void codec_path_quiet(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                             const struct HdaPath* p, bool quiet) {
    for (uint32_t k = 0; k < p->length; k++) {
        uint8_t nid = p->nid[k];
        const struct HdaWidget* w = &c->w[nid];
        uint32_t type = HDA_WCAP_TYPE(w->caps);
        bool last = (k + 1 == p->length);
        if (!last && (w->caps & HDA_WCAP_IN_AMP)) {
            // The input the path takes, as hda_codec_setup set it
            uint8_t index = (type == HDA_WIDGET_MIXER || w->conn_count > 1) ? p->sel[k] : 0;
            uint32_t caps = w->amp_in_caps;
            uint32_t gm = codec_amp_0db(caps);
            if (quiet && (caps & HDA_AMPCAP_MUTE))
                gm |= HDA_AMP_MUTE;
            codec_set_amp(hc, verb, c, nid, false, index, gm);
        }
        if ((w->caps & HDA_WCAP_OUT_AMP) && nid != p->vol_nid) {
            uint32_t caps = w->amp_out_caps;
            uint32_t gm = codec_amp_0db(caps);
            if (quiet && (caps & HDA_AMPCAP_MUTE))
                gm |= HDA_AMP_MUTE;
            codec_set_amp(hc, verb, c, nid, true, 0, gm);
        }
    }
}

void hda_codec_volume(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t left, uint8_t right, bool mute) {
    if (left > 100)  left = 100;
    if (right > 100) right = 100;
    bool silent = mute || (left == 0 && right == 0);
    for (uint32_t i = 0; i < c->out_count; i++) {
        struct HdaPath* p = &c->out[i];
        codec_path_quiet(hc, verb, c, p, silent);
        uint8_t nid = p->vol_nid;
        if (nid == 0)
            continue;               // No steps anywhere: the mutes above are all there is
        uint32_t caps = c->w[nid].amp_out_caps;
        uint32_t top  = codec_amp_0db(caps);
        if (top == 0)
            top = HDA_AMPCAP_STEPS(caps);
        const uint8_t side_vol[2] = { left, right };
        for (int side = 0; side < 2; side++) {
            uint32_t v = side_vol[side];
            uint32_t g = (top * v + 50) / 100;
            uint32_t payload = HDA_AMP_OUT | (side == 0 ? HDA_AMP_LEFT : HDA_AMP_RIGHT) | HDA_AMP_GAIN(g);
            if ((mute || v == 0) && (caps & HDA_AMPCAP_MUTE))
                payload |= HDA_AMP_MUTE;
            codec_cmd(hc, verb, VERB4(c->addr, nid, HDA_SET_AMP, payload));
        }
    }
}

//
// Recording
//

static void codec_capture_off(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    for (uint32_t i = 0; i < c->in_count; i++) {
        const struct HdaInPath* p = &c->in[i];
        uint8_t adc = p->nid[0];
        bool seen = false;
        for (uint32_t j = 0; j < i; j++)
            seen = seen || c->in[j].nid[0] == adc;
        if (!seen)
            codec_cmd(hc, verb, VERB(c->addr, adc, HDA_SET_STREAM_ID, 0));
        // Mixer inputs on the way muted again (hda_codec_setup left them so)
        for (uint32_t k = 0; k + 1 < p->length; k++) {
            const struct HdaWidget* w = &c->w[p->nid[k]];
            if (HDA_WCAP_TYPE(w->caps) == HDA_WIDGET_MIXER && (w->caps & HDA_WCAP_IN_AMP))
                codec_set_amp(hc, verb, c, p->nid[k], false, p->sel[k], HDA_AMP_MUTE);
        }
        codec_cmd(hc, verb, VERB(c->addr, p->nid[p->length - 1], HDA_SET_PIN_CTL, 0));
    }
}

// The microphone bias: 80 % where the pin has it, else 50 or 100 %
static uint32_t codec_vref(const struct HdaWidget* w) {
    if (w->pin_caps & HDA_PINCAP_VREF_80)  return HDA_PINCTL_VREF_80;
    if (w->pin_caps & HDA_PINCAP_VREF_50)  return HDA_PINCTL_VREF_50;
    if (w->pin_caps & HDA_PINCAP_VREF_100) return HDA_PINCTL_VREF_100;
    return HDA_PINCTL_VREF_HIZ;
}

void hda_codec_capture(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                       int index, uint8_t tag, uint16_t format) {
    codec_capture_off(hc, verb, c);
    if (index < 0 || index >= (int)c->in_count || tag == 0)
        return;
    const struct HdaInPath* p = &c->in[index];
    for (uint32_t k = 0; k < p->length; k++) {
        uint8_t nid = p->nid[k];
        const struct HdaWidget* w = &c->w[nid];
        uint32_t type = HDA_WCAP_TYPE(w->caps);
        bool pin = (k + 1 == p->length);

        if (w->caps & HDA_WCAP_POWER)
            codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_POWER_STATE, 0));

        if (!pin) {
            bool gain = (nid == p->gain_nid);
            if (type == HDA_WIDGET_MIXER) {
                if ((w->caps & HDA_WCAP_IN_AMP) && !gain)
                    codec_set_amp(hc, verb, c, nid, false, p->sel[k], codec_amp_0db(w->amp_in_caps));
            } else {
                if (w->conn_count > 1)
                    codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_CONN_SELECT, p->sel[k]));
                if ((w->caps & HDA_WCAP_IN_AMP) && !gain)
                    codec_set_amp(hc, verb, c, nid, false, w->conn_count > 1 ? p->sel[k] : 0,
                                  codec_amp_0db(w->amp_in_caps));
            }
            if ((w->caps & HDA_WCAP_OUT_AMP) && type != HDA_WIDGET_INPUT)
                codec_set_amp(hc, verb, c, nid, true, 0, codec_amp_0db(w->amp_out_caps));
        } else {
            // The pin: its input amp is the microphone boost (0 dB at step
            // 0, about +10 dB a step): one step for a microphone
            if (w->caps & HDA_WCAP_IN_AMP) {
                uint32_t caps  = w->amp_in_caps;
                uint32_t steps = HDA_AMPCAP_STEPS(caps);
                uint32_t g = codec_amp_0db(caps);
                if (p->kind != AUDIO_IN_LINE && HDA_AMPCAP_OFFSET(caps) == 0 && steps > 0)
                    g = 1;
                codec_set_amp(hc, verb, c, nid, false, 0, g);
            }
            uint32_t ctl = HDA_PINCTL_IN;
            if (p->kind != AUDIO_IN_LINE)
                ctl |= codec_vref(w);
            codec_cmd(hc, verb, VERB(c->addr, nid, HDA_SET_PIN_CTL, ctl));
        }
    }
    uint8_t adc = p->nid[0];
    codec_cmd(hc, verb, VERB4(c->addr, adc, HDA_SET_FORMAT, format));
    codec_cmd(hc, verb, VERB(c->addr, adc, HDA_SET_STREAM_ID, (uint32_t)(tag & 0xF) << 4));
}

static uint8_t codec_sense_pin(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t pin) {
    if (c->w[pin].pin_caps & HDA_PINCAP_TRIGGER) {
        codec_cmd(hc, verb, VERB(c->addr, pin, HDA_EXEC_PIN_SENSE, 0));
        codec_delay_ms(1);
    }
    uint32_t sense = codec_cmd(hc, verb, VERB(c->addr, pin, HDA_GET_PIN_SENSE, 0));
    if (sense == 0xFFFFFFFFu)
        return AUDIO_JACK_UNKNOWN;
    return (sense & HDA_PIN_SENSE_PRESENT) ? 1 : 0;
}

int hda_codec_capture_pick(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    if (c->in_count == 0)
        return -1;
    int plugged = -1, internal = -1, mic = -1;
    uint8_t present = AUDIO_JACK_UNKNOWN;
    for (uint32_t i = 0; i < c->in_count; i++) {
        const struct HdaInPath* p = &c->in[i];
        if (p->kind == AUDIO_IN_INTERNAL_MIC && internal < 0)
            internal = (int)i;
        if (p->kind != AUDIO_IN_MIC)
            continue;
        if (mic < 0)
            mic = (int)i;
        if (!p->detect)
            continue;
        uint8_t s = codec_sense_pin(hc, verb, c, p->nid[p->length - 1]);
        if (s == AUDIO_JACK_UNKNOWN)
            continue;
        if (present == AUDIO_JACK_UNKNOWN)
            present = 0;
        if (s == 1) {
            present = 1;
            if (plugged < 0)
                plugged = (int)i;
        }
    }
    c->mic_present = present;
    if (plugged >= 0)  return plugged;
    if (internal >= 0) return internal;
    if (mic >= 0)      return mic;
    return 0;
}

void hda_codec_capture_gain(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                            int index, uint8_t level, bool mute) {
    if (index < 0 || index >= (int)c->in_count)
        return;
    const struct HdaInPath* p = &c->in[index];
    if (p->gain_nid == 0)
        return;
    if (level > 100)
        level = 100;
    uint32_t caps = c->w[p->gain_nid].amp_in_caps;
    uint32_t g = (HDA_AMPCAP_STEPS(caps) * (uint32_t)level + 50) / 100;
    uint32_t gm = HDA_AMP_GAIN(g);
    if (mute && (caps & HDA_AMPCAP_MUTE))
        gm |= HDA_AMP_MUTE;
    codec_set_amp(hc, verb, c, p->gain_nid, false, p->gain_index, gm);
}

uint8_t hda_codec_capture_default_gain(const struct HdaCodec* c, int index) {
    if (index < 0 || index >= (int)c->in_count || c->in[index].gain_nid == 0)
        return 100;
    uint32_t caps  = c->w[c->in[index].gain_nid].amp_in_caps;
    uint32_t steps = HDA_AMPCAP_STEPS(caps);
    if (steps == 0)
        return 100;
    return (uint8_t)((codec_amp_0db(caps) * 100 + steps / 2) / steps);
}

// Something plugged into any `kind` pin that can tell; unknown if none can
static uint8_t codec_sense(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t kind) {
    uint8_t state = AUDIO_JACK_UNKNOWN;
    for (uint32_t i = 0; i < c->out_count; i++) {
        const struct HdaPath* p = &c->out[i];
        if (p->kind != kind || !p->detect)
            continue;
        uint8_t pin = p->nid[0];
        if (c->w[pin].pin_caps & HDA_PINCAP_TRIGGER) {
            codec_cmd(hc, verb, VERB(c->addr, pin, HDA_EXEC_PIN_SENSE, 0));
            codec_delay_ms(1);
        }
        uint32_t sense = codec_cmd(hc, verb, VERB(c->addr, pin, HDA_GET_PIN_SENSE, 0));
        if (sense == 0xFFFFFFFFu)
            continue;
        if (state == AUDIO_JACK_UNKNOWN)
            state = 0;
        if (sense & HDA_PIN_SENSE_PRESENT)
            state = 1;
    }
    return state;
}

bool hda_codec_jack(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    uint8_t hp = codec_sense(hc, verb, c, AUDIO_OUT_HEADPHONE);
    uint8_t lo = (c->flags & HDA_CODEC_LINE_MUTES_SPEAKER) ? codec_sense(hc, verb, c, AUDIO_OUT_LINE)
                                                           : AUDIO_JACK_UNKNOWN;
    if (hp == c->hp_present && lo == c->lo_present)
        return false;
    c->hp_present = hp;
    c->lo_present = lo;
    bool speakers_on = (hp != 1 && lo != 1);
    bool lines_on    = !((c->flags & HDA_CODEC_HP_MUTES_LINE) && hp == 1);
    for (uint32_t i = 0; i < c->out_count; i++) {
        struct HdaPath* p = &c->out[i];
        if (p->kind == AUDIO_OUT_SPEAKER && p->enabled != speakers_on)
            codec_pin_output(hc, verb, c, p, speakers_on);
        if (p->kind == AUDIO_OUT_LINE && p->enabled != lines_on)
            codec_pin_output(hc, verb, c, p, lines_on);
    }
    return true;
}

//
// The report (AUDIO_IOCTL_REPORT)
//

static void report_add(char* out, uint32_t size, uint32_t* used, const char* text) {
    while (*text != '\0' && *used + 1 < size)
        out[(*used)++] = *text++;
    out[*used < size ? *used : size - 1] = '\0';
}

static const char* kind_name(uint8_t kind) {
    switch (kind) {
        case AUDIO_OUT_SPEAKER:   return "speaker";
        case AUDIO_OUT_HEADPHONE: return "headphone";
        default:                  return "line out";
    }
}

void hda_codec_report(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      char* out, uint32_t size, uint32_t* used) {
    char line[160];
    snprintf(line, sizeof(line), "codec %u: %s (%08x rev %08x) subsystem %08x afg 0x%02x nodes 0x%02x-0x%02x\n",
             (unsigned)c->addr, c->name, (unsigned)c->vendor_id, (unsigned)c->revision,
             (unsigned)c->subsystem, (unsigned)c->afg, (unsigned)c->first_nid,
             (unsigned)(c->first_nid + c->node_count - 1));
    report_add(out, size, used, line);

    // Every pin with what the BIOS says about it
    for (uint32_t nid = c->first_nid; nid < (uint32_t)c->first_nid + c->node_count; nid++) {
        const struct HdaWidget* w = &c->w[nid];
        if (w->caps == 0 || HDA_WCAP_TYPE(w->caps) != HDA_WIDGET_PIN)
            continue;
        snprintf(line, sizeof(line), "  pin 0x%02x config %08x caps %08x conn %u\n",
                 (unsigned)nid, (unsigned)w->config, (unsigned)w->pin_caps, (unsigned)w->conn_count);
        report_add(out, size, used, line);
    }
    for (uint32_t i = 0; i < c->out_count; i++) {
        const struct HdaPath* p = &c->out[i];
        int n = snprintf(line, sizeof(line), "  %s:", kind_name(p->kind));
        for (uint32_t k = 0; k < p->length && n > 0 && (uint32_t)n < sizeof(line); k++)
            n += snprintf(line + n, sizeof(line) - (uint32_t)n, " 0x%02x", (unsigned)p->nid[k]);
        if (n > 0 && (uint32_t)n < sizeof(line))
            snprintf(line + n, sizeof(line) - (uint32_t)n, "  volume 0x%02x%s%s\n", (unsigned)p->vol_nid,
                     p->detect ? "  detect" : "", p->enabled ? "  on" : "  off");
        report_add(out, size, used, line);
    }
    for (uint32_t i = 0; i < c->in_count; i++) {
        const struct HdaInPath* p = &c->in[i];
        const char* kind = p->kind == AUDIO_IN_INTERNAL_MIC ? "internal mic" :
                           p->kind == AUDIO_IN_MIC ? "mic jack" : "line in";
        int n = snprintf(line, sizeof(line), "  %s:", kind);
        for (uint32_t k = 0; k < p->length && n > 0 && (uint32_t)n < sizeof(line); k++)
            n += snprintf(line + n, sizeof(line) - (uint32_t)n, " 0x%02x", (unsigned)p->nid[k]);
        if (n > 0 && (uint32_t)n < sizeof(line))
            snprintf(line + n, sizeof(line) - (uint32_t)n, "  gain 0x%02x/%u%s\n", (unsigned)p->gain_nid,
                     (unsigned)p->gain_index, p->detect ? "  detect" : "");
        report_add(out, size, used, line);
    }
    if (c->in_count > 0) {
        snprintf(line, sizeof(line), "  input rates %03x bits %02x mic jack %s\n", (unsigned)c->in_rates,
                 (unsigned)c->in_bits, c->mic_present == 1 ? "in" : c->mic_present == 0 ? "out" : "unknown");
        report_add(out, size, used, line);
    }
    snprintf(line, sizeof(line), "  rates %03x bits %02x headphone %s\n", (unsigned)c->rates, (unsigned)c->bits,
             c->hp_present == 1 ? "in" : c->hp_present == 0 ? "out" : "unknown");
    report_add(out, size, used, line);
    if (c->drv == NULL)
        return;
    snprintf(line, sizeof(line), "  codec driver: %s, %u pin default%s replaced%s%s, line out jack %s\n",
             c->drv->name, (unsigned)c->pins_changed, c->pins_changed == 1 ? "" : "s",
             (c->flags & HDA_CODEC_LINE_MUTES_SPEAKER) ? ", line out mutes speaker" : "",
             (c->flags & HDA_CODEC_HP_MUTES_LINE) ? ", headphones mute line out" : "",
             c->lo_present == 1 ? "in" : c->lo_present == 0 ? "out" : "unknown");
    report_add(out, size, used, line);
    if (hc != NULL && verb != NULL && c->drv->report != NULL) {
        char extra[384];
        extra[0] = '\0';
        struct HdaLinkCtx x;
        struct HdaCodecLink l;
        codec_link_make(hc, verb, c, false, &x, &l);
        c->drv->report(&l, extra, sizeof(extra));
        extra[sizeof(extra) - 1] = '\0';
        report_add(out, size, used, extra);
    }
}
