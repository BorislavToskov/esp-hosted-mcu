/* Wi-Fi iperf Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_console.h"
#include "cmd_system.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DATA_UART_PORT      UART_NUM_1
#define DATA_UART_TX_PIN    10
#define DATA_UART_RX_PIN    11
#define DATA_UART_BAUD      115200
#define DATA_SEND_PERIOD_MS 1000

static const char *DATA_TAG = "uart_data";

static atomic_bool s_time_synced = ATOMIC_VAR_INIT(false);

static void on_sntp_sync(struct timeval *tv)
{
    atomic_store(&s_time_synced, true);
    ESP_LOGI(DATA_TAG, "SNTP first sync: %lld", (long long)tv->tv_sec);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    ESP_LOGI(DATA_TAG, "SNTP started");
}

static void uart_data_task(void *arg)
{
    char buf[48];
    while (1) {
        int len;
        if (atomic_load(&s_time_synced)) {
            time_t now = time(NULL);
            struct tm tm;
            gmtime_r(&now, &tm);
            char ts[24];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
            len = snprintf(buf, sizeof(buf), "%s\r\n", ts);
        } else {
            len = snprintf(buf, sizeof(buf), "NO_SYNC\r\n");
        }
        uart_write_bytes(DATA_UART_PORT, buf, len);
        ESP_LOGI(DATA_TAG, "TX -> %.*s", len - 2, buf); /* strip trailing \r\n */
        vTaskDelay(pdMS_TO_TICKS(DATA_SEND_PERIOD_MS));
    }
}

static void uart_data_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = DATA_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(DATA_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DATA_UART_PORT, DATA_UART_TX_PIN, DATA_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(DATA_UART_PORT, 256, 0, 0, NULL, 0));
    xTaskCreate(uart_data_task, "uart_data", 2048, NULL, 5, NULL);
}

/* component manager */
#include "iperf.h"
#include "wifi_cmd.h"
#include "iperf_cmd.h"
#include "ping_cmd.h"


#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS || CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
#include "esp_wifi_he.h"
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
extern int wifi_cmd_get_tx_statistics(int argc, char **argv);
extern int wifi_cmd_clr_tx_statistics(int argc, char **argv);
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
extern int wifi_cmd_get_rx_statistics(int argc, char **argv);
extern int wifi_cmd_clr_rx_statistics(int argc, char **argv);
#endif

#if defined(CONFIG_ESP_EXT_CONN_ENABLE) && defined(CONFIG_ESP_HOST_WIFI_ENABLED)
#include "esp_extconn.h"
#endif

void iperf_hook_show_wifi_stats(iperf_traffic_type_t type, iperf_status_t status)
{
    if (status == IPERF_STARTED) {
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
        if (type != IPERF_UDP_SERVER) {
            wifi_cmd_clr_tx_statistics(0, NULL);
        }
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
        if (type != IPERF_UDP_CLIENT) {
            wifi_cmd_clr_rx_statistics(0, NULL);
        }
#endif
    }

    if (status == IPERF_STOPPED) {
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
        if (type != IPERF_UDP_SERVER) {
            wifi_cmd_get_tx_statistics(0, NULL);
        }
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
        if (type != IPERF_UDP_CLIENT) {
            wifi_cmd_get_rx_statistics(0, NULL);
        }
#endif
    }

}


void app_main(void)
{
#if defined(CONFIG_ESP_EXT_CONN_ENABLE) && defined(CONFIG_ESP_HOST_WIFI_ENABLED)
    esp_extconn_config_t ext_config = ESP_EXTCONN_CONFIG_DEFAULT();
    esp_extconn_init(&ext_config);
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    uart_data_init();

    /* initialise wifi */
    app_wifi_initialise_config_t config = APP_WIFI_CONFIG_DEFAULT();
    config.storage = WIFI_STORAGE_RAM;
    config.ps_type = WIFI_PS_NONE;
    app_initialise_wifi(&config);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_got_ip, NULL));
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_MU_STATS
    esp_wifi_enable_rx_statistics(true, true);
#else
    esp_wifi_enable_rx_statistics(true, false);
#endif
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
    esp_wifi_enable_tx_statistics(ESP_WIFI_ACI_BE, true);
#endif


    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";

    // init console REPL environment
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif

    /* Register commands */
    register_system();
    app_register_all_wifi_commands();
    app_register_iperf_commands();
    ping_cmd_register_ping();
    app_register_iperf_hook_func(iperf_hook_show_wifi_stats);


    printf("\n ==================================================\n");
    printf(" |       Steps to test WiFi throughput            |\n");
    printf(" |                                                |\n");
    printf(" |  1. Print 'help' to gain overview of commands  |\n");
    printf(" |  2. Configure device to station or soft-AP     |\n");
    printf(" |  3. Setup WiFi connection                      |\n");
    printf(" |  4. Run iperf to test UDP/TCP RX/TX throughput |\n");
    printf(" |                                                |\n");
    printf(" =================================================\n\n");

    // start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
