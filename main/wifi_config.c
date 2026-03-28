/* WiFi AP + Captive Portal fuer ETH-IP-Konfiguration
 *
 * Ablauf:
 *   - AP "ESP32-ETH-Setup" (offen) startet, IP 192.168.4.1
 *   - DNS-Server antwortet auf alle Anfragen mit 192.168.4.1
 *     → Browser leitet automatisch auf Konfigurationsseite
 *   - HTTP-Server serviert Formular: DHCP / Statisch
 *   - Nach "Speichern": NVS-Schreiben + esp_restart()
 *   - Beim naechsten Boot: eth_ip_apply() setzt statische IP vor eth_start
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "wifi_config.h"

#define TAG          "wcfg"
#define AP_SSID      "ESP32-ETH-Setup"
#define AP_CHANNEL   1
#define AP_MAX_CONN  4
#define NVS_NS       "eth_cfg"
#define NVS_KEY_MODE "mode"   /* uint8: 0=DHCP, 1=statisch */
#define NVS_KEY_IP   "ip"
#define NVS_KEY_MASK "mask"
#define NVS_KEY_GW   "gw"

/* Gespeicherter ETH-Netif-Handle fuer IP-Anzeige im Formular */
static esp_netif_t *s_eth_netif = NULL;

/* ------------------------------------------------------------------ */
/* NVS                                                                  */
/* ------------------------------------------------------------------ */

eth_ip_cfg_t eth_ip_cfg_load(void)
{
    eth_ip_cfg_t cfg = {
        .use_static = false,
        .ip   = "192.168.1.100",
        .mask = "255.255.255.0",
        .gw   = "192.168.1.1",
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return cfg;

    uint8_t mode = 0;
    nvs_get_u8(h, NVS_KEY_MODE, &mode);
    cfg.use_static = (mode == 1);

    size_t sz;
    sz = sizeof(cfg.ip);   nvs_get_str(h, NVS_KEY_IP,   cfg.ip,   &sz);
    sz = sizeof(cfg.mask); nvs_get_str(h, NVS_KEY_MASK, cfg.mask, &sz);
    sz = sizeof(cfg.gw);   nvs_get_str(h, NVS_KEY_GW,   cfg.gw,   &sz);

    nvs_close(h);
    return cfg;
}

static esp_err_t cfg_save(const eth_ip_cfg_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h),
                        TAG, "nvs_open failed");
    nvs_set_u8(h, NVS_KEY_MODE, cfg->use_static ? 1 : 0);
    nvs_set_str(h, NVS_KEY_IP,   cfg->ip);
    nvs_set_str(h, NVS_KEY_MASK, cfg->mask);
    nvs_set_str(h, NVS_KEY_GW,   cfg->gw);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/* ETH-IP anwenden                                                      */
/* ------------------------------------------------------------------ */

esp_err_t eth_ip_apply(esp_netif_t *netif, const eth_ip_cfg_t *cfg)
{
    if (!cfg->use_static) {
        ESP_LOGI(TAG, "ETH: DHCP (default)");
        return ESP_OK;
    }

    esp_netif_ip_info_t info = {};
    if (!ip4addr_aton(cfg->ip,   (ip4_addr_t *)&info.ip)  ||
        !ip4addr_aton(cfg->mask, (ip4_addr_t *)&info.netmask) ||
        !ip4addr_aton(cfg->gw,   (ip4_addr_t *)&info.gw)) {
        ESP_LOGE(TAG, "Ungueltige IP-Konfiguration in NVS, nutze DHCP");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &info));
    ESP_LOGI(TAG, "ETH: statisch " IPSTR " / " IPSTR " GW " IPSTR,
             IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Hilfsfunktionen                                                      */
/* ------------------------------------------------------------------ */

/* Einfacher URL-decoded Parameterwert aus application/x-www-form-urlencoded */
static void url_decode(const char *src, char *dst, int maxlen)
{
    int i = 0;
    while (*src && i < maxlen - 1) {
        if (*src == '+') { dst[i++] = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Wert eines Formular-Parameters aus URL-encoded Body extrahieren */
static bool form_get(const char *body, const char *key,
                     char *out, int outlen)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    int len = end ? (int)(end - p) : (int)strlen(p);
    char tmp[128] = {};
    if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    url_decode(tmp, out, outlen);
    return true;
}

/* Aktuelle ETH-IP als String, "---" wenn nicht verbunden */
static void get_eth_ip_str(char *buf, size_t len)
{
    if (!s_eth_netif) { snprintf(buf, len, "---"); return; }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(s_eth_netif, &info) == ESP_OK &&
        info.ip.addr != 0) {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(buf, len, "---");
    }
}

/* ------------------------------------------------------------------ */
/* HTTP-Handler                                                         */
/* ------------------------------------------------------------------ */

static const char HTML_TMPL[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ETH Konfiguration</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:2em auto;padding:0 1em;color:#222}"
    "h2{margin-bottom:.5em}fieldset{border:1px solid #ccc;border-radius:4px;padding:.8em;margin:.8em 0}"
    "legend{font-weight:bold}label{display:block;margin:6px 0}"
    "input[type=text]{width:100%%;padding:5px;box-sizing:border-box;border:1px solid #bbb;border-radius:3px}"
    ".btn{background:#0055aa;color:#fff;border:none;padding:9px 18px;"
    "border-radius:4px;cursor:pointer;width:100%%;margin-top:10px;font-size:1em}"
    ".info{color:#666;font-size:.82em;margin-top:14px;border-top:1px solid #eee;padding-top:8px}"
    "</style></head><body>"
    "<h2>Ethernet IP-Konfiguration</h2>"
    "<form method='post' action='/save'>"
    "<fieldset><legend>Modus</legend>"
    "<label><input type='radio' name='mode' value='dhcp' %s> DHCP (automatisch)</label>"
    "<label><input type='radio' name='mode' value='static' %s> Statische IP</label>"
    "</fieldset>"
    "<fieldset><legend>Statische IP (nur bei Modus &quot;Statisch&quot;)</legend>"
    "<label>IP-Adresse:<input type='text' name='ip' value='%s'></label>"
    "<label>Subnetzmaske:<input type='text' name='mask' value='%s'></label>"
    "<label>Gateway:<input type='text' name='gw' value='%s'></label>"
    "</fieldset>"
    "<button class='btn' type='submit'>Speichern und Neustart</button>"
    "</form>"
    "<p class='info'>Aktuelle ETH-IP: <b>%s</b><br>"
    "WLAN: <b>" AP_SSID "</b> &bull; Portal: 192.168.4.1</p>"
    "</body></html>";

static esp_err_t handler_get(httpd_req_t *req)
{
    eth_ip_cfg_t cfg = eth_ip_cfg_load();
    char eth_ip[20];
    get_eth_ip_str(eth_ip, sizeof(eth_ip));

    char *page = malloc(2048);
    if (!page) return ESP_ERR_NO_MEM;

    snprintf(page, 2048, HTML_TMPL,
             cfg.use_static ? "" : "checked",   /* DHCP radio   */
             cfg.use_static ? "checked" : "",   /* Static radio */
             cfg.ip, cfg.mask, cfg.gw,
             eth_ip);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

static void restart_delayed(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static esp_err_t handler_save(httpd_req_t *req)
{
    int body_len = req->content_len;
    if (body_len <= 0 || body_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    char *body = malloc(body_len + 1);
    if (!body) return ESP_ERR_NO_MEM;

    int received = httpd_req_recv(req, body, body_len);
    if (received != body_len) { free(body); return ESP_FAIL; }
    body[body_len] = '\0';
    ESP_LOGD(TAG, "POST body: %s", body);

    eth_ip_cfg_t cfg = {};
    char mode_str[12] = {};
    form_get(body, "mode", mode_str, sizeof(mode_str));
    cfg.use_static = (strcmp(mode_str, "static") == 0);
    form_get(body, "ip",   cfg.ip,   sizeof(cfg.ip));
    form_get(body, "mask", cfg.mask, sizeof(cfg.mask));
    form_get(body, "gw",   cfg.gw,   sizeof(cfg.gw));
    free(body);

    /* Defaults wenn Felder leer (bei DHCP-Auswahl normal) */
    if (cfg.ip[0]   == '\0') strlcpy(cfg.ip,   "192.168.1.100",  sizeof(cfg.ip));
    if (cfg.mask[0] == '\0') strlcpy(cfg.mask, "255.255.255.0",  sizeof(cfg.mask));
    if (cfg.gw[0]   == '\0') strlcpy(cfg.gw,   "192.168.1.1",   sizeof(cfg.gw));

    if (cfg_save(&cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Konfiguration gespeichert: mode=%s ip=%s mask=%s gw=%s",
                 cfg.use_static ? "statisch" : "DHCP",
                 cfg.ip, cfg.mask, cfg.gw);
    }

    static const char *OK_HTML =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3;url=/'>"
        "</head><body style='font-family:sans-serif;max-width:400px;margin:2em auto;padding:0 1em'>"
        "<h2>Gespeichert!</h2>"
        "<p>Neustart in 3 Sekunden...</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, OK_HTML, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(restart_delayed, "restart", 1024, NULL, 3, NULL);
    return ESP_OK;
}

/* Catch-all: Captive-Portal-Redirect fuer Betriebssystem-Erkennung */
static esp_err_t handler_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Minimaler DNS-Server (alle Anfragen → 192.168.4.1)                  */
/* ------------------------------------------------------------------ */

/* Gibt den Offset hinter dem QNAME+QTYPE+QCLASS zurueck */
static int dns_question_end(const uint8_t *buf, int len)
{
    int i = 12;
    while (i < len) {
        uint8_t lb = buf[i];
        if (lb == 0) { i++; break; }
        if ((lb & 0xC0) == 0xC0) { i += 2; break; }
        i += lb + 1;
    }
    return i + 4; /* +QTYPE(2) +QCLASS(2) */
}

static int build_dns_response(const uint8_t *q, int qlen,
                               uint8_t *resp, uint32_t ip_be)
{
    int qend = dns_question_end(q, qlen);
    if (qend > qlen || qend + 16 > 512) return -1;

    memcpy(resp, q, qend);
    resp[2] = 0x81; /* QR=1, AA=1, TC=0, RD=1 */
    resp[3] = 0x80; /* RA=0, RCODE=0           */
    resp[6] = 0x00; resp[7] = 0x01; /* ANCOUNT = 1 */

    int p = qend;
    resp[p++] = 0xC0; resp[p++] = 0x0C; /* Pointer auf Offset 12 */
    resp[p++] = 0x00; resp[p++] = 0x01; /* Type A                */
    resp[p++] = 0x00; resp[p++] = 0x01; /* Class IN              */
    resp[p++] = 0x00; resp[p++] = 0x00;
    resp[p++] = 0x00; resp[p++] = 0x3C; /* TTL = 60 s            */
    resp[p++] = 0x00; resp[p++] = 0x04; /* RDLENGTH = 4          */
    memcpy(resp + p, &ip_be, 4); p += 4;
    return p;
}

static void dns_server_task(void *arg)
{
    /* AP-IP in Netzwerk-Byte-Order */
    uint32_t ap_ip_be;
    inet_pton(AF_INET, "192.168.4.1", &ap_ip_be);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS socket: %s", strerror(errno)); vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind: %s", strerror(errno));
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS-Server gestartet (Port 53)");

    static uint8_t buf[512];
    static uint8_t resp[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        int rlen = build_dns_response(buf, n, resp, ap_ip_be);
        if (rlen > 0) {
            sendto(sock, resp, rlen, 0,
                   (struct sockaddr *)&client, clen);
        }
    }
}

/* ------------------------------------------------------------------ */
/* WiFi-AP                                                              */
/* ------------------------------------------------------------------ */

static void wifi_ap_init(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = AP_SSID,
            .ssid_len        = sizeof(AP_SSID) - 1,
            .channel         = AP_CHANNEL,
            .authmode        = WIFI_AUTH_OPEN,   /* Kein Passwort */
            .max_connection  = AP_MAX_CONN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi-AP gestartet: SSID=\"%s\" (offen)", AP_SSID);
}

/* ------------------------------------------------------------------ */
/* HTTP-Server                                                          */
/* ------------------------------------------------------------------ */

static void http_server_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server Start fehlgeschlagen");
        return;
    }

    static const httpd_uri_t uri_get = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handler_get,
    };
    static const httpd_uri_t uri_save = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = handler_save,
    };
    static const httpd_uri_t uri_wild = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handler_catchall,
    };

    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_wild);
    ESP_LOGI(TAG, "HTTP-Server gestartet auf Port 80");
}

/* ------------------------------------------------------------------ */
/* Oeffentliche API                                                     */
/* ------------------------------------------------------------------ */

esp_err_t wifi_captive_start(esp_netif_t *eth_netif)
{
    s_eth_netif = eth_netif;
    wifi_ap_init();
    http_server_init();
    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Captive Portal bereit: http://192.168.4.1");
    return ESP_OK;
}
