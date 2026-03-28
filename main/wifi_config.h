/* WiFi AP + Captive Portal fuer ETH-IP-Konfiguration
 *
 * SSID:  ESP32-ETH-Setup  (offen, kein Passwort)
 * URL:   http://192.168.4.1
 *
 * Gespeichert wird in NVS-Namespace "eth_cfg".
 * Default: DHCP. Statische Werte ueberschreiben DHCP nach Neustart.
 */
#pragma once
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Gespeicherte ETH-IP-Konfiguration */
typedef struct {
    bool use_static;   /**< true = statisch, false = DHCP */
    char ip[16];       /**< IPv4 als String, z.B. "192.168.1.100" */
    char mask[16];     /**< Subnetzmaske, z.B. "255.255.255.0"    */
    char gw[16];       /**< Gateway,       z.B. "192.168.1.1"     */
} eth_ip_cfg_t;

/**
 * @brief Gespeicherte Konfiguration aus NVS laden.
 *        Wenn nichts gespeichert: Defaults (DHCP).
 */
eth_ip_cfg_t eth_ip_cfg_load(void);

/**
 * @brief Konfiguration auf das ETH-Netif anwenden.
 *        Bei DHCP: no-op. Bei statisch: DHCP stoppen, IP setzen.
 *        Muss VOR esp_eth_start() aufgerufen werden.
 */
esp_err_t eth_ip_apply(esp_netif_t *netif, const eth_ip_cfg_t *cfg);

/**
 * @brief WiFi-AP + DNS + HTTP-Captive-Portal starten.
 *        eth_netif wird benoetigt, um die aktuelle ETH-IP im Formular anzuzeigen.
 *        Muss nach esp_event_loop_create_default() und esp_netif_init() aufgerufen werden.
 */
esp_err_t wifi_captive_start(esp_netif_t *eth_netif);

#ifdef __cplusplus
}
#endif
