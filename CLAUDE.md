# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Projekt

ESP-IDF (v5.5.4) Projekt für das **Waveshare ESP32-S3-ETH** (ESP32-S3 + W5500 SPI-Ethernet) als iperf-Benchmark-Station.
Implementiert das iperf**2**-Protokoll (kompatibel mit `iperf`, nicht `iperf3`).

## Build & Flash

```bash
# ESP-IDF Umgebung laden (WSL/bash)
. ~/esp/esp-idf/export.sh

# Erstmalig: Target setzen und bauen
rm -rf build sdkconfig
idf.py set-target esp32s3
idf.py build

# Flashen (WSL2 kann ttyS* nicht konfigurieren → Windows Python nutzen)
cd build && /mnt/c/Users/Thomas/.espressif/python_env/idf5.4_py3.11_env/Scripts/python.exe \
  -m esptool --chip esp32s3 -p COM3 -b 460800 --before default_reset --after hard_reset \
  write_flash @flash_args

# Monitor (funktioniert mit ttyS3 da nur lesen)
idf.py -p /dev/ttyS3 monitor
```

## Projektstruktur

- `main/main.c` — Ethernet-Init + iperf-Konsole
- `main/idf_component.yml` — Abhängigkeiten: `ethernet_init`, `iperf-cmd`, `cmd_system`
- `sdkconfig.defaults` — W5500 SPI-Pinbelegung + Ethernet-Konfiguration
- `sdkconfig.defaults.esp32s3` — Performance-Optimierungen (Dual-Core, IRAM, 240 MHz)

Die `ethernet_init`-Komponente kommt aus `$IDF_PATH/examples/ethernet/basic/components/ethernet_init`
und wird über Kconfig konfiguriert (nicht durch Code-Änderungen).

## Waveshare ESP32-S3-ETH Hardware (W5500 via SPI)

| Signal   | GPIO |
|----------|------|
| SCLK     | 13   |
| MOSI     | 11   |
| MISO     | 12   |
| CS       | 14   |
| INT      | 10   |
| RST      | 9    |

SPI-Host: SPI2 (Host-Nr. 1), Takt: 25 MHz.

## Konsolenbefehle (nach DHCP-IP)

```
iperf -s -i 3                    # TCP-Server
iperf -u -s -i 3                 # UDP-Server (+ Paketverlust)
iperf -c <ip> -t 30 -i 3         # TCP-Client
iperf -u -c <ip> -b 100M -t 30   # UDP-Client
iperf [-u] -r -c <ip> -t 30      # Reverse (Gegenstelle sendet)
iperf --help                     # alle Optionen
restart                          # Neustart
tasks                            # FreeRTOS Task-Statistik
```

Gegenstelle (Linux/Mac): `iperf -s` bzw. `iperf -u -s`

## Performance-Hinweise

- Dual-Core-Modus: SPI-DMA und iperf-Task laufen auf verschiedenen Kernen.
- Erwarteter Durchsatz: ~50–70 Mbit/s (W5500 SPI-limitiert bei 25 MHz).
- Watchdogs sind in `sdkconfig.defaults.esp32s3` deaktiviert, um Benchmark-Unterbrechungen zu vermeiden.
- Für reproduzierbare Messungen: feste IP per DHCP-Reservierung vergeben.
