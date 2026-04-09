#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_netif.h"

static const char *TAG = "demo_app";

#define DEMO_UART_PORT      UART_NUM_1
#define DEMO_UART_TX_PIN    GPIO_NUM_5
#define DEMO_UART_RX_PIN    GPIO_NUM_4
#define DEMO_UART_BAUD      115200
#define DEMO_UART_BUF_SIZE  256

#define TCP_SERVER_IP       "192.168.0.107"
#define TCP_SERVER_PORT     23

static bool has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }
    return ip_info.ip.addr != 0;
}

static int tcp_connect(void)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(TCP_SERVER_PORT),
    };
    inet_pton(AF_INET, TCP_SERVER_IP, &dest.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect() failed: %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Connected to %s:%d", TCP_SERVER_IP, TCP_SERVER_PORT);
    return sock;
}

static void uart_rx_task(void *arg)
{
    uint8_t *buf = malloc(DEMO_UART_BUF_SIZE);
    int sock = -1;
    uint32_t ping_counter = 0;

    while (1) {
        int len = uart_read_bytes(DEMO_UART_PORT, buf, DEMO_UART_BUF_SIZE,
                                  pdMS_TO_TICKS(1000));
        if (len <= 0) {
            /* No UART data — send a keepalive to test TCP */
            len = snprintf((char *)buf, DEMO_UART_BUF_SIZE, "PING %"PRIu32"\r\n", ping_counter++);

        } else {
            ESP_LOGI(TAG, "UART RX (%d bytes):", len);
            ESP_LOG_BUFFER_HEX(TAG, buf, len);
        }

        if (sock < 0) {
            if (!has_ip()) {
                ESP_LOGW(TAG, "No IP yet, dropping packet");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            sock = tcp_connect();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        if (sock >= 0) {
            int sent = send(sock, buf, len, 0);
            if (sent < 0) {
                ESP_LOGE(TAG, "send() failed: %d, reconnecting", errno);
                close(sock);
                sock = -1;
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

void demo_app_start(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = DEMO_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(DEMO_UART_PORT, DEMO_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(DEMO_UART_PORT, &uart_cfg);
    uart_set_pin(DEMO_UART_PORT, DEMO_UART_TX_PIN, DEMO_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 5, NULL);
}
