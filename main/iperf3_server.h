/* iperf3 server emulation for ESP32-S3 / W5500
 * Implements the iperf3 wire protocol (TCP receive, port 5201)
 * Compatible with: iperf3 -c <ip>
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start iperf3 server (non-blocking, spawns a task)
 * @return ESP_OK on success, ESP_FAIL if already running
 */
esp_err_t iperf3_server_start(void);

/**
 * @brief Stop iperf3 server
 */
esp_err_t iperf3_server_stop(void);

/**
 * @brief Register "iperf3" console command
 */
esp_err_t iperf3_register_cmd(void);

#ifdef __cplusplus
}
#endif
