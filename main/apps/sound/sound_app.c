// Settings > Sound (hidden app): the audio module's volume, mute and test
// tone (task spec FR-4). The values live in components/audio's own NVS
// namespace and apply on top of every app's volume, at once. Without a DAC
// the page says so and changes nothing.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/ui.h"
#include "audio.h"

enum { ROW_VOLUME, ROW_MUTE, ROW_TONE, ROW_STATUS, ROW_COUNT };

static ui_list_t *s_list;
static bool s_tone;

static void row(int i, ui_row_t *r, void *ctx)
{
    bool dac = audio_available();
    switch (i) {
    case ROW_VOLUME: {
        int v = audio_setting_volume();
        strlcpy(r->title, "Volume", sizeof(r->title));
        strlcpy(r->sub, LV_SYMBOL_LEFT " quieter  " LV_SYMBOL_RIGHT " louder; headphones only", sizeof(r->sub));
        char bar[AUDIO_VOLUME_MAX + 1];
        for (int b = 0; b < AUDIO_VOLUME_MAX; b++) {
            bar[b] = b < v ? '#' : '-';
        }
        bar[AUDIO_VOLUME_MAX] = 0;
        strlcpy(r->right, bar, sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_MUTE: {
        bool m = audio_setting_muted();
        strlcpy(r->title, "Mute", sizeof(r->title));
        strlcpy(r->sub, m ? "Apps play silently" : "Apps can play sound", sizeof(r->sub));
        strlcpy(r->right, m ? "On" : "Off", sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_TONE:
        strlcpy(r->title, "Test tone", sizeof(r->title));
        strlcpy(r->sub, dac ? "440 Hz at a low level" : "Needs the audio DAC", sizeof(r->sub));
        strlcpy(r->right, s_tone ? "Playing" : LV_SYMBOL_PLAY, sizeof(r->right));
        r->right_color = s_tone ? COL_GREEN : COL_TEXT;
        break;
    case ROW_STATUS: {
        strlcpy(r->title, "Audio DAC", sizeof(r->title));
        if (!dac) {
            strlcpy(r->sub, "None found on the I2C bus at boot", sizeof(r->sub));
            strlcpy(r->right, "None", sizeof(r->right));
            r->right_color = COL_TEXT;
            break;
        }
        audio_stats_t st;
        audio_stats(&st);
        if (st.running) {
            snprintf(r->sub, sizeof(r->sub), "%lu Hz, CPU %.1f%%, underruns %lu", (unsigned long)st.rate,
                     (double)st.cpu, (unsigned long)st.underruns);
        } else {
            strlcpy(r->sub, "TLV320DAC3100, idle", sizeof(r->sub));
        }
        strlcpy(r->right, "OK", sizeof(r->right));
        r->right_color = COL_GREEN;
        break;
    }
    }
}

static void enter(lv_obj_t *root)
{
    s_tone = false;
    s_list = ui_list_create(root, 4, OS_ROOT_H - 4 - UI_HINT_H, row, NULL);
    ui_list_set_count(s_list, ROW_COUNT);
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT change  CANCEL exit");
}

static void leave(void)
{
    if (s_tone) {
        audio_test_tone(false);
        s_tone = false;
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    int i = ui_list_selected(s_list);
    if (k == OS_KEY_BACK) {
        return false;                   // CANCEL exits; on_exit stops the tone
    }
    if (i == ROW_VOLUME && (k == OS_KEY_LEFT || k == OS_KEY_RIGHT || (k == OS_KEY_SELECT && !repeat))) {
        int v = audio_setting_volume() + (k == OS_KEY_LEFT ? -1 : 1);
        if (k == OS_KEY_SELECT && v > AUDIO_VOLUME_MAX) {
            v = 0;
        }
        if (v >= 0 && v <= AUDIO_VOLUME_MAX) {
            audio_setting_set_volume(v);
            ui_list_refresh(s_list);
        }
        return true;
    }
    bool change = k == OS_KEY_SELECT || ((k == OS_KEY_LEFT || k == OS_KEY_RIGHT) && i == ROW_MUTE);
    if (!change || repeat) {
        return true;
    }
    switch (i) {
    case ROW_MUTE:
        audio_setting_set_muted(!audio_setting_muted());
        break;
    case ROW_TONE:
        if (!audio_available()) {
            os_toast("No audio DAC on this badge");
            break;
        }
        s_tone = !s_tone;
        if (audio_test_tone(s_tone) < 0) {
            s_tone = false;
            os_toast("Audio didn't start");
        }
        break;
    }
    ui_list_refresh(s_list);
    return true;
}

static void on_tick(void)
{
    static int n;
    if (s_tone && ++n % 10 == 0) {
        ui_list_refresh(s_list);        // the status row's CPU figure, once a second
    }
}

const os_app_t app_sound = {
    .id = "sound",
    .name = "Sound",
    .symbol = LV_SYMBOL_AUDIO,
    .accent = COL_BLUE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
