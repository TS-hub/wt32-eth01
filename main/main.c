/* WT32-ETH01 iperf-Benchmark-Station
 * Ethernet-Init (W5500 SPI) + iperf2/iperf3-Konsole + WiFi-Captive-Portal
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "ethernet_init.h"
#include "iperf_cmd.h"
#include "cmd_system.h"
#include "iperf3_server.h"
#include "wifi_config.h"

static const char *TAG = "wt32";

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Link Up  MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Link Down");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IP: " IPSTR "  Maske: " IPSTR "  GW: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
}

/* Gibt den eth_netif-Handle zurueck, damit IP-Config angewandt werden kann */
static esp_netif_t *ethernet_init(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handles[0]);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &got_ip_event_handler, NULL));

    /* IP-Konfiguration aus NVS anwenden (statisch oder DHCP) */
    eth_ip_cfg_t ip_cfg = eth_ip_cfg_load();
    eth_ip_apply(eth_netif, &ip_cfg);

    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));
    return eth_netif;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_netif_t *eth_netif = ethernet_init();

    /* WiFi-AP + Captive Portal fuer IP-Konfiguration
     * SSID: ESP32-ETH-Setup  |  URL: http://192.168.4.1  */
    wifi_captive_start(eth_netif);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "wt32>";
    repl_config.max_cmdline_length = 256;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    esp_console_register_help_command();
    register_system_common();
    ESP_ERROR_CHECK(iperf_cmd_register_iperf());
    ESP_ERROR_CHECK(iperf3_register_cmd());
    iperf3_server_start();   /* iperf3-Server direkt beim Boot starten */

    printf("\nWT32-ETH01 iperf-Station bereit.\n"
           "Warte auf DHCP-IP (oder statisch per WLAN-Portal konfigurieren).\n"
           "  iperf  -s -i 3        (iperf2 server, Port 5001)\n"
           "  iperf3 -s             (iperf3 server, Port 5201)\n"
           "  WLAN: ESP32-ETH-Setup -> http://192.168.4.1\n"
           "Tippe 'help' fuer alle Befehle.\n\n");

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
