/* ========================================================
 * nRF5340 — NETWORK CORE (cpunet)
 * NCS v2.5.1
 *
 * Responsibilities:
 *   - Run BLE stack
 *   - Passively scan for trigger / reset packets from mobile
 *   - On trigger → ask APP core for latest sensor data via IPC
 *   - On DATA from APP core → advertise mfg data over BLE
 *   - After advertising → go back to scanning
 *
 * No data logging — live sensor data only
 * ======================================================== */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/ipc/ipc_service.h>
#include <string.h>
#include <errno.h>

#include "ipc_msg.h"

/* ============================
 * Constants — same as original code
 * ============================ */
#define ADVERT_DURATION_MS     200
#define RSSI_THRESHOLD         -90

#define SCAN_INTERVAL_SECONDS  5
#define SCAN_DURATION_MS       200
#define FAST_SCAN_INTERVAL_MS  1000
#define FAST_SCAN_DURATION_MS  2000
#define FAST_SCAN_CYCLES       3

/* Trigger / reset signatures — same as original nRF52832 code */
static const uint8_t trigger_signature[] = {0x59, 0x00, 0xBB, 0xCC};
static const uint8_t reset_signature[]   = {0x59, 0x00, 0xFF, 0xFF};

#define SENSOR_ADDRESS  "DE:AD:BE:AF:BA:58"

/* ============================
 * BLE advertising
 * ============================ */
static struct bt_le_ext_adv *adv;

static struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_NONE | BT_LE_ADV_OPT_EXT_ADV |
    BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_NO_2M,
    0x30, 0x30, NULL);

/* 2 advertising events then auto-stop — same as original code */
static struct bt_le_ext_adv_start_param ext_adv_param =
    BT_LE_EXT_ADV_START_PARAM_INIT(0, 2);

static uint8_t mfg_payload[WS_MFG_LEN] = {0};

/* ============================
 * BLE scanning
 * ============================ */
static struct bt_le_scan_param scan_param = {
    .type     = BT_LE_SCAN_TYPE_PASSIVE,
    .options  = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0030,
    .window   = 0x0030,
};

static volatile bool    is_scanning       = false;
static volatile bool    fast_scan_mode    = false;
static volatile uint8_t fast_scan_counter = 0;
static volatile bool    scan_work_pending = false;
static volatile bool    tx_active         = false;

/* ============================
 * IPC
 * ============================ */
static struct ipc_ept net_ept;
static K_SEM_DEFINE(ipc_bound_sem, 0, 1);

/* ============================
 * Work & Timers
 * ============================ */
static struct k_work_delayable scan_work;
static struct k_work_delayable stop_scan_work;
static struct k_timer          scan_scheduler_timer;
static struct k_work           tx_work;

K_MUTEX_DEFINE(scan_ctrl_mutex);

/* ============================
 * Forward declarations
 * ============================ */
static void adv_init(void);
static void transmit_sensor_data(void);
static void enter_fast_scan_mode(void);
static void restart_scanning(void);
static void scan_work_handler(struct k_work *w);
static void stop_scan_work_handler(struct k_work *w);
static void scan_timer_cb(struct k_timer *t);
static void tx_work_handler(struct k_work *w);
static bool ad_parser_cb(struct bt_data *data, void *user_data);
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t type, struct net_buf_simple *ad_buf);
static void ipc_bound_cb(void *priv);
static void ipc_recv_cb(const void *data, size_t len, void *priv);
static void ipc_send_cmd(uint8_t type);

/* ============================
 * IPC callbacks
 * ============================ */
static const struct ipc_ept_cfg net_ept_cfg = {
    .name = "app_ept",   /* must match app_core exactly */
    .prio = 0,
    .cb   = {
        .bound    = ipc_bound_cb,
        .received = ipc_recv_cb,
    },
};

static void ipc_bound_cb(void *priv)
{
    ARG_UNUSED(priv);
    printk("[NET] IPC endpoint bound\n");
    k_sem_give(&ipc_bound_sem);
}

static void ipc_recv_cb(const void *data, size_t len, void *priv)
{
    ARG_UNUSED(priv);
    if (len < 1) return;

    const uint8_t *msg = (const uint8_t *)data;

    if (msg[0] == IPC_MSG_TYPE_DATA && len >= (1 + WS_MFG_LEN)) {
        /* Copy Mfg data from APP core */
        memcpy(mfg_payload, &msg[1], WS_MFG_LEN);
        printk("[NET] Data received from APP core\n");
        
        printk("[NET] Adv Payload: ");
        for (int i = 0; i < WS_MFG_LEN; i++) {
            printk("%02X ", mfg_payload[i]);
        }
        printk("\n");

        /* Trigger BLE advertising */
        k_work_submit(&tx_work);

    } else if (msg[0] == IPC_MSG_TYPE_NACK) {
        printk("[NET] APP core has no sensor data — skipping adv\n");
        /* Resume scanning */
        restart_scanning();
    }
}

static void ipc_send_cmd(uint8_t type)
{
    uint8_t msg[1] = { type };
    int err = ipc_service_send(&net_ept, msg, sizeof(msg));
    if (err < 0) {
        printk("[NET] IPC send failed (err %d)\n", err);
    }
}

/* ============================
 * BLE advertising
 * ============================ */
static void adv_init(void)
{
    int err = bt_le_ext_adv_create(&adv_param, NULL, &adv);
    if (err) {
        printk("[NET] Failed to create adv set (err %d)\n", err);
    } else {
        printk("[NET] Extended adv set created\n");
    }
}

static void transmit_sensor_data(void)
{
    /* Build BLE data: device name and manufacturer specific data */
    struct bt_data ad[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, (sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_payload, WS_MFG_LEN),
    };

    int err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("[NET] set_data failed (err %d)\n", err);
        return;
    }

    printk("[NET] Starting sensor adv...\n");
    err = bt_le_ext_adv_start(adv, &ext_adv_param);
    if (err) {
        printk("[NET] adv_start failed (err %d)\n", err);
        return;
    }

    /* Wait for 2 advertising events to complete */
    k_msleep(ADVERT_DURATION_MS);
    bt_le_ext_adv_stop(adv);
    printk("[NET] Sensor adv done\n");
}

/* ============================
 * TX work: advertise sensor data then resume scanning
 * ============================ */
static void tx_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);

    /* Stop scanning while advertising */
    k_mutex_lock(&scan_ctrl_mutex, K_FOREVER);
    if (is_scanning) {
        bt_le_scan_stop();
        is_scanning = false;
        k_work_cancel_delayable(&stop_scan_work);
    }
    k_timer_stop(&scan_scheduler_timer);
    k_mutex_unlock(&scan_ctrl_mutex);

    tx_active = true;

    /* Advertise the sensor data */
    transmit_sensor_data();

    /* ACK to APP core */
    ipc_send_cmd(IPC_MSG_TYPE_ACK);

    tx_active = false;

    /* Resume scanning after advertising */
    restart_scanning();
}

/* ============================
 * Restart scan schedule after TX
 * ============================ */
static void restart_scanning(void)
{
    k_mutex_lock(&scan_ctrl_mutex, K_FOREVER);
    if (!fast_scan_mode) {
        k_timer_start(&scan_scheduler_timer,
                      K_SECONDS(SCAN_INTERVAL_SECONDS),
                      K_SECONDS(SCAN_INTERVAL_SECONDS));
        printk("[NET] Resumed normal scan schedule\n");
    } else {
        k_timer_start(&scan_scheduler_timer,
                      K_MSEC(FAST_SCAN_INTERVAL_MS), K_NO_WAIT);
        printk("[NET] Resumed fast scan schedule\n");
    }
    k_mutex_unlock(&scan_ctrl_mutex);
}

/* ============================
 * BLE Scanning — trigger/reset detection
 * ============================ */
struct adv_parse_ctx {
    int8_t rssi;
    bool   trigger_found;
    bool   reset_found;
};

static bool ad_parser_cb(struct bt_data *data, void *user_data)
{
    struct adv_parse_ctx *ctx = (struct adv_parse_ctx *)user_data;

    if (data->type == BT_DATA_MANUFACTURER_DATA) {
        if (data->data_len >= sizeof(trigger_signature) &&
            memcmp(data->data, trigger_signature,
                   sizeof(trigger_signature)) == 0) {
            ctx->trigger_found = true;
            return false;
        }
        if (data->data_len >= sizeof(reset_signature) &&
            memcmp(data->data, reset_signature,
                   sizeof(reset_signature)) == 0) {
            ctx->reset_found = true;
            return false;
        }
    }
    return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t type, struct net_buf_simple *ad_buf)
{
    if (rssi < RSSI_THRESHOLD) return;

    static struct adv_parse_ctx ctx;
    ctx.rssi          = rssi;
    ctx.trigger_found = false;
    ctx.reset_found   = false;

    bt_data_parse(ad_buf, ad_parser_cb, &ctx);

    if (ctx.trigger_found) {
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
        printk("[NET] Trigger from %s RSSI=%d\n", addr_str, rssi);
        enter_fast_scan_mode();
        /* Ask APP core to read sensor and send data */
        ipc_send_cmd(IPC_MSG_TYPE_TRIGGER);

    } else if (ctx.reset_found) {
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
        printk("[NET] Reset from %s RSSI=%d\n", addr_str, rssi);
        ipc_send_cmd(IPC_MSG_TYPE_RESET);
    }
}

/* ============================
 * Scan scheduling
 * ============================ */
static void scan_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    if (!scan_work_pending) {
        scan_work_pending = true;
        k_work_schedule(&scan_work, K_NO_WAIT);
    }
}

static void scan_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    scan_work_pending = false;

    if (tx_active) {
        printk("[NET] Skip scan — TX active\n");
        k_mutex_lock(&scan_ctrl_mutex, K_FOREVER);
        k_timer_start(&scan_scheduler_timer,
                      fast_scan_mode
                          ? K_MSEC(FAST_SCAN_INTERVAL_MS)
                          : K_SECONDS(SCAN_INTERVAL_SECONDS),
                      K_NO_WAIT);
        k_mutex_unlock(&scan_ctrl_mutex);
        return;
    }

    if (is_scanning) {
        printk("[NET] Already scanning\n");
        return;
    }

    int err = bt_le_scan_start(&scan_param, scan_cb);
    if (err) {
        printk("[NET] Scan start failed %d\n", err);
        is_scanning = false;
    } else {
        is_scanning = true;
        printk("[NET] Scanning started\n");
    }

    uint32_t dur = fast_scan_mode
                   ? FAST_SCAN_DURATION_MS
                   : SCAN_DURATION_MS;
    k_work_schedule(&stop_scan_work, K_MSEC(dur));
}

static void stop_scan_work_handler(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!is_scanning) return;

    bt_le_scan_stop();
    is_scanning = false;

    if (fast_scan_mode) {
        fast_scan_counter--;
        printk("[NET] Fast scan %d/%d done\n",
               FAST_SCAN_CYCLES - fast_scan_counter,
               FAST_SCAN_CYCLES);
        if (fast_scan_counter == 0) {
            fast_scan_mode = false;
            printk("[NET] Back to normal scan schedule\n");
            k_timer_start(&scan_scheduler_timer,
                          K_SECONDS(SCAN_INTERVAL_SECONDS),
                          K_SECONDS(SCAN_INTERVAL_SECONDS));
        } else {
            k_timer_start(&scan_scheduler_timer,
                          K_MSEC(FAST_SCAN_INTERVAL_MS), K_NO_WAIT);
        }
    } else {
        printk("[NET] Normal scan done\n");
    }
}

static void enter_fast_scan_mode(void)
{
    if (k_mutex_lock(&scan_ctrl_mutex, K_NO_WAIT) != 0) {
        printk("[NET] Cannot enter fast scan — busy\n");
        return;
    }

    if (fast_scan_mode) {
        fast_scan_counter = FAST_SCAN_CYCLES;
    } else {
        printk("[NET] Entering fast scan mode\n");
        fast_scan_mode    = true;
        fast_scan_counter = FAST_SCAN_CYCLES;

        k_timer_stop(&scan_scheduler_timer);

        if (is_scanning) {
            bt_le_scan_stop();
            is_scanning = false;
            k_work_cancel_delayable(&stop_scan_work);
        }
        if (scan_work_pending) {
            k_work_cancel_delayable(&scan_work);
            scan_work_pending = false;
        }

        k_work_schedule(&scan_work, K_MSEC(100));
    }

    k_mutex_unlock(&scan_ctrl_mutex);
}

/* ============================
 * main()
 * ============================ */
int main(void)
{
    printk("=== nRF5340 NET CORE — BLE BRIDGE ===\n");

    /* Set static random BT address */
    bt_addr_le_t addr;
    if (bt_addr_le_from_str(SENSOR_ADDRESS, "random", &addr) == 0) {
        bt_id_create(&addr, NULL);
    }

    /* Enable Bluetooth */
    int err = bt_enable(NULL);
    if (err) {
        printk("[NET] BT enable failed %d\n", err);
        return -1;
    }
    printk("[NET] BT enabled\n");

    adv_init();
    k_msleep(100);

    /* IPC setup */
    const struct device *ipc_dev =
        DEVICE_DT_GET(DT_NODELABEL(ipc0));
    if (!device_is_ready(ipc_dev)) {
        printk("[NET] FATAL: IPC device not ready\n");
        return -1;
    }

    err = ipc_service_open_instance(ipc_dev);
    if (err < 0 && err != -EALREADY) {
        printk("[NET] ipc_service_open_instance failed (%d)\n", err);
        return err;
    }

    err = ipc_service_register_endpoint(ipc_dev, &net_ept, &net_ept_cfg);
    if (err < 0) {
        printk("[NET] ipc_service_register_endpoint failed (%d)\n", err);
        return err;
    }

    printk("[NET] Waiting for APP core IPC bind...\n");
    k_sem_take(&ipc_bound_sem, K_FOREVER);
    printk("[NET] IPC ready\n");

    /* Work & timer init */
    k_work_init(&tx_work,        tx_work_handler);
    k_work_init_delayable(&scan_work,      scan_work_handler);
    k_work_init_delayable(&stop_scan_work, stop_scan_work_handler);
    k_timer_init(&scan_scheduler_timer, scan_timer_cb, NULL);

    /* Start scanning after 2 s */
    k_timer_start(&scan_scheduler_timer,
                  K_SECONDS(2),
                  K_SECONDS(SCAN_INTERVAL_SECONDS));

    printk("[NET] Scan schedule started — %dms window every %ds\n",
           SCAN_DURATION_MS, SCAN_INTERVAL_SECONDS);
    printk("[NET] Waiting for trigger from mobile...\n");

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}

