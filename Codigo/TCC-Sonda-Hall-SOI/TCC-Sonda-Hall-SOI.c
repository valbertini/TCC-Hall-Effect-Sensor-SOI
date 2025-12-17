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
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // ALERT/RDY - Garanta que o pull-up interno está forte o suficiente
    gpio_init(ALERT_PIN);
    gpio_set_dir(ALERT_PIN, GPIO_IN);
    gpio_pull_up(ALERT_PIN);

    gpio_set_irq_enabled_with_callback(
        ALERT_PIN,
        GPIO_IRQ_EDGE_FALL,
        true,
        &alert_isr
    );

    // 1. Configurar Hi_thresh para MSB = 1 e Lo_thresh para MSB = 0
    // Isso ativa o modo Conversion Ready no pino ALERT
    uint8_t hi_thresh[] = {0x03, 0xFF, 0xFF}; 
    uint8_t lo_thresh[] = {0x02, 0x00, 0x00}; 

    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, hi_thresh, 3, false);
    i2c_write_blocking(I2C_PORT, ADS1115_ADDR, lo_thresh, 3, false);

    // 2. Configuração: 
    // Bit 15: 0 (No effect em modo contínuo)
    // Bits 14-12: 100 (AIN0 vs GND)
    // Bits 11-9: 100 (+/- 0.256V)
    // Bit 8: 0 (Modo Contínuo)
    // Bits 7-5: 111 (860 SPS)
    // Bits 4-2: 000 (Modo ALERT tradicional, mas os thresholds acima forçam o RDY)
    // Bit 1: 0 (Polaridade ativa baixa)
    // Bit 0: 0 (Latching desativado - importante para o RDY pulsar em cada amostra)
    
    uint8_t config[3];
    config[0] = REG_CONFIG;
    config[1] = 0xC8; // 1100 1000 -> AIN0, 0.256V, Continuous
    config[2] = 0xE0; // 1110 0000 -> 860 SPS, Alert Queue Enable (necessário para RDY)

    int ret = i2c_write_blocking(I2C_PORT, ADS1115_ADDR, config, 3, false);
    if (ret == PICO_ERROR_GENERIC) {
        printf("Erro: ADS1115 não encontrado no endereço 0x48!\n");
    }
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
    stdio_init_all();
    ads1115_init();

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
        }

        tight_loop_contents();
    }
}
