// TLV320DAC3100 driver (task spec FR-1, FR-2, FR-5). The DAC has no MCLK
// wired, so its PLL runs from the I2S bit clock: 16-bit stereo frames make
// BCLK = 32 * fs, and the PLL multiplies that to NDAC * MDAC * DOSR * fs.
//
// Values verified against the datasheet's constraints (D = 0):
//   512 kHz <= BCLK / P <= 20 MHz, 80 MHz <= BCLK * R * J / P <= 110 MHz,
//   4 <= R * J <= 259, DAC_MOD_CLK = 128 * fs <= 6.758 MHz.
// 22.05 and 48 kHz are Adafruit's BCLK table; 32 kHz is derived the same way.
// Every bus transfer goes through the IDF i2c_master driver, which holds
// the bus lock, so the button task's expander reads and these writes never
// collide. The DAC is written only at start, sleep and wake.

#include "dac_tlv320.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board.h"
#include "bsp_i2c.h"

static const char *TAG = "dac";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_pr, s_ndac, s_mdac;

typedef struct {
    uint32_t rate;
    uint8_t p, r, j, ndac, mdac;
    uint16_t dosr;
} clk_t;

static const clk_t s_clk[] = {
    { 22050, 1, 4, 38, 19, 1, 256 },    // 705.6 kHz * 152 = 107.25 MHz / 19 = 256 * fs
    { 32000, 1, 2, 48, 24, 1, 128 },    // 1.024 MHz * 96 = 98.30 MHz / 24 = 128 * fs
    { 48000, 1, 2, 34, 17, 1, 128 },    // 1.536 MHz * 68 = 104.45 MHz / 17 = 128 * fs
};

// Page 0
#define P0_RESET        0x01
#define P0_CLK_MUX      0x04    // [3:2] PLL_CLKIN, [1:0] CODEC_CLKIN
#define P0_PLL_PR       0x05    // [7] power, [6:4] P, [3:0] R
#define P0_PLL_J        0x06
#define P0_PLL_D_MSB    0x07
#define P0_PLL_D_LSB    0x08
#define P0_NDAC         0x0B    // [7] power, [6:0] divider
#define P0_MDAC         0x0C
#define P0_DOSR_MSB     0x0D
#define P0_DOSR_LSB     0x0E
#define P0_IFACE1       0x1B    // [7:6] I2S, [5:4] 16 bit, [3:2] BCLK/WCLK inputs
#define P0_DATAPATH     0x3F    // [7:6] L/R DAC power, [5:4] [3:2] paths
#define P0_VOL_CTRL     0x40    // [3:2] L/R mute
#define P0_LVOL         0x41    // signed, 0.5 dB steps
#define P0_RVOL         0x42
#define P0_VOL_PIN      0x74    // [7] the VOL/MICDET pin's ADC
// Page 1
#define P1_HP_DRIVERS   0x1F    // [7:6] HPL/HPR power, [4:3] common mode, [2] write 1
#define P1_SPK_AMP      0x20    // [7] class-D power
#define P1_HP_POP       0x21    // [6:3] power-on time, [2:1] ramp step
#define P1_ROUTING      0x23    // [7:6] DAC_L, [3:2] DAC_R: 01 to the mixer
#define P1_HPL_VOL      0x24    // [7] route, [6:0] attenuation (table 6-24)
#define P1_HPR_VOL      0x25
#define P1_HPL_DRV      0x28    // [6:3] gain dB, [2] unmute
#define P1_HPR_DRV      0x29

#define HP_ON           (0xC0 | 0x10 | 0x04)    // both drivers, 1.65 V common mode
#define HP_OFF          (0x10 | 0x04)
#define HP_ATTN         0x0C                    // about -6 dB analog (table 6-24); the volume itself is digital
#define DAC_ON          0xD4                    // both DACs, left to left, right to right
#define DAC_OFF         0x14

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

static esp_err_t page(uint8_t p)
{
    return wr(0x00, p);
}

bool dac_probe(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_bus();
    if (!bus) {
        return false;
    }
    if (i2c_master_probe(bus, BOARD_DAC_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "no DAC at 0x%02x", BOARD_DAC_ADDR);
        return false;
    }
    if (!s_dev) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_DAC_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
            return false;
        }
    }
    ESP_LOGI(TAG, "TLV320DAC3100 at 0x%02x", BOARD_DAC_ADDR);
    return true;
}

#define TRY(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(_e)); return _e; } } while (0)

esp_err_t dac_start(uint32_t rate)
{
    const clk_t *c = NULL;
    for (size_t i = 0; i < sizeof(s_clk) / sizeof(s_clk[0]); i++) {
        if (s_clk[i].rate == rate) {
            c = &s_clk[i];
        }
    }
    if (!s_dev || !c) {
        return ESP_ERR_INVALID_ARG;
    }
    TRY(page(0));
    TRY(wr(P0_RESET, 0x01));
    vTaskDelay(pdMS_TO_TICKS(2));
    // Datasheet order: PLL dividers with the PLL off, its input, power it,
    // let it lock, then the codec clock from it and the dividers.
    s_pr = (uint8_t)((c->p & 7) << 4 | (c->r & 15));
    s_ndac = c->ndac;
    s_mdac = c->mdac;
    TRY(wr(P0_PLL_PR, s_pr));
    TRY(wr(P0_PLL_J, c->j));
    TRY(wr(P0_PLL_D_MSB, 0));
    TRY(wr(P0_PLL_D_LSB, 0));
    TRY(wr(P0_CLK_MUX, 0x04));              // PLL_CLKIN = BCLK
    TRY(wr(P0_PLL_PR, 0x80 | s_pr));
    vTaskDelay(pdMS_TO_TICKS(10));
    TRY(wr(P0_CLK_MUX, 0x07));              // CODEC_CLKIN = PLL_CLK
    TRY(wr(P0_IFACE1, 0x00));               // I2S, 16 bit, BCLK and WCLK in
    TRY(wr(P0_NDAC, 0x80 | s_ndac));
    TRY(wr(P0_MDAC, 0x80 | s_mdac));
    TRY(wr(P0_DOSR_MSB, (uint8_t)(c->dosr >> 8)));
    TRY(wr(P0_DOSR_LSB, (uint8_t)c->dosr));
    TRY(wr(P0_VOL_PIN, 0x00));
    // Analog side: DAC to the mixer amplifiers, headphones on, speaker off.
    TRY(page(1));
    TRY(wr(P1_HP_POP, (6 << 3) | (1 << 1)));    // 153 ms power-on, 0.98 ms steps
    TRY(wr(P1_ROUTING, 0x44));
    TRY(wr(P1_HPL_VOL, 0x80 | HP_ATTN));
    TRY(wr(P1_HPR_VOL, 0x80 | HP_ATTN));
    TRY(wr(P1_HPL_DRV, 0x04));              // 0 dB, unmuted
    TRY(wr(P1_HPR_DRV, 0x04));
    TRY(wr(P1_SPK_AMP, 0x06));              // class-D off (reset value)
    TRY(wr(P1_HP_DRIVERS, HP_ON));
    TRY(page(0));
    TRY(wr(P0_DATAPATH, DAC_ON));
    TRY(wr(P0_LVOL, 0));
    TRY(wr(P0_RVOL, 0));
    TRY(wr(P0_VOL_CTRL, 0x00));
    // Read back what decides whether there is sound at all.
    uint8_t j = 0, nd = 0, path = 0;
    TRY(rd(P0_PLL_J, &j));
    TRY(rd(P0_NDAC, &nd));
    TRY(rd(P0_DATAPATH, &path));
    if (j != c->j || nd != (0x80 | s_ndac) || path != DAC_ON) {
        ESP_LOGE(TAG, "readback J %u NDAC %02x path %02x", j, nd, path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "%lu Hz: P %u R %u J %u NDAC %u MDAC %u DOSR %u", (unsigned long)rate, c->p, c->r, c->j,
             c->ndac, c->mdac, c->dosr);
    return ESP_OK;
}

esp_err_t dac_sleep(bool asleep)
{
    if (!s_dev || !s_pr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (asleep) {
        TRY(page(0));
        TRY(wr(P0_VOL_CTRL, 0x0C));         // mute first: no click
        TRY(page(1));
        TRY(wr(P1_HP_DRIVERS, HP_OFF));
        TRY(page(0));
        TRY(wr(P0_DATAPATH, DAC_OFF));
        TRY(wr(P0_NDAC, s_ndac));
        TRY(wr(P0_MDAC, s_mdac));
        TRY(wr(P0_PLL_PR, s_pr));
    } else {
        TRY(page(0));
        TRY(wr(P0_PLL_PR, 0x80 | s_pr));
        vTaskDelay(pdMS_TO_TICKS(10));
        TRY(wr(P0_NDAC, 0x80 | s_ndac));
        TRY(wr(P0_MDAC, 0x80 | s_mdac));
        TRY(wr(P0_DATAPATH, DAC_ON));
        TRY(page(1));
        TRY(wr(P1_HP_DRIVERS, HP_ON));
        TRY(page(0));
        TRY(wr(P0_VOL_CTRL, 0x00));
    }
    return ESP_OK;
}

void dac_stop(void)
{
    if (s_dev && s_pr) {
        dac_sleep(true);
        page(0);
        wr(P0_RESET, 0x01);                 // back to its power-on state
    }
    s_pr = 0;
}
