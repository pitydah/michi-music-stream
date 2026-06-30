#include "esp_log.h"
#include "driver/i2c.h"
#include "pcm5122.h"

static const char *TAG = "pcm5122";

#define I2C_MASTER_SCL  GPIO_NUM_2
#define I2C_MASTER_SDA  GPIO_NUM_1
#define I2C_MASTER_FREQ 100000

static uint8_t reg_read(uint8_t reg)
{
    uint8_t val = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCM5122_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCM5122_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return val;
}

static void reg_write(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCM5122_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
}

bool pcm5122_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA,
        .scl_io_num = I2C_MASTER_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ,
    };
    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %d", err);
        return false;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %d", err);
        return false;
    }

    /* Verificar presencia del DAC */
    uint8_t id = reg_read(0);
    ESP_LOGI(TAG, "PCM5122 ID register: 0x%02x", id);

    /* Configurar formato I2S 24-bit, sin atenuacion, salida habilitada */
    reg_write(0x02, 0x01);  /* auto-mute deshabilitado */
    reg_write(0x09, 0x30);  /* DSP bypass, sin procesamiento */
    reg_write(0x25, 0x00);  /* volume = max (0dB) */
    reg_write(0x26, 0x00);
    reg_write(0x28, 0x00);  /* sin atenuacion de canal */

    reg_write(0x2E, 0x80);  /* salida de audio habilitada */

    ESP_LOGI(TAG, "PCM5122 initialized successfully");
    return true;
}

void pcm5122_set_mute(bool mute)
{
    reg_write(0x02, mute ? 0x11 : 0x01);
}

void pcm5122_set_format(int sample_rate, int bit_depth)
{
    (void)sample_rate;
    (void)bit_depth;
    /* I2S slave, el formato lo determina el maestro */
}
