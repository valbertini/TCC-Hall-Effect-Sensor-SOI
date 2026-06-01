/**
 * ============================================================
 *  MEDIDOR DE TENSÃO HALL — Raspberry Pi Pico
 * ============================================================
 *
 *  - Leitura ADS1115 AI0
 *  - Auto ganho por relés com histerese correta
 *  - Média de 100ms para decisão de ganho
 *  - Display LCD 16x2
 *
 * CONEXÕES:
 *  ADS1115 : SDA=GPIO9  SCL=GPIO8  ADDR=GND(0x48)
 *  Display  : RS=GPIO17 E=GPIO18 D4=GPIO19 D5=GPIO20 D6=GPIO21 D7=GPIO22
 *  Relés    : K1=GPIO2  K2=GPIO3  K3=GPIO4
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/time.h"

/* ===========================================================
 *  PINOS
 * =========================================================== */
#define I2C_PORT    i2c0
#define PIN_SDA     9
#define PIN_SCL     8

#define PIN_LCD_RS  17
#define PIN_LCD_E   18
#define PIN_LCD_D4  19
#define PIN_LCD_D5  20
#define PIN_LCD_D6  21
#define PIN_LCD_D7  22

#define PIN_K1      2
#define PIN_K2      3
#define PIN_K3      4

/* ===========================================================
 *  RESISTORES REAIS — PREENCHA AQUI (em Ohms)
 *
 *  Ganho INA = 5 + 200000 / Req
 *
 *  K1=1 K2=0 K3=0 → menor ganho
 *  K1=1 K2=1 K3=0 → ganho médio-baixo
 *  K1=0 K2=0 K3=0 → ganho médio-alto
 *  K1=1 K2=0 K3=1 → maior ganho
 * =========================================================== */
#define REQ_G0   46000.0f   /* K1=1 K2=0 K3=0 ← ajuste */
#define REQ_G1    2000.0f   /* K1=1 K2=1 K3=0 ← ajuste */
#define REQ_G2     195.0f   /* K1=0 K2=0 K3=0 ← ajuste */
#define REQ_G3      20.0f   /* K1=1 K2=0 K3=1 ← ajuste */

/* Ganho calculado automaticamente: G = 5 + 200000/Req */
#define GAIN_G0  (5.0f + 200000.0f / REQ_G0)
#define GAIN_G1  (5.0f + 200000.0f / REQ_G1)
#define GAIN_G2  (5.0f + 200000.0f / REQ_G2)
#define GAIN_G3  (5.0f + 200000.0f / REQ_G3)

/* ===========================================================
 *  OFFSET POR GANHO — PREENCHA APÓS CALIBRAÇÃO (em mV)
 *  Medir com entrada da sonda em circuito aberto ou campo=0
 * =========================================================== */
#define OFFSET_G0   0.0f   /* ← ajuste */
#define OFFSET_G1   0.0f   /* ← ajuste */
#define OFFSET_G2   0.0f   /* ← ajuste */
#define OFFSET_G3   0.0f   /* ← ajuste */

/* ===========================================================
 *  GANHO MANUAL — AJUSTE AQUI
 *  1 = G~10    (K1=1 K2=0 K3=0)
 *  2 = G~100   (K1=1 K2=1 K3=0)
 *  3 = G~1000  (K1=0 K2=0 K3=0)
 *  4 = G~10000 (K1=1 K2=0 K3=1)
 * =========================================================== */
#define GANHO_MANUAL  2

#define tempo_aq 500

/* ===========================================================
 *  ADS1115
 * =========================================================== */
#define ADS_ADDR        0x48
#define ADS_REG_CONV    0x00
#define ADS_REG_CFG     0x01

/* PGA ±6.144V — cobre toda a faixa possível na saída do INA */
#define ADS_PGA         0x0000
#define ADS_LSB_uV      187.5f
#define ADS_FS_mV       6144.0f

#define ADS_MUX_AIN0    0x4000  /* AIN0 vs GND */
#define ADS_DR_860SPS   0x00E0
#define ADS_MODE_CONT   0x0000
#define ADS_COMP_OFF    0x0003
#define ADS_WAIT_US     1200    /* 1/860SPS ≈ 1.16ms */

/* ===========================================================
 *  AUTO-GANHO
 *
 *  Limites em % do fundo de escala do ADS:
 *    > 90% → desce ganho
 *    <  8% → sobe ganho
 *
 *  Histerese: após uma troca, a tensão precisa SAIR
 *  da zona de histerese antes de poder trocar de novo.
 *  Ex: subiu ganho em 8% → precisa passar de 13% antes
 *      de qualquer nova decisão de troca.
 * =========================================================== */
#define GAIN_HIGH_PCT   0.90f
#define GAIN_LOW_PCT    0.08f
#define GAIN_HYST_PCT   0.05f   /* 5% de margem após troca */

#define GAIN_HIGH_mV    (ADS_FS_mV * GAIN_HIGH_PCT)
#define GAIN_LOW_mV     (ADS_FS_mV * GAIN_LOW_PCT)
#define GAIN_HYST_mV    (ADS_FS_mV * GAIN_HYST_PCT)

/* ===========================================================
 *  TABELA DE GANHOS
 * =========================================================== */
typedef enum {
    G0 = 0,  /* menor ganho */
    G1,
    G2,
    G3,      /* maior ganho */
    G_COUNT
} gain_idx_t;

typedef struct {
    float    gain;      /* 5 + 200k/Req */
    float    offset_mV;
    uint8_t  k1, k2, k3;
} gain_cfg_t;

static const gain_cfg_t GAIN_TABLE[G_COUNT] = {
    /* gain      offset    K1  K2  K3 */
    { GAIN_G0,  OFFSET_G0,  1,  0,  0 },
    { GAIN_G1,  OFFSET_G1,  1,  1,  0 },
    { GAIN_G2,  OFFSET_G2,  0,  0,  0 },
    { GAIN_G3,  OFFSET_G3,  1,  0,  1 },
};

/* ===========================================================
 *  ESTADO GLOBAL
 * =========================================================== */
static gain_idx_t g_gain   = G2;   /* inicia no ganho médio */

/* Histerese: controla se já saiu da zona após última troca */
typedef enum {
    HYST_FREE,       /* livre para trocar */
    HYST_WAIT_UP,    /* esperando sair da zona baixa (subiu ganho) */
    HYST_WAIT_DOWN,  /* esperando sair da zona alta (desceu ganho) */
} hyst_state_t;

static hyst_state_t g_hyst = HYST_FREE;

/* ===========================================================
 *  LCD HD44780 — 4-bit
 * =========================================================== */
static void lcd_pulse_e(void) {
    gpio_put(PIN_LCD_E, 1); sleep_us(1);
    gpio_put(PIN_LCD_E, 0); sleep_us(100);
}
static void lcd_nibble(uint8_t n) {
    gpio_put(PIN_LCD_D4, (n >> 0) & 1);
    gpio_put(PIN_LCD_D5, (n >> 1) & 1);
    gpio_put(PIN_LCD_D6, (n >> 2) & 1);
    gpio_put(PIN_LCD_D7, (n >> 3) & 1);
    lcd_pulse_e();
}
static void lcd_send(uint8_t val, bool data) {
    gpio_put(PIN_LCD_RS, data ? 1 : 0);
    sleep_us(1);
    lcd_nibble(val >> 4);
    lcd_nibble(val & 0x0F);
    sleep_us(data ? 50 : 2000);
}
#define lcd_cmd(c)  lcd_send((c), false)
#define lcd_char(c) lcd_send((c), true)

static void lcd_init(void) {
    const uint pins[] = { PIN_LCD_RS, PIN_LCD_E,
                          PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7 };
    for (int i = 0; i < 6; i++) {
        gpio_init(pins[i]); gpio_set_dir(pins[i], GPIO_OUT); gpio_put(pins[i], 0);
    }
    sleep_ms(50);
    gpio_put(PIN_LCD_RS, 0);
    lcd_nibble(0x03); sleep_ms(5);
    lcd_nibble(0x03); sleep_ms(1);
    lcd_nibble(0x03); sleep_ms(1);
    lcd_nibble(0x02); sleep_ms(1);
    lcd_cmd(0x28); lcd_cmd(0x0C); lcd_cmd(0x06);
    lcd_cmd(0x01); sleep_ms(2);
}
static void lcd_goto(uint8_t col, uint8_t row) {
    lcd_cmd(0x80 | (col + (row ? 0x40 : 0x00)));
}
static void lcd_str_w(const char *s, int w) {
    int n = 0;
    while (*s && n < w) { lcd_char((uint8_t)*s++); n++; }
    while (n++ < w) lcd_char(' ');
}

/* Anti-flickering: só reescreve linha se mudou */
static char g_lcd_prev[2][17] = {"", ""};
static void lcd_update_line(uint8_t row, const char *line) {
    if (strncmp(g_lcd_prev[row], line, 16) != 0) {
        lcd_goto(0, row);
        lcd_str_w(line, 16);
        strncpy(g_lcd_prev[row], line, 16);
        g_lcd_prev[row][16] = '\0';
    }
}

/* ===========================================================
 *  RELÉS
 * =========================================================== */
static void relays_init(void) {
    const uint pins[] = { PIN_K1, PIN_K2, PIN_K3 };
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]); gpio_set_dir(pins[i], GPIO_OUT); gpio_put(pins[i], 0);
    }
}
static void apply_gain(gain_idx_t idx) {
    const gain_cfg_t *g = &GAIN_TABLE[idx];
    gpio_put(PIN_K1, g->k1);
    gpio_put(PIN_K2, g->k2);
    gpio_put(PIN_K3, g->k3);
    sleep_ms(15);
    g_gain = idx;
}

/* ===========================================================
 *  ADS1115
 * =========================================================== */
static void ads_start(void) {
    uint16_t cfg = ADS_MUX_AIN0 | ADS_PGA | ADS_MODE_CONT
                 | ADS_DR_860SPS | ADS_COMP_OFF;
    uint8_t buf[3] = { ADS_REG_CFG, (uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF) };
    i2c_write_blocking(I2C_PORT, ADS_ADDR, buf, 3, false);
    uint8_t reg = ADS_REG_CONV;
    i2c_write_blocking(I2C_PORT, ADS_ADDR, &reg, 1, false);
    sleep_us(ADS_WAIT_US * 2);
}

static float ads_sample_mV(void) {
    sleep_us(ADS_WAIT_US);
    uint8_t data[2];
    i2c_read_blocking(I2C_PORT, ADS_ADDR, data, 2, false);
    int16_t raw = (int16_t)((data[0] << 8) | data[1]);
    return (float)raw * ADS_LSB_uV / 1000.0f;
}

/* Acumula amostras por `ms` milissegundos e retorna a média */
static float ads_average_mV(uint32_t ms) {
    absolute_time_t t0 = get_absolute_time();
    double acc = 0.0;
    uint32_t n = 0;
    while (absolute_time_diff_us(t0, get_absolute_time()) < (ms * 1000)) {
        acc += ads_sample_mV();
        n++;
    }
    return n > 0 ? (float)(acc / n) : 0.0f;
}

/* ===========================================================
 *  AUTO-GANHO COM HISTERESE CORRETA
 *
 *  Lógica:
 *  - HYST_FREE: avalia se deve trocar
 *      > 90% FS → desce ganho → entra em HYST_WAIT_DOWN
 *      <  8% FS → sobe ganho  → entra em HYST_WAIT_UP
 *  - HYST_WAIT_DOWN: esperando sair da zona alta
 *      só volta a HYST_FREE quando v < (90% - 5%) = 85%
 *  - HYST_WAIT_UP: esperando sair da zona baixa
 *      só volta a HYST_FREE quando v > ( 8% + 5%) = 13%
 * =========================================================== */
static void auto_gain(float v_abs_mV) {

    switch (g_hyst) {

        case HYST_FREE:
            if (v_abs_mV > GAIN_HIGH_mV && g_gain > G0) {
                apply_gain((gain_idx_t)(g_gain - 1));
                g_hyst = HYST_WAIT_DOWN;
            } else if (v_abs_mV < GAIN_LOW_mV && g_gain < G3) {
                apply_gain((gain_idx_t)(g_gain + 1));
                g_hyst = HYST_WAIT_UP;
            }
            break;

        case HYST_WAIT_DOWN:
            /* desceu ganho: espera sair da zona alta */
            if (v_abs_mV < (GAIN_HIGH_mV - GAIN_HYST_mV))
                g_hyst = HYST_FREE;
            break;

        case HYST_WAIT_UP:
            /* subiu ganho: espera sair da zona baixa */
            if (v_abs_mV > (GAIN_LOW_mV + GAIN_HYST_mV))
                g_hyst = HYST_FREE;
            break;
    }
}

/* ===========================================================
 *  DISPLAY
 *
 *  Linha 1: tensão bruta no ADS  +  ganho ativo
 *  Linha 2: tensão corrigida (offset removido, dividida pelo ganho)
 * =========================================================== */
static void display_update(float v_raw_mV, float v_corr_mV, gain_idx_t idx) {
    char l1[17], l2[17];
    /* Mostra ganho como 1/2/3/4 em vez do valor numérico */
    snprintf(l1, sizeof(l1), "R%+8.2fmV G%d",
             v_raw_mV, (int)idx + 1);
    snprintf(l2, sizeof(l2), "C%+8.2fmV",
             v_corr_mV);
    lcd_update_line(0, l1);
    lcd_update_line(1, l2);
}

/* ===========================================================
 *  MAIN
 * =========================================================== */
int main(void) {
    stdio_init_all();

    /* I2C */
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    /* Relés — ganho fixo conforme GANHO_MANUAL */
    relays_init();
    apply_gain((gain_idx_t)(GANHO_MANUAL - 1));

    /* LCD */
    lcd_init();
    lcd_goto(0, 0); lcd_str_w("  Hall Sensor  ", 16);
    lcd_goto(0, 1); lcd_str_w("  Iniciando... ", 16);
    sleep_ms(1500);
    lcd_cmd(0x01); sleep_ms(2);

    /* Inicia ADS em modo contínuo */
    ads_start();
    int idx = 0;
    while (true) {

        /* ── 1. Média de 100ms ─────────────────────────────── */
        float v_raw_mV = ads_average_mV(tempo_aq);

        /* ── 2. Tensão corrigida (offset + ganho INA) ─────── */
        float v_corr_mV = (v_raw_mV - GAIN_TABLE[g_gain].offset_mV)
                          / GAIN_TABLE[g_gain].gain;

        /* ── 3. Display ─────────────────────────────────────── */
        display_update(v_raw_mV, v_corr_mV, g_gain);

        /* ── 4. Debug USB ───────────────────────────────────── */
        printf("Vraw=%+8.3f mV | Vcorr=%+8.3f mV | G%d (%.1f) | Idx: %d\n",
               v_raw_mV, v_corr_mV, (int)g_gain + 1, GAIN_TABLE[g_gain].gain, idx);
        
        idx++;
    }

    return 0;
}