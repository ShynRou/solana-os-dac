// audio_port over the badge's I2S0 and the TLV320DAC3100 (task spec FR-1 to
// FR-5). I2S0 runs as the clock master (BCLK = 32 * fs, no MCLK pin); the
// DAC's PLL locks to BCLK, so I2S starts before the DAC is configured. DMA:
// CONFIG_SOLANA_AUDIO_DMA_BLOCKS blocks of the mixer's block size, cleared
// to silence when the mixer falls behind (an underrun, counted).

#include <stdatomic.h>
#include "audio_port.h"
#include "dac_tlv320.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "board.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static int s_channels, s_block;
static bool s_enabled;
static atomic_uint s_underruns;

static IRAM_ATTR bool on_ovf(i2s_chan_handle_t h, i2s_event_data_t *ev, void *ctx)
{
    // The DMA ran out of written blocks and sent a cleared one.
    atomic_fetch_add_explicit(&s_underruns, 1, memory_order_relaxed);
    return false;
}

bool audio_port_probe(void)
{
    return dac_probe();
}

int audio_port_start(uint32_t rate, int channels, int block)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = CONFIG_SOLANA_AUDIO_DMA_BLOCKS;
    cc.dma_frame_num = block;
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) {
        return -1;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;     // mono: the same sample to both ears
    i2s_event_callbacks_t cbs = { .on_send_q_ovf = on_ovf };
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK ||
            i2s_channel_register_event_callback(s_tx, &cbs, NULL) != ESP_OK ||
            i2s_channel_enable(s_tx) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return -1;
    }
    s_enabled = true;
    s_channels = channels;
    s_block = block;
    if (dac_start(rate) != ESP_OK) {
        ESP_LOGE(TAG, "DAC setup failed");
        audio_port_stop();
        return -1;
    }
    return 0;
}

int audio_port_write(const int16_t *pcm, int frames, uint32_t timeout_ms)
{
    if (!s_tx || !s_enabled) {
        return 0;
    }
    size_t done = 0;
    i2s_channel_write(s_tx, pcm, (size_t)frames * s_channels * sizeof(int16_t), &done, timeout_ms);
    return (int)(done / (s_channels * sizeof(int16_t)));
}

void audio_port_stop(void)
{
    dac_stop();
    if (s_tx) {
        if (s_enabled) {
            i2s_channel_disable(s_tx);
        }
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_enabled = false;
}

void audio_port_sleep(bool asleep)
{
    if (!s_tx || asleep == !s_enabled) {
        return;
    }
    if (asleep) {
        dac_sleep(true);
        i2s_channel_disable(s_tx);
        s_enabled = false;
    } else {
        // BCLK first: the DAC's PLL needs it to lock.
        s_enabled = i2s_channel_enable(s_tx) == ESP_OK;
        dac_sleep(false);
    }
}

uint32_t audio_port_underruns(void)
{
    return atomic_load_explicit(&s_underruns, memory_order_relaxed);
}

uint32_t audio_port_latency(void)
{
    // A full DMA ring: what the mixer has written but the jack hasn't played.
    return (uint32_t)(CONFIG_SOLANA_AUDIO_DMA_BLOCKS * s_block);
}
