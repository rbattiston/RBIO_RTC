#include "ds3231.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "ds3231";
static bool s_initialized = false;

/* BCD helpers */
static uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

esp_err_t ds3231_init(void)
{
    if (s_initialized) return ESP_OK;

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = DS3231_SDA_PIN,
        .scl_io_num       = DS3231_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,  /* 100 kHz — conservative for long wires */
    };

    esp_err_t err = i2c_param_config(DS3231_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(DS3231_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Probe: read register 0x00 to confirm DS3231 is present */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    uint8_t probe;
    i2c_master_read_byte(cmd, &probe, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(DS3231_I2C_PORT, cmd, pdMS_TO_TICKS(500));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 not found on I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DS3231 detected on I2C%d (SDA=%d, SCL=%d)",
             DS3231_I2C_PORT, DS3231_SDA_PIN, DS3231_SCL_PIN);
    s_initialized = true;
    return ESP_OK;
}

/* Read a single register */
static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DS3231_I2C_PORT, cmd, pdMS_TO_TICKS(500));
    i2c_cmd_link_delete(cmd);
    return err;
}

/* Write a single register */
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DS3231_I2C_PORT, cmd, pdMS_TO_TICKS(500));
    i2c_cmd_link_delete(cmd);
    return err;
}

esp_err_t ds3231_get_time(struct tm *t)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t data[7];

    /* Set register pointer to 0x00, then read 7 bytes (sec..year) */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 6, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[6], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(DS3231_I2C_PORT, cmd, pdMS_TO_TICKS(500));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time: %s", esp_err_to_name(err));
        return err;
    }

    t->tm_sec  = bcd_to_dec(data[0] & 0x7F);
    t->tm_min  = bcd_to_dec(data[1] & 0x7F);
    t->tm_hour = bcd_to_dec(data[2] & 0x3F);  /* 24h mode assumed */
    t->tm_wday = bcd_to_dec(data[3] & 0x07) - 1;  /* DS3231: 1-7, tm: 0-6 */
    t->tm_mday = bcd_to_dec(data[4] & 0x3F);
    t->tm_mon  = bcd_to_dec(data[5] & 0x1F) - 1;  /* DS3231: 1-12, tm: 0-11 */
    t->tm_year = bcd_to_dec(data[6]) + 100;         /* DS3231: 0-99, tm: years since 1900 */
    t->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *t)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t data[8];
    data[0] = 0x00;  /* starting register */
    data[1] = dec_to_bcd(t->tm_sec);
    data[2] = dec_to_bcd(t->tm_min);
    data[3] = dec_to_bcd(t->tm_hour);
    data[4] = dec_to_bcd(t->tm_wday + 1);
    data[5] = dec_to_bcd(t->tm_mday);
    data[6] = dec_to_bcd(t->tm_mon + 1);
    data[7] = dec_to_bcd(t->tm_year - 100);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, sizeof(data), true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(DS3231_I2C_PORT, cmd, pdMS_TO_TICKS(500));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set time: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    }

    return err;
}

/* Status register: 0x0F.  Bit 7 = OSF (Oscillator Stop Flag). */
#define DS3231_REG_STATUS  0x0F

esp_err_t ds3231_get_osf(bool *osf_out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t status;
    esp_err_t err = read_reg(DS3231_REG_STATUS, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read status register: %s", esp_err_to_name(err));
        return err;
    }

    *osf_out = (status & 0x80) != 0;
    return ESP_OK;
}

esp_err_t ds3231_clear_osf(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t status;
    esp_err_t err = read_reg(DS3231_REG_STATUS, &status);
    if (err != ESP_OK) return err;

    status &= ~0x80;  /* clear OSF bit */
    err = write_reg(DS3231_REG_STATUS, status);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OSF flag cleared");
    }
    return err;
}

/* Temperature registers: 0x11 (MSB, signed), 0x12 (upper 2 bits = fractional) */
#define DS3231_REG_TEMP_MSB  0x11

esp_err_t ds3231_get_temperature(float *temp_out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t msb, lsb;
    esp_err_t err = read_reg(DS3231_REG_TEMP_MSB, &msb);
    if (err != ESP_OK) return err;
    err = read_reg(DS3231_REG_TEMP_MSB + 1, &lsb);
    if (err != ESP_OK) return err;

    /* MSB is signed integer part, upper 2 bits of LSB are 0.25C steps */
    int16_t raw = ((int16_t)(int8_t)msb << 2) | (lsb >> 6);
    *temp_out = raw * 0.25f;
    return ESP_OK;
}
