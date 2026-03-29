#pragma once
#include <stdbool.h>

/* 1.3" SH1106 I2C OLED — SDA=GPIO16, SCL=GPIO17 */

void oled_init(void);
void oled_set_eth_ip(const char *ip);       /* NULL/"" = show "---" */
void oled_set_iperf3_active(bool active);
