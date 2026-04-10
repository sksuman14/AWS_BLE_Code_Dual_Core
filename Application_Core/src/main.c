/* ========================================================
 * nRF5340 — APPLICATION CORE (cpuapp)
 * NCS v2.5.1
 *
 * Responsibilities:
 *   - Release network core from reset
 *   - Read SHT45 temperature & humidity sensor
 *   - Keep latest reading in memory (no storage/logging)
 *   - On TRIGGER from NET core → send latest SHT45 data via IPC
 *   - On RESET  from NET core → clear latest data
 * ======================================================== */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <hal/nrf_gpio.h>

#include "ipc_msg.h"

/* ============================
 * Weather Station Error Codes
 * ============================ */
typedef enum {
    WS_ERR_NONE = 0,                               /* Default: No System Error */
    WS_ERR_NO_ERROR = 1,                           /* 1: NO error */
    WS_ERR_DUPLICATE_RECORD = 7,
    WS_ERR_STATION_ID_MISMATCH = 8,
    WS_ERR_REQUIRED_FIELD_MISSING = 9,
    WS_ERR_ALL_REQUIRED_VALUE_MISSING = 10,
    WS_ERR_MAX_TEMP_NOT_FLOAT = 11,
    WS_ERR_MAX_TEMP_OUT_OF_RANGE = 12,
    WS_ERR_MIN_TEMP_NOT_FLOAT = 21,
    WS_ERR_MIN_TEMP_OUT_OF_RANGE = 22,
    WS_ERR_NOW_TEMP_NOT_FLOAT = 31,
    WS_ERR_NOW_TEMP_OUT_OF_RANGE = 32,
    WS_ERR_MAX_RH_NOT_FLOAT = 41,
    WS_ERR_MAX_RH_OUT_OF_RANGE = 42,
    WS_ERR_MIN_RH_NOT_FLOAT = 51,
    WS_ERR_MIN_RH_OUT_OF_RANGE = 52,
    WS_ERR_NOW_RH_NOT_FLOAT = 61,
    WS_ERR_NOW_RH_OUT_OF_RANGE = 62,
    WS_ERR_RAIN_NOT_FLOAT = 71,
    WS_ERR_RAIN_OUT_OF_RANGE = 72,
    WS_ERR_WIND_SPEED_NOT_FLOAT = 81,
    WS_ERR_WIND_SPEED_OUT_OF_RANGE = 82,
    WS_ERR_WIND_DIR_NOT_FLOAT = 91,
    WS_ERR_WIND_DIR_OUT_OF_RANGE = 92
} ws_error_code_t;

/* ============================
 * Plausible Value Ranges
 *
 * Temperature : -40.00 to +85.00 °C   (SHT45 hardware range)
 * Humidity    :   0.00 to 100.00 %RH
 * Wind Speed  :   0.00 to 100.00 m/s  (above hurricane force)
 * Wind Dir    :   0    to 360   degrees
 * Rainfall    :   0.00 to 500.00 mm   (extreme flood level)
 *
 * All raw values are stored * 100 as int16_t
 * so ranges below are also * 100
 * ============================ */
#define TEMP_MIN_RAW      (-4000)   /* -40.00 C  */
#define TEMP_MAX_RAW       (8500)   /* +85.00 C  */

#define HUM_MIN_RAW           (0)   /*   0.00 %  */
#define HUM_MAX_RAW       (10000)   /* 100.00 %  */

#define WIND_SPEED_MIN_RAW    (0)   /*   0.00 m/s */
#define WIND_SPEED_MAX_RAW (10000)  /* 100.00 m/s */

#define WIND_DIR_MIN          (0)   /*   0 deg   */
#define WIND_DIR_MAX        (360)   /* 360 deg   */

#define RAIN_MIN_RAW          (0)   /*   0.00 mm */
#define RAIN_MAX_RAW      (50000)   /* 500.00 mm */

/* ============================
 * sensor variables
 * ============================ */
static int16_t wind_speed_raw = 0;
static int16_t wind_dir_raw   = 0;
static int16_t rain_raw       = 0;
static int16_t temp_raw       = 0;
static int16_t hum_raw        = 0;

/* ============================
 * Hardware Devices: Rain & UART
 * ============================ */
#if DT_NODE_EXISTS(DT_NODELABEL(uart3))
#define UART_DEVICE_NODE DT_NODELABEL(uart3)
#else
#error "UART3 node not defined in devicetree"
#endif

const struct device *uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#define RAIN_NODE DT_ALIAS(sw0)
#else
#error "Rain gauge (sw0) not defined"
#endif

static const struct gpio_dt_spec rain_gauge = GPIO_DT_SPEC_GET(RAIN_NODE, gpios);

static struct gpio_callback rain_cb_data;
static volatile uint32_t rain_tip_count = 0;
static int64_t last_interrupt_time = 0;

#define MSG_SIZE 64
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);

static char rx_buf[MSG_SIZE];
static int rx_buf_pos;

/* Global error multiplex tracking */
static bool is_error_active[256] = { false };

static void ws_set_error(ws_error_code_t err) {
    if (err != WS_ERR_NONE) {
        is_error_active[err] = true;
    }
}

static void ws_clear_error(ws_error_code_t err)
{
    is_error_active[err] = false;
}

static void ws_clear_all_errors(void) {
    memset(is_error_active, 0, sizeof(is_error_active));
}

/* ============================
 * SHT45 sensor device
 * ============================ */
static const struct device *const sht =
    DEVICE_DT_GET_ANY(sensirion_sht4x);

/* ============================
 * Latest sensor reading / Mfg Data Array
 * ============================ */
static uint8_t latest_mfg_data[WS_MFG_LEN] = {0};
static volatile bool data_ready = false;

/* ============================
 * IPC
 * ============================ */
static struct ipc_ept app_ept;
static K_SEM_DEFINE(ipc_bound_sem, 0, 1);

/* ============================
 * Work items
 * ============================ */
static struct k_work read_sensor_work;  /* read SHT45 once              */
static struct k_work send_data_work;    /* send latest data to NET core  */
static struct k_work reset_work;        /* clear latest data             */

/* ============================
 * Forward declarations
 * ============================ */
static void read_sensor_handler(struct k_work *w);
static void send_data_handler(struct k_work *w);
static void do_reset_handler(struct k_work *w);
static void ipc_bound_cb(void *priv);
static void ipc_recv_cb(const void *data, size_t len, void *priv);

/* ============================
 * IPC endpoint config
 * ============================ */
static const struct ipc_ept_cfg app_ept_cfg = {
    .name = "app_ept",
    .prio = 0,
    .cb   = {
        .bound    = ipc_bound_cb,
        .received = ipc_recv_cb,
    },
};

static void ipc_bound_cb(void *priv)
{
    ARG_UNUSED(priv);
    printk("[APP] IPC endpoint bound\n");
    k_sem_give(&ipc_bound_sem);
}

static void ipc_recv_cb(const void *data, size_t len, void *priv)
{
    ARG_UNUSED(priv);
    if (len < 1) return;

    const uint8_t *msg = (const uint8_t *)data;

    switch (msg[0]) {
    case IPC_MSG_TYPE_TRIGGER:
        printk("[APP] Trigger received — reading SHT45 and sending\n");
        /* First read fresh sensor data, then send */
        k_work_submit(&read_sensor_work);
        break;

    case IPC_MSG_TYPE_RESET:
        printk("[APP] Reset received — clearing sensor data\n");
        k_work_submit(&reset_work);
        break;

    case IPC_MSG_TYPE_ACK:
        printk("[APP] Network core confirmed BLE advertising done\n");
        break;

    default:
        printk("[APP] Unknown IPC msg: 0x%02X\n", msg[0]);
        break;
    }
}

/* ============================
 * Hardware: Rain Interrupt
 * ============================ */
void rain_interrupt_handler(const struct device *dev,
                            struct gpio_callback *cb,
                            uint32_t pins)
{
    int64_t now = k_uptime_get();
    if (now - last_interrupt_time > 200) /* 200 ms debounce */
    {
        rain_tip_count++;
        last_interrupt_time = now;
    }
}

void init_rain_sensor(void)
{
    if (!device_is_ready(rain_gauge.port)) {
        printk("[APP] Rain GPIO not ready\n");
        return;
    }
    gpio_pin_configure_dt(&rain_gauge, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&rain_gauge, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&rain_cb_data, rain_interrupt_handler, BIT(rain_gauge.pin));
    gpio_add_callback(rain_gauge.port, &rain_cb_data);
    printk("[APP] Rain sensor initialized\n");
}

/* ============================
 * Hardware: Anemometer UART Thread
 * ============================ */
void clean_line(char *line)
{
    char clean[MSG_SIZE];
    int j = 0;
    for (int i = 0; i < strlen(line); i++) {
        if (line[i] >= 32 && line[i] <= 126) { /* printable ASCII only */
            clean[j++] = line[i];
        }
    }
    clean[j] = '\0';
    strcpy(line, clean);
}

static void try_parse_line(const char *line)
{
    if (strlen(line) == 0 || line[0] != 'Q') {
        return; /* ignore boot/config messages */
    }

    int speed_int = 0;
    int speed_dec = 0;
    int wind_dir = 0;

    /* Completely bypass %f floating-point libc requirements using integer parts */
    if (sscanf(line, "Q,%d,%d.%d", &wind_dir, &speed_int, &speed_dec) == 3) {
        wind_speed_raw = (int16_t)(speed_int * 100 + speed_dec);
        wind_dir_raw = wind_dir;
        return;
    }
    /* We silently ignore lines that don't parse to prevent console spam */
}

static void serial_cb(const struct device *dev, void *user_data)
{
    uint8_t c;
    if (!uart_irq_update(uart_dev) || !uart_irq_rx_ready(uart_dev)) return;

    while (uart_fifo_read(uart_dev, &c, 1) == 1) {
        if ((c == '\n' || c == '\r') && rx_buf_pos > 0) {
            rx_buf[rx_buf_pos] = '\0';
            k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);
            rx_buf_pos = 0;
        } else if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
            rx_buf[rx_buf_pos++] = (char)c;
        }
    }
}

void US_Anemometer(void *p1, void *p2, void *p3)
{
    char line[MSG_SIZE];
    printk("[APP] Anemometer thread started\n");

    if (!device_is_ready(uart_dev)) {
        printk("[APP] UART not ready\n");
        return;
    }

    int ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
    if (ret < 0) {
        printk("[APP] UART callback error: %d\n", ret);
        return;
    }

    uart_irq_rx_enable(uart_dev);

    while (1) {
        if (k_msgq_get(&uart_msgq, &line, K_FOREVER) == 0) {
            clean_line(line);
            if (strlen(line) > 0 && line[0] == 'Q') {
                try_parse_line(line);
            }
        }
    }
}

K_THREAD_DEFINE(anemometer_thread_id, 4096, US_Anemometer, NULL, NULL, NULL, 7, 0, 0);

/* ============================
 * Hardware: Continuous SHT45 Thread
 * ============================ */
void sensor_thread(void *p1, void *p2, void *p3)
{
    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

    while (1) {
        k_msleep(1000);

        if (!device_is_ready(sht)) {
            ws_set_error(WS_ERR_NOW_TEMP_NOT_FLOAT);
            ws_set_error(WS_ERR_NOW_RH_NOT_FLOAT);
            temp_raw = 0;
            hum_raw  = 0;

            /* Industrial Hot-plug recovery:
             * If booted without sensor, Zephyr marks driver dead permanently.
             * We manually ping the bus here. If we get an ACK, we trigger a seamless reboot! */
            if (device_is_ready(i2c_dev)) {
                uint8_t dummy = 0;
                int ret = i2c_write(i2c_dev, &dummy, 0, 0x44);
                if (ret == 0) {
                    printk("[APP] SHT45 Hot-plug detected! Performing safe soft-reboot to mount driver...\n");
                    k_msleep(50);
                    sys_reboot(SYS_REBOOT_WARM);
                }
            }
        } else {
            struct sensor_value t_val, h_val;
            int err = sensor_sample_fetch(sht);
            if (err != 0) {
                ws_set_error(WS_ERR_NOW_TEMP_NOT_FLOAT);
                ws_set_error(WS_ERR_NOW_RH_NOT_FLOAT);
                temp_raw = 0;
                hum_raw  = 0;
            } else {
                sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t_val);
                sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY,     &h_val);

                double temperature = sensor_value_to_double(&t_val);
                double humidity    = sensor_value_to_double(&h_val);

                temp_raw = (int16_t)(temperature * 100);
                hum_raw  = (int16_t)(humidity * 100);

                /* Clear fetch errors since read succeeded */
                ws_clear_error(WS_ERR_NOW_TEMP_NOT_FLOAT);
                ws_clear_error(WS_ERR_NOW_RH_NOT_FLOAT);
            }
        }

        /* Compute Rain from accumulated interrupts */
        float rain_calc = rain_tip_count * 0.5f;
        rain_raw = (int16_t)(rain_calc * 100);

        /* Print real-time feed formatted identically to original architecture */
        printk("[APP] SHT45: T=%d.%02d C, H=%u.%02u %%RH, WindSpeed=%d.%02d m/s, WindDirection=%d°, Rainfall=%d.%02d mm\n",
               temp_raw / 100, abs(temp_raw % 100),
               hum_raw / 100, abs(hum_raw % 100),
               wind_speed_raw / 100, abs(wind_speed_raw % 100),
               wind_dir_raw,
               rain_raw / 100, abs(rain_raw % 100));
    }
}

K_THREAD_DEFINE(sensor_thread_id, 2048, sensor_thread, NULL, NULL, NULL, 7, 0, 0);

/* ============================
 * Error evaluation function
 *
 * Called every time a trigger arrives.
 * Checks each live sensor value against its plausible range and
 * sets / clears the corresponding error flags.
 *
 * "not float" errors  → raw value == 0 (fetch failed / sensor absent)
 * "out of range" errors → value outside defined plausible limits
 *
 * Temperature covers errors 11,12,21,22,31,32
 * Humidity     covers errors 41,42,51,52,61,62
 * Wind speed   covers errors 81,82
 * Wind dir     covers errors 91,92
 * Rainfall     covers errors 71,72
 * ============================ */
static void evaluate_sensor_errors(void)
{
    /* ------------------------------------------------
     * TEMPERATURE  (temp_raw = degrees * 100)
     * Plausible range: -40.00 to +85.00 °C
     * ------------------------------------------------ */

    /* --- Max temperature (error 11, 12) --- */
    if (temp_raw == 0) {
        ws_set_error(WS_ERR_MAX_TEMP_NOT_FLOAT);          /* err 11 */
        ws_clear_error(WS_ERR_MAX_TEMP_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_MAX_TEMP_NOT_FLOAT);
        if (temp_raw < TEMP_MIN_RAW || temp_raw > TEMP_MAX_RAW) {
            ws_set_error(WS_ERR_MAX_TEMP_OUT_OF_RANGE);   /* err 12 */
        } else {
            ws_clear_error(WS_ERR_MAX_TEMP_OUT_OF_RANGE);
        }
    }

    /* --- Min temperature (error 21, 22) --- */
    if (temp_raw == 0) {
        ws_set_error(WS_ERR_MIN_TEMP_NOT_FLOAT);          /* err 21 */
        ws_clear_error(WS_ERR_MIN_TEMP_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_MIN_TEMP_NOT_FLOAT);
        if (temp_raw < TEMP_MIN_RAW || temp_raw > TEMP_MAX_RAW) {
            ws_set_error(WS_ERR_MIN_TEMP_OUT_OF_RANGE);   /* err 22 */
        } else {
            ws_clear_error(WS_ERR_MIN_TEMP_OUT_OF_RANGE);
        }
    }

    /* --- Now temperature (error 31, 32) --- */
    if (temp_raw == 0) {
        ws_set_error(WS_ERR_NOW_TEMP_NOT_FLOAT);          /* err 31 */
        ws_clear_error(WS_ERR_NOW_TEMP_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_NOW_TEMP_NOT_FLOAT);
        if (temp_raw < TEMP_MIN_RAW || temp_raw > TEMP_MAX_RAW) {
            ws_set_error(WS_ERR_NOW_TEMP_OUT_OF_RANGE);   /* err 32 */
        } else {
            ws_clear_error(WS_ERR_NOW_TEMP_OUT_OF_RANGE);
        }
    }

    /* ------------------------------------------------
     * HUMIDITY  (hum_raw = percent * 100)
     * Plausible range: 0.00 to 100.00 %RH
     * ------------------------------------------------ */

    /* --- Max RH (error 41, 42) --- */
    if (hum_raw == 0) {
        ws_set_error(WS_ERR_MAX_RH_NOT_FLOAT);            /* err 41 */
        ws_clear_error(WS_ERR_MAX_RH_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_MAX_RH_NOT_FLOAT);
        if (hum_raw < HUM_MIN_RAW || hum_raw > HUM_MAX_RAW) {
            ws_set_error(WS_ERR_MAX_RH_OUT_OF_RANGE);     /* err 42 */
        } else {
            ws_clear_error(WS_ERR_MAX_RH_OUT_OF_RANGE);
        }
    }

    /* --- Min RH (error 51, 52) --- */
    if (hum_raw == 0) {
        ws_set_error(WS_ERR_MIN_RH_NOT_FLOAT);            /* err 51 */
        ws_clear_error(WS_ERR_MIN_RH_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_MIN_RH_NOT_FLOAT);
        if (hum_raw < HUM_MIN_RAW || hum_raw > HUM_MAX_RAW) {
            ws_set_error(WS_ERR_MIN_RH_OUT_OF_RANGE);     /* err 52 */
        } else {
            ws_clear_error(WS_ERR_MIN_RH_OUT_OF_RANGE);
        }
    }

    /* --- Now RH (error 61, 62) --- */
    if (hum_raw == 0) {
        ws_set_error(WS_ERR_NOW_RH_NOT_FLOAT);            /* err 61 */
        ws_clear_error(WS_ERR_NOW_RH_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_NOW_RH_NOT_FLOAT);
        if (hum_raw < HUM_MIN_RAW || hum_raw > HUM_MAX_RAW) {
            ws_set_error(WS_ERR_NOW_RH_OUT_OF_RANGE);     /* err 62 */
        } else {
            ws_clear_error(WS_ERR_NOW_RH_OUT_OF_RANGE);
        }
    }

    /* ------------------------------------------------
     * WIND SPEED  (wind_speed_raw = m/s * 100)
     * Plausible range: 0.00 to 100.00 m/s
     * ------------------------------------------------ */

    if (wind_speed_raw == 0 && wind_dir_raw == 0) {
        ws_set_error(WS_ERR_WIND_SPEED_NOT_FLOAT);        /* err 81 */
        ws_clear_error(WS_ERR_WIND_SPEED_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_WIND_SPEED_NOT_FLOAT);
        if (wind_speed_raw < WIND_SPEED_MIN_RAW ||
            wind_speed_raw > WIND_SPEED_MAX_RAW) {
            ws_set_error(WS_ERR_WIND_SPEED_OUT_OF_RANGE); /* err 82 */
        } else {
            ws_clear_error(WS_ERR_WIND_SPEED_OUT_OF_RANGE);
        }
    }

    /* ------------------------------------------------
     * WIND DIRECTION  (wind_dir_raw = degrees 0-360)
     * Plausible range: 0 to 360 degrees
     * ------------------------------------------------ */

    if (wind_speed_raw == 0 && wind_dir_raw == 0) {
        ws_set_error(WS_ERR_WIND_DIR_NOT_FLOAT);          /* err 91 */
        ws_clear_error(WS_ERR_WIND_DIR_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_WIND_DIR_NOT_FLOAT);
        if (wind_dir_raw < WIND_DIR_MIN ||
            wind_dir_raw > WIND_DIR_MAX) {
            ws_set_error(WS_ERR_WIND_DIR_OUT_OF_RANGE);   /* err 92 */
        } else {
            ws_clear_error(WS_ERR_WIND_DIR_OUT_OF_RANGE);
        }
    }

    /* ------------------------------------------------
     * RAINFALL  (rain_raw = mm * 100)
     * Plausible range: 0.00 to 500.00 mm
     * ------------------------------------------------ */

    if (rain_raw < 0) {
        ws_set_error(WS_ERR_RAIN_NOT_FLOAT);              /* err 71 */
        ws_clear_error(WS_ERR_RAIN_OUT_OF_RANGE);
    } else {
        ws_clear_error(WS_ERR_RAIN_NOT_FLOAT);
        if (rain_raw < RAIN_MIN_RAW || rain_raw > RAIN_MAX_RAW) {
            ws_set_error(WS_ERR_RAIN_OUT_OF_RANGE);       /* err 72 */
        } else {
            ws_clear_error(WS_ERR_RAIN_OUT_OF_RANGE);
        }
    }

    /* Print active errors for debug */
    printk("[APP] Error check done. Active errors: ");
    bool any = false;
    for (int i = 1; i < 256; i++) {
        if (is_error_active[i]) {
            printk("%d ", i);
            any = true;
        }
    }
    if (!any) printk("none");
    printk("\n");
}

/* ============================
 * Work: pack combo payload INSTANTLY on trigger
 * ============================ */
static void read_sensor_handler(struct k_work *w)
{
    ARG_UNUSED(w);

    /* Evaluate all error conditions using live sensor values */
    evaluate_sensor_errors();

    /* --- Pack into COMBO payload --- */
    memset(latest_mfg_data, 0, sizeof(latest_mfg_data));
    
    /* 1. Header (3 bytes) */
    latest_mfg_data[0] = 0x59;
    latest_mfg_data[1] = 0x00;
    latest_mfg_data[2] = 102;                    
    
    /* 2. Sensor Data (10 bytes) */
    /* Temp */
    latest_mfg_data[3] = (int8_t)(temp_raw / 100);
    latest_mfg_data[4] = (int8_t)(abs(temp_raw % 100));
    
    /* Hum */
    latest_mfg_data[5] = (int8_t)(hum_raw / 100);
    latest_mfg_data[6] = (int8_t)(abs(hum_raw % 100));
    
    /* Wind Speed */
    latest_mfg_data[7] = (int8_t)(wind_speed_raw / 100);
    latest_mfg_data[8] = (int8_t)(abs(wind_speed_raw % 100));
    
    /* Wind Dir (16-bit integer 0-360, Little Endian) */
    uint16_t wind_d = (uint16_t)wind_dir_raw;
    latest_mfg_data[9]  = (uint8_t)(wind_d & 0xFF);        /* LSB */
    latest_mfg_data[10] = (uint8_t)((wind_d >> 8) & 0xFF); /* MSB */
    
    /* Rain */
    latest_mfg_data[11] = (int8_t)(rain_raw / 100);
    latest_mfg_data[12] = (int8_t)(abs(rain_raw % 100));

        /* ---- [13..34] Error flags ----
     * Each index = one dedicated error slot.
     * 0x00 = no error  |  error_code value = error active
     * ---- */

     /* Duplicate / station / required field errors
     * (set externally by application logic if needed) */
    if (is_error_active[WS_ERR_DUPLICATE_RECORD]) {
        latest_mfg_data[13] = WS_ERR_DUPLICATE_RECORD;           /* 7  */
    }
    if (is_error_active[WS_ERR_STATION_ID_MISMATCH]) {
        latest_mfg_data[14] = WS_ERR_STATION_ID_MISMATCH;        /* 8  */
    }
    if (is_error_active[WS_ERR_REQUIRED_FIELD_MISSING]) {
        latest_mfg_data[15] = WS_ERR_REQUIRED_FIELD_MISSING;     /* 9  */
    }
    if (is_error_active[WS_ERR_ALL_REQUIRED_VALUE_MISSING]) {
        latest_mfg_data[16] = WS_ERR_ALL_REQUIRED_VALUE_MISSING; /* 10 */
    }

    /* Temperature errors */
    if (is_error_active[WS_ERR_MAX_TEMP_NOT_FLOAT]) {
        latest_mfg_data[17] = WS_ERR_MAX_TEMP_NOT_FLOAT;         /* 11 */
    }
    if (is_error_active[WS_ERR_MAX_TEMP_OUT_OF_RANGE]) {
        latest_mfg_data[18] = WS_ERR_MAX_TEMP_OUT_OF_RANGE;      /* 12 */
    }
    if (is_error_active[WS_ERR_MIN_TEMP_NOT_FLOAT]) {
        latest_mfg_data[19] = WS_ERR_MIN_TEMP_NOT_FLOAT;         /* 21 */
    }
    if (is_error_active[WS_ERR_MIN_TEMP_OUT_OF_RANGE]) {
        latest_mfg_data[20] = WS_ERR_MIN_TEMP_OUT_OF_RANGE;      /* 22 */
    }
    if (is_error_active[WS_ERR_NOW_TEMP_NOT_FLOAT]) {
        latest_mfg_data[21] = WS_ERR_NOW_TEMP_NOT_FLOAT;         /* 31 */
    }
    if (is_error_active[WS_ERR_NOW_TEMP_OUT_OF_RANGE]) {
        latest_mfg_data[22] = WS_ERR_NOW_TEMP_OUT_OF_RANGE;      /* 32 */
    }

    /* Humidity errors */
    if (is_error_active[WS_ERR_MAX_RH_NOT_FLOAT]) {
        latest_mfg_data[23] = WS_ERR_MAX_RH_NOT_FLOAT;           /* 41 */
    }
    if (is_error_active[WS_ERR_MAX_RH_OUT_OF_RANGE]) {
        latest_mfg_data[24] = WS_ERR_MAX_RH_OUT_OF_RANGE;        /* 42 */
    }
    if (is_error_active[WS_ERR_MIN_RH_NOT_FLOAT]) {
        latest_mfg_data[25] = WS_ERR_MIN_RH_NOT_FLOAT;           /* 51 */
    }
    if (is_error_active[WS_ERR_MIN_RH_OUT_OF_RANGE]) {
        latest_mfg_data[26] = WS_ERR_MIN_RH_OUT_OF_RANGE;        /* 52 */
    }
    if (is_error_active[WS_ERR_NOW_RH_NOT_FLOAT]) {
        latest_mfg_data[27] = WS_ERR_NOW_RH_NOT_FLOAT;           /* 61 */
    }
    if (is_error_active[WS_ERR_NOW_RH_OUT_OF_RANGE]) {
        latest_mfg_data[28] = WS_ERR_NOW_RH_OUT_OF_RANGE;        /* 62 */
    }

    /* Rainfall errors */
    if (is_error_active[WS_ERR_RAIN_NOT_FLOAT]) {
        latest_mfg_data[29] = WS_ERR_RAIN_NOT_FLOAT;             /* 71 */
    }
    if (is_error_active[WS_ERR_RAIN_OUT_OF_RANGE]) {
        latest_mfg_data[30] = WS_ERR_RAIN_OUT_OF_RANGE;          /* 72 */
    }

    /* Wind speed errors */
    if (is_error_active[WS_ERR_WIND_SPEED_NOT_FLOAT]) {
        latest_mfg_data[31] = WS_ERR_WIND_SPEED_NOT_FLOAT;       /* 81 */
    }
    if (is_error_active[WS_ERR_WIND_SPEED_OUT_OF_RANGE]) {
        latest_mfg_data[32] = WS_ERR_WIND_SPEED_OUT_OF_RANGE;    /* 82 */
    }

    /* Wind direction errors */
    if (is_error_active[WS_ERR_WIND_DIR_NOT_FLOAT]) {
        latest_mfg_data[33] = WS_ERR_WIND_DIR_NOT_FLOAT;         /* 91 */
    }
    if (is_error_active[WS_ERR_WIND_DIR_OUT_OF_RANGE]) {
        latest_mfg_data[34] = WS_ERR_WIND_DIR_OUT_OF_RANGE;      /* 92 */
    }

    // /* For testing Errors*/
    // latest_mfg_data[13] = 7;
    // latest_mfg_data[14] = 8;
    // latest_mfg_data[15] = 9;
    // latest_mfg_data[16] = 10;
    // latest_mfg_data[17] = 11;
    // latest_mfg_data[18] = 12;
    // latest_mfg_data[19] = 21;
    // latest_mfg_data[20] = 22;
    // latest_mfg_data[21] = 31;
    // latest_mfg_data[22] = 32;
    // latest_mfg_data[23] = 41;
    // latest_mfg_data[24] = 42;
    // latest_mfg_data[25] = 51;
    // latest_mfg_data[26] = 52;
    // latest_mfg_data[27] = 61;
    // latest_mfg_data[28] = 62;
    // latest_mfg_data[29] = 71;
    // latest_mfg_data[30] = 72;
    // latest_mfg_data[31] = 81;
    // latest_mfg_data[32] = 82;
    // latest_mfg_data[33] = 91;
    // latest_mfg_data[34] = 92;

    data_ready = true;
 
    /* Print full payload for debug */
    printk("[APP] MFG Payload (%d bytes): ", WS_MFG_LEN);
    for (int i = 0; i < WS_MFG_LEN; i++) {
        printk("%02X ", latest_mfg_data[i]);
    }
    printk("\n");
    
    /* Now send data to NET core */
    k_work_submit(&send_data_work);
}

/* ============================
 * Work: send latest SHT45 data to NET core via IPC
 * ============================ */
static void send_data_handler(struct k_work *w)
{
    ARG_UNUSED(w);

    if (!data_ready) {
        printk("[APP] No sensor data available yet\n");
        /* Tell NET core nothing to send */
        uint8_t nack[1] = { IPC_MSG_TYPE_NACK };
        ipc_service_send(&app_ept, nack, sizeof(nack));
        return;
    }

    /* IPC message: [type=DATA | WS_MFG_LEN bytes mfg_data] */
    uint8_t ipc_msg[1 + WS_MFG_LEN];
    ipc_msg[0] = IPC_MSG_TYPE_DATA;
    memcpy(&ipc_msg[1], latest_mfg_data, WS_MFG_LEN);

    printk("[APP] Sending Payload: ");
    for(int i = 0; i < WS_MFG_LEN; i++) {
        printk("%02X ", latest_mfg_data[i]);
    }
    printk("\n");

    int err = ipc_service_send(&app_ept, ipc_msg, sizeof(ipc_msg));
    if (err < 0) {
        printk("[APP] IPC send failed (err %d)\n", err);
    } else {
        printk("[APP] SHT45 data sent to NET core\n");
    }
}

/* ============================
 * Work: reset / clear sensor data
 * ============================ */
static void do_reset_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    memset(latest_mfg_data, 0x00, sizeof(latest_mfg_data));
    data_ready = false;
    ws_clear_all_errors(); /* Clear error state on reset */
    printk("[APP] Sensor data and error state cleared\n");
}

/* ============================
 * main()
 * ============================ */
int main(void)
{
    printk("=== nRF5340 APP CORE — SHT45 + UART Anemometer + Rain ===\n");

    /* Initialize hardware */
    init_rain_sensor();

    /* Verify SHT40 is ready */
    if (!device_is_ready(sht)) {
        printk("[APP] WARNING: SHT45 missing at boot. Armed for hot-plug recovery.\n");
    } else {
        printk("[APP] SHT45 ready\n");
    }

    /* IPC setup */
    const struct device *ipc_dev =
        DEVICE_DT_GET(DT_NODELABEL(ipc0));
    if (!device_is_ready(ipc_dev)) {
        printk("[APP] FATAL: IPC device not ready\n");
        return -1;
    }

    int err = ipc_service_open_instance(ipc_dev);
    if (err < 0 && err != -EALREADY) {
        printk("[APP] ipc_service_open_instance failed (%d)\n", err);
        return err;
    }

    err = ipc_service_register_endpoint(ipc_dev, &app_ept, &app_ept_cfg);
    if (err < 0) {
        printk("[APP] ipc_service_register_endpoint failed (%d)\n", err);
        return err;
    }

    printk("[APP] Waiting for NET core IPC bind...\n");
    k_sem_take(&ipc_bound_sem, K_FOREVER);
    printk("[APP] IPC ready\n");

    /* Work items */
    k_work_init(&read_sensor_work, read_sensor_handler);
    k_work_init(&send_data_work,   send_data_handler);
    k_work_init(&reset_work,       do_reset_handler);

    printk("[APP] Waiting for trigger from NET core...\n");

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}