#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/audio/hda.h>
#include <kernel/arch/x86/drivers/audio/hda_regs.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>
#include <kernel/arch/x86/io.h>

//
// HDA codecs: reading the widget graph, finding the outputs, routing them
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
// Then the Realtek ALC269 (10ec:0269): its vendor coefficients for the
// ALC269VB, as the Linux driver sets them, and EAPD (the external amplifier
// power) on both output pins.
//
// The codec's answers are untrusted like any device's: node numbers and
// connection entries are range checked before use, and loops in the graph
// end at HDA_MAX_PATH.
//

#define REALTEK_ALC269      0x10EC0269u
#define REALTEK_COEF_NID    0x20

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

bool hda_codec_parse(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr) {
    memset(c, 0, sizeof(*c));
    c->addr = addr;
    c->hp_present = AUDIO_JACK_UNKNOWN;

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
        if (type == HDA_WIDGET_OUTPUT) {
            uint32_t pcm = codec_param(hc, verb, addr, (uint8_t)nid, HDA_PAR_PCM);
            w->pcm = (pcm != 0 && pcm != 0xFFFFFFFFu) ? pcm : c->afg_pcm;
        }
        codec_read_connections(hc, verb, c, (uint8_t)nid);
    }

    if (c->vendor_id == REALTEK_ALC269)
        strncpy(c->name, "Realtek ALC269", sizeof(c->name) - 1);
    else if ((c->vendor_id >> 16) == 0x10EC)
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

// Realtek vendor coefficients: index then value, on node 0x20
static uint32_t alc_coef_read(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint16_t index) {
    codec_cmd(hc, verb, VERB4(c->addr, REALTEK_COEF_NID, HDA_SET_COEF_INDEX, index));
    return codec_cmd(hc, verb, VERB4(c->addr, REALTEK_COEF_NID, HDA_GET_PROC_COEF, 0)) & 0xFFFF;
}

static void alc_coef_write(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint16_t index, uint16_t value) {
    codec_cmd(hc, verb, VERB4(c->addr, REALTEK_COEF_NID, HDA_SET_COEF_INDEX, index));
    codec_cmd(hc, verb, VERB4(c->addr, REALTEK_COEF_NID, HDA_SET_PROC_COEF, value));
}

// The ALC269VB (coefficient 0 bits 7:4 = 1) needs these on every start
// (alc269_fill_coef in Linux); the other ALC269 cuts need none
static void alc269_fixup(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    if (c->vendor_id != REALTEK_ALC269 || !codec_nid_ok(c, REALTEK_COEF_NID))
        return;
    uint32_t coef0 = alc_coef_read(hc, verb, c, 0);
    if ((coef0 & 0x00F0) != 0x0010)
        return;
    uint32_t cut = coef0 & 0x00FF;
    if (cut < 0x015) {
        alc_coef_write(hc, verb, c, 0x0F, 0x960B);
        alc_coef_write(hc, verb, c, 0x0E, 0x8817);
    }
    if (cut == 0x016) {
        alc_coef_write(hc, verb, c, 0x0F, 0x960B);
        alc_coef_write(hc, verb, c, 0x0E, 0x8814);
    }
    if (cut == 0x018) {
        uint32_t v = alc_coef_read(hc, verb, c, 0x0D);
        if (((v & 0x0C00) >> 10) != 0x1)
            alc_coef_write(hc, verb, c, 0x0D, (uint16_t)(v | (1u << 10)));    // Capless ramp-up clock
        v = alc_coef_read(hc, verb, c, 0x17);
        if (((v & 0x01C0) >> 6) != 0x4)
            alc_coef_write(hc, verb, c, 0x17, (uint16_t)(v | (1u << 7)));     // Class D power-on reset
    }
    // Headphone amp power (coefficient 4 bit 11), every cut
    uint32_t v = alc_coef_read(hc, verb, c, 0x04);
    alc_coef_write(hc, verb, c, 0x04, (uint16_t)(v | (1u << 11)));
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
    alc269_fixup(hc, verb, c);

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
    hda_codec_jack(hc, verb, c);
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

void hda_codec_volume(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t left, uint8_t right, bool mute) {
    if (left > 100)  left = 100;
    if (right > 100) right = 100;
    for (uint32_t i = 0; i < c->out_count; i++) {
        struct HdaPath* p = &c->out[i];
        uint8_t nid = p->vol_nid;
        if (nid == 0) {
            // No steps anywhere: the pin's own mute is all there is
            const struct HdaWidget* pw = &c->w[p->nid[0]];
            if (pw->caps & HDA_WCAP_OUT_AMP)
                codec_set_amp(hc, verb, c, p->nid[0], true, 0,
                              (mute || (left == 0 && right == 0)) ? HDA_AMP_MUTE : codec_amp_0db(pw->amp_out_caps));
            continue;
        }
        uint32_t caps = c->w[nid].amp_out_caps;
        uint32_t top  = codec_amp_0db(caps);
        if (top == 0)
            top = HDA_AMPCAP_STEPS(caps);
        const uint8_t side_vol[2] = { left, right };
        for (int side = 0; side < 2; side++) {
            uint32_t v = side_vol[side];
            uint32_t g = (top * v + 50) / 100;
            uint32_t payload = HDA_AMP_OUT | (side == 0 ? HDA_AMP_LEFT : HDA_AMP_RIGHT) | HDA_AMP_GAIN(g);
            if (mute || v == 0)
                payload |= HDA_AMP_MUTE;
            codec_cmd(hc, verb, VERB4(c->addr, nid, HDA_SET_AMP, payload));
        }
    }
}

// Headphones present on any headphone pin that can tell; unknown if none can
static uint8_t codec_sense(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c) {
    uint8_t state = AUDIO_JACK_UNKNOWN;
    for (uint32_t i = 0; i < c->out_count; i++) {
        const struct HdaPath* p = &c->out[i];
        if (p->kind != AUDIO_OUT_HEADPHONE || !p->detect)
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
    uint8_t state = codec_sense(hc, verb, c);
    if (state == c->hp_present)
        return false;
    c->hp_present = state;
    bool speakers_on = (state != 1);
    for (uint32_t i = 0; i < c->out_count; i++) {
        struct HdaPath* p = &c->out[i];
        if (p->kind == AUDIO_OUT_SPEAKER && p->enabled != speakers_on)
            codec_pin_output(hc, verb, c, p, speakers_on);
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

void hda_codec_report(const struct HdaCodec* c, char* out, uint32_t size, uint32_t* used) {
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
    snprintf(line, sizeof(line), "  rates %03x bits %02x headphone %s\n", (unsigned)c->rates, (unsigned)c->bits,
             c->hp_present == 1 ? "in" : c->hp_present == 0 ? "out" : "unknown");
    report_add(out, size, used, line);
}
