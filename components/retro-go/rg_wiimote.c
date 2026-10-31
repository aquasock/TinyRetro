/*
 * rg_wiimote.c - Wii Classic Controller and nunchuck over I2C.
 *
 * See rg_wiimote.h for the protocol and why the transactions here are not
 * rg_i2c_read()'s. The button tables come from TinyTang's measured records:
 * a genuine Classic in format 3 reads 84 86 86 86 00 00 ff ff at rest, with
 * A dropping byte 7 bit 4 and B bit 6, and the d-pad on byte 6 (right bit 7,
 * down bit 6) and byte 7 (left bit 1, up bit 0), all active low.
 */
#include "rg_wiimote.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "rg_input.h"
#include "rg_system.h"

static const char *TAG = "rg_wiimote";

#define I2C_PORT_USED   I2C_NUM_0
#define I2C_FREQ_HZ     100000
#define WRITE_STOP_US   200   /* the extension needs a real STOP, then a pause */
#define POLL_MIN_MS     20

static rg_wii_kind_t s_kind;
static bool s_bus_ready;
static int64_t s_last_poll_us;
/* The last decoded mask. A throttled poll returns this rather than nothing:
 * returning "no keys" between reads made the state flicker pressed/released,
 * and retro-go's debounce filter needs consecutive presses, so every press
 * was rejected. Holding the last value keeps the state steady between bus
 * reads. */
static uint32_t s_last_keys;
static uint8_t s_last_id[6];

/* ---- bus primitives ---------------------------------------------- */

/* retro-go installs the I2C driver from rg_i2c_init(), but only its GPIO
 * expander path calls that. This port drives the bus directly, so it has to
 * install it itself -- otherwise every transaction fails with
 * "i2c driver not installed". */
static bool wii_bus_init(void)
{
    if (s_bus_ready)
        return true;

    const i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RG_GPIO_I2C_SDA,
        .scl_io_num = RG_GPIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT_USED, &cfg);
    if (err != ESP_OK)
    {
        RG_LOGE("i2c_param_config failed: 0x%x\n", err);
        return false;
    }
    err = i2c_driver_install(I2C_PORT_USED, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        RG_LOGE("i2c_driver_install failed: 0x%x\n", err);
        return false;
    }
    RG_LOGI("wiimote I2C ready (SDA:%d SCL:%d, %d Hz)\n",
            RG_GPIO_I2C_SDA, RG_GPIO_I2C_SCL, I2C_FREQ_HZ);
    s_bus_ready = true;
    return true;
}

/* Write a register then STOP. No repeated START: the extension's register
 * pointer is latched by a STOP, and a repeated START leaves it ambiguous. */
static bool wii_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!wii_bus_init())
        return false;
    uint8_t buf[8];
    if (len > sizeof(buf) - 1)
        return false;
    buf[0] = reg;
    memcpy(buf + 1, data, len);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RG_WIIMOTE_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, len + 1, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_USED, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

/* Read n bytes from the current register pointer with its own START/STOP. */
static bool wii_read(uint8_t *out, size_t len)
{
    if (!wii_bus_init())
        return false;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RG_WIIMOTE_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, out, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_USED, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

/* The full extension transaction: point at a register, STOP, pause, read. */
static bool wii_read_reg(uint8_t reg, uint8_t *out, size_t len)
{
    if (!wii_write_reg(reg, NULL, 0))
        return false;
    rg_usleep(WRITE_STOP_US);
    return wii_read(out, len);
}

/* ---- bring-up ---------------------------------------------------- */

void rg_wiimote_scan(void)
{
    if (!wii_bus_init())
        return;
    RG_LOGI("I2C scan on SDA:%d SCL:%d\n", RG_GPIO_I2C_SDA, RG_GPIO_I2C_SCL);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_PORT_USED, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK)
        {
            RG_LOGI("  found device at 0x%02X\n", a);
            found++;
        }
    }
    RG_LOGI("I2C scan: %d device(s)\n", found);
}

static bool wii_wake(void)
{
    uint8_t v;

    /* The documented new-initialisation: 0x55 to 0xF0, 0x00 to 0xFB. */
    v = 0x55;
    if (!wii_write_reg(0xF0, &v, 1))
        return false;
    rg_usleep(1000);
    v = 0x00;
    if (!wii_write_reg(0xFB, &v, 1))
        return false;
    rg_usleep(1000);
    return true;
}

static rg_wii_kind_t identify(void)
{
    memset(s_last_id, 0, sizeof(s_last_id));
    if (!wii_read_reg(0xFA, s_last_id, 6))
        return RG_WII_NONE;

    RG_LOGI("identity: %02x %02x %02x %02x %02x %02x\n",
            s_last_id[0], s_last_id[1], s_last_id[2],
            s_last_id[3], s_last_id[4], s_last_id[5]);

    /* Nunchuck: 00 00 A4 20 00 00. Classic and the NES/SNES receivers:
     * 00 00 A4 20 01 01 (format byte differs but byte 4 is 0x01). */
    if (s_last_id[0] == 0x00 && s_last_id[1] == 0x00 && s_last_id[2] == 0xA4 && s_last_id[3] == 0x20)
    {
        if (s_last_id[4] == 0x00)
            return RG_WII_NUNCHUCK;
        return RG_WII_CLASSIC;
    }
    /* All-zero or all-0xff means nothing is there or it is still asleep. */
    bool blank = true;
    for (int i = 0; i < 6; i++)
        if (s_last_id[i] != 0x00 && s_last_id[i] != 0xFF)
            blank = false;
    return blank ? RG_WII_NONE : RG_WII_UNKNOWN;
}

rg_wii_kind_t rg_wiimote_init(void)
{
    if (!wii_wake())
    {
        RG_LOGW("wake write failed; nothing at 0x%02X\n", RG_WIIMOTE_ADDR);
        s_kind = RG_WII_NONE;
        return s_kind;
    }

    rg_wii_kind_t kind = identify();
    if (kind == RG_WII_NONE)
    {
        /* A part plugged in under power needs the wake again. */
        rg_usleep(5000);
        if (wii_wake())
            kind = identify();
    }

    if (kind == RG_WII_CLASSIC)
    {
        /* Data format 3: 8-bit sticks and triggers, the shape the decode
         * expects. A third-party receiver sends nothing usable until this is
         * written, and a controller that lost power comes back in format 1,
         * so the write is verified rather than assumed: after it the identity
         * at 0xFA carries 0x03 in its format byte (WII-004). */
        uint8_t fmt = 0x03;
        bool armed = false;
        for (int attempt = 0; attempt < 3 && !armed; attempt++)
        {
            if (!wii_write_reg(0xFE, &fmt, 1))
            {
                RG_LOGW("format write attempt %d failed\n", attempt + 1);
                continue;
            }
            rg_usleep(2000);
            uint8_t id[6] = {0};
            if (wii_read_reg(0xFA, id, 6) && id[4] == 0x03)
                armed = true;
            else
                RG_LOGW("format 3 not confirmed (id[4]=0x%02x, attempt %d)\n",
                        id[4], attempt + 1);
        }
        if (armed)
            RG_LOGI("Classic Controller ready (data format 3, confirmed)\n");
        else
            RG_LOGE("Classic Controller did not accept data format 3\n");
    }
    else if (kind == RG_WII_NUNCHUCK)
    {
        RG_LOGI("Nunchuck ready\n");
    }
    else
    {
        RG_LOGW("no recognised controller (kind=%d)\n", (int)kind);
    }

    s_kind = kind;
    return s_kind;
}

const char *rg_wiimote_kind_name(void)
{
    switch (s_kind)
    {
    case RG_WII_NUNCHUCK: return "nunchuck";
    case RG_WII_CLASSIC:  return "classic";
    case RG_WII_UNKNOWN:  return "unknown";
    default:              return "none";
    }
}

/* ---- decoding ---------------------------------------------------- */

/* Classic Controller, data format 3, eight bytes:
 *   [0] LX [1] LY [2] RX [3] RY [4] RT [5] LT
 *   [6] 1 0 Ddown Dright L minus Home plus R
 *   [7] ZL ZR B X A dleft dup 1
 * All buttons active low. */
static uint32_t decode_classic(const uint8_t *d)
{
    uint32_t k = 0;
    if (!(d[6] & 0x40)) k |= RG_KEY_DOWN;     /* bit 6 */
    if (!(d[6] & 0x80)) k |= RG_KEY_RIGHT;    /* bit 7 */
    if (!(d[7] & 0x02)) k |= RG_KEY_LEFT;     /* bit 1 */
    if (!(d[7] & 0x01)) k |= RG_KEY_UP;       /* bit 0 */
    if (!(d[7] & 0x10)) k |= RG_KEY_A;        /* bit 4 */
    if (!(d[7] & 0x40)) k |= RG_KEY_B;        /* bit 6 */
    if (!(d[7] & 0x08)) k |= RG_KEY_X;        /* bit 3 */
    if (!(d[7] & 0x20)) k |= RG_KEY_Y;        /* bit 5 */
    if (!(d[6] & 0x20)) k |= RG_KEY_L;        /* bit 5 */
    if (!(d[6] & 0x02)) k |= RG_KEY_R;        /* bit 1 */
    if (!(d[6] & 0x10)) k |= RG_KEY_SELECT;   /* minus */
    if (!(d[6] & 0x04)) k |= RG_KEY_START;    /* plus  */
    if (!(d[6] & 0x08) || !(d[7] & 0x04))
        k |= RG_KEY_MENU;                     /* Home, treated as the menu key */
    return k;
}

/* Nunchuck, six bytes: stick X, stick Y, three accelerometer bytes, then a
 * byte whose bit 0 is Z and bit 1 is C, active low. Only Z and C are buttons;
 * the stick is read by the pointer path, not here. */
static uint32_t decode_nunchuck(const uint8_t *d)
{
    uint32_t k = 0;
    if (!(d[5] & 0x01)) k |= RG_KEY_A;   /* Z */
    if (!(d[5] & 0x02)) k |= RG_KEY_B;   /* C */
    return k;
}

bool rg_wiimote_read(uint32_t *keys)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_poll_us < POLL_MIN_MS * 1000)
    {
        /* Too soon to hit the bus again: report what was last seen. */
        if (keys)
            *keys = s_last_keys;
        return true;
    }
    s_last_poll_us = now;

    if (s_kind == RG_WII_NONE || s_kind == RG_WII_UNKNOWN)
    {
        /* Retry the bring-up, but not on every poll: a controller that is
         * absent or asleep would otherwise flood the log and the bus. */
        static int64_t next_try_us;
        if (now < next_try_us)
            return false;
        next_try_us = now + 2000000;
        rg_wiimote_scan();
        rg_wiimote_init();
        if (s_kind == RG_WII_NONE || s_kind == RG_WII_UNKNOWN)
            return false;
    }

    uint8_t d[8] = {0};
    size_t n = (s_kind == RG_WII_CLASSIC) ? 8 : 6;
    if (!wii_read_reg(0x00, d, n))
        return false;

    /* All-0xff is a controller that went to sleep (replugged under power);
     * wake it and try once more. */
    bool blank = true;
    for (size_t i = 0; i < n; i++)
        if (d[i] != 0xFF)
            blank = false;
    if (blank)
    {
        s_kind = RG_WII_NONE;
        return true;
    }

    /* In format 3 the button bytes are 0xff at rest (all active low), so an
     * idle controller reads ff ff and that is correct. A controller that has
     * fallen back to format 1 reports only six bytes, and reading eight from
     * it leaves bytes 6 and 7 at 0x00 -- the shape that decodes as every
     * button held. That is the real fallback signature. */
    if (s_kind == RG_WII_CLASSIC && d[6] == 0x00 && d[7] == 0x00)
    {
        static int64_t next_rearm_us;
        if (now >= next_rearm_us)
        {
            next_rearm_us = now + 2000000;
            RG_LOGW("controller fell back to format 1; re-arming\n");
            s_kind = RG_WII_NONE;
        }
        return true;
    }

    /* Log every change in the raw report. A controller whose button table is
     * not yet confirmed is diagnosed by watching which bit moves when each
     * button is pressed, so this prints the eight bytes on any change. */
#ifdef RG_WIIMOTE_TRACE
    {
        static uint8_t prev[8];
        static bool have_prev;
        if (!have_prev || memcmp(prev, d, n) != 0)
        {
            have_prev = true;
            memcpy(prev, d, n);
            RG_LOGI("raw: %02x %02x %02x %02x %02x %02x %02x %02x -> keys %03x\n",
                    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                    (unsigned)((s_kind == RG_WII_CLASSIC) ? decode_classic(d) : decode_nunchuck(d)));
        }
    }
#endif

    s_last_keys = (s_kind == RG_WII_CLASSIC) ? decode_classic(d) : decode_nunchuck(d);
    if (keys)
        *keys = s_last_keys;
    return true;
}
