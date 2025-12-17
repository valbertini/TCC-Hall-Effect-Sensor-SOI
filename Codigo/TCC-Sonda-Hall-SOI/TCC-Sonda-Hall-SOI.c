#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

// I2C
#define I2C_PORT i2c0
#define I2C_SCL  4
#define I2C_SDA  5
#define ADS1115_ADDR 0x48

// ADS1115
#define REG_CONVERSION 0x00
#define REG_CONFIG     0x01

// ALERT
#define ALERT_PIN 6

// Buffer
#define SAMPLE_RATE_SPS   860
#define BUFFER_MS         500
#define AVG_INTERVAL_MS   100
#define BUFFER_SIZE ((SAMPLE_RATE_SPS * BUFFER_MS) / 1000)

// LSB (±0.256V)
#define LSB_VOLT 0.0000078125f

volatile bool conversion_ready = false;

// Buffer circular
int16_t sample_buffer[BUFFER_SIZE];
volatile uint16_t buffer_index = 0;
volatile uint16_t buffer_count = 0;

// ---------- ISR ALERT ----------
void alert_isr(uint gpio, uint32_t events) {
    conversion_ready = true;
}

// ---------- ADS1115 INIT ----------
void ads1115_init() {
    i2c_init(I2C_PORT, 100 * 1000); // Tente 100kHz primeiro para maior estabilidade
    
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // 1. Configurar Thresholds (Necessário para o modo RDY)
    uint8_t hi_thresh[] = {REG_THRESHOLD_HI, 0xFF, 0xFF}; 
    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, hi_thresh, 3, false);
    
    uint8_t lo_thresh[] = {REG_THRESHOLD_LO, 0x00, 0x00}; 
    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, lo_thresh, 3, false);

    // 2. Configurar o Registro de Configuração
    // Vamos usar valores explícitos:
    // MSB: 0100 (AIN0-GND) 100 (+/- 0.256V) 0 (Contínuo) -> 0x48
    // LSB: 111 (860 SPS) 0 (Trad) 0 (Low) 0 (Non-lat) 00 (RDY mode) -> 0xE0
    uint8_t config_data[] = {REG_CONFIG, 0x48, 0xE0};

    int ret = i2c_write_blocking(I2C_PORT, ADS1115_ADDR, config_data, 3, false);
    
    if (ret != 3) {
        printf("ERRO: Falha ao enviar configuração. Bytes enviados: %d\n", ret);
    } else {
        printf("Configuração enviada com sucesso!\n");
    }

    // 3. Verificação (Read-back) imediata
    uint8_t reg_ptr = REG_CONFIG;
    uint8_t read_check[2];
    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, &reg_ptr, 1, true); // Re-aponta para o config
    i2c_read_blocking(I2C_PORT, ADS1115_ADDR, read_check, 2, false);
    
    printf("Confirmando no chip: 0x%02x%02x\n", read_check[0], read_check[1]);
}

// ---------- LEITURA ----------
int16_t ads1115_read() {
    uint8_t reg = REG_CONVERSION;
    uint8_t data[2];

    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, ADS1115_ADDR, data, 2, false);

    return (data[0] << 8) | data[1];
}

// ---------- MÉDIA ----------
float compute_average() {
    int32_t sum = 0;
    uint16_t count = buffer_count;

    for (uint16_t i = 0; i < count; i++) {
        sum += sample_buffer[i];
    }

    return (count > 0) ? (sum / (float)count) * LSB_VOLT : 0.0f;
}

// ---------- MAIN ----------
int main() {
    printf("a");
    sleep_ms(5000);
    stdio_init_all();
    ads1115_init();

    uint8_t reg_ptr = REG_CONFIG;
    uint8_t read_check[2];
    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, &reg_ptr, 1, true); // Re-aponta para o config
    i2c_read_blocking(I2C_PORT, ADS1115_ADDR, read_check, 2, false);
    
    printf("Confirmando no chip: 0x%02x%02x\n", read_check[0], read_check[1]);

    absolute_time_t last_avg = get_absolute_time();

    while (true) {

        if (conversion_ready) {
    	    printf("ALERT\n");
            conversion_ready = false;

            int16_t sample = ads1115_read();

            sample_buffer[buffer_index] = sample;
            buffer_index = (buffer_index + 1) % BUFFER_SIZE;

            if (buffer_count < BUFFER_SIZE)
                buffer_count++;
        }

        if (absolute_time_diff_us(last_avg, get_absolute_time()) >= AVG_INTERVAL_MS * 1000) {
            last_avg = get_absolute_time();

            float avg = compute_average();
            printf("Media (100ms): %.6f V | Amostras: %u\n", avg, buffer_count);
            printf("Confirmando no chip: 0x%02x%02x\n", read_check[0], read_check[1]);

        }

        tight_loop_contents();
    }
}
