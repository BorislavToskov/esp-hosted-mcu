# ESP32-P4 + ESP32-C6: Shared Network + UART Data Bridge Demo

This document describes how to build, configure, and run the custom two-device demo that combines:

- **ESP32-C6** (Slave / Co-processor) — provides WiFi connectivity over SDIO and bridges incoming UART data to a remote TCP server.
- **ESP32-P4** (Host) — consumes the shared network via SDIO, synchronises time over SNTP, and transmits timestamps every second to the ESP32-C6 over UART.

---

## System Overview

```
                    ┌─────────────────────────────────┐
 TCP Server ◄───────┤  ESP32-C6  (Slave / Wi-Fi)       │
 192.168.x.x:23    │                                   │
                    │  • Connects to Wi-Fi AP           │
                    │  • Runs TCP client → server       │
                    │  • Receives UART data from P4     │
                    │  • Forwards data to TCP server    │
                    └──────┬──────────────┬────────────┘
                           │  SDIO        │  UART
                           │  (network)   │  (data)
                    ┌──────┴──────────────┴────────────┐
                    │  ESP32-P4  (Host / Application)   │
                    │                                   │
                    │  • Gets IP via SDIO from C6       │
                    │  • Syncs time via SNTP            │
                    │  • Sends timestamps over UART     │
                    │  • iPerf console for testing      │
                    └───────────────────────────────────┘
```

**Data flow:**
1. ESP32-C6 joins a Wi-Fi AP and shares the network with ESP32-P4 over SDIO.
2. ESP32-P4 obtains an IP address, starts SNTP, and once synced sends ISO-8601 timestamps (e.g. `2024-12-01T10:30:00Z`) to ESP32-C6 once per second over UART.
   Before sync it sends `NO_SYNC`.
3. ESP32-C6 receives each line on UART and forwards it to the configured TCP server.
   If UART is silent for more than 1 second it sends a `PING N` keepalive to keep the TCP connection alive.

---

## Hardware Connections

### SDIO Bus  (ESP32-P4 ↔ ESP32-C6)

These pins carry the network traffic between the two chips.

| Signal | ESP32-P4 GPIO | ESP32-C6 GPIO | Notes |
|:-------|:-------------:|:-------------:|:------|
| CLK    | 18            | 19            | Clock |
| CMD    | 19            | 18            | Command |
| D0     | 14            | 20            | Data 0 |
| D1     | 15            | 21            | Data 1 |
| D2     | 16            | 22            | Data 2 |
| D3     | 17            | 23            | Data 3 |
| RESET  | 54 (output)   | EN (input)    | P4 resets C6 on boot |
| GND    | GND           | GND           | Common ground |

> **Note:** These match the `sdkconfig.defaults.esp32p4` values in
> `examples/host_network_split__power_save/`.

### UART Data Channel  (ESP32-P4 → ESP32-C6)

This is the secondary, application-level link that carries timestamp data.
TX of one device connects to RX of the other (cross-connection).

| Signal        | ESP32-P4 GPIO | ESP32-C6 GPIO |
|:--------------|:-------------:|:-------------:|
| Data TX → RX  | **10** (TX)   | **4**  (RX)   |
| Data RX ← TX  | **11** (RX)   | **5**  (TX)   |
| GND           | GND           | GND           |

Both sides use **115200 baud, 8N1, no flow control**.

---

## Firmware Overview

### ESP32-C6 — Slave firmware (`slave/`)

| Source file | Purpose |
|:------------|:--------|
| `slave/main/app_main.c` | Entry point, ESP-Hosted slave init, calls `demo_app_start()` |
| `slave/main/demo_app.c` | UART listener + TCP forwarder (custom code) |

**`demo_app.c` behaviour:**
- Initialises UART1 on GPIO 4/5 at 115200 baud.
- Spawns `uart_rx_task` which blocks on UART with a 1-second timeout.
- **On UART data:** logs the raw bytes and sends them to the TCP server.
- **On timeout:** sends a `PING N\r\n` keepalive (N increments each time).
- Opens a TCP connection to `CONFIG_TCP_SERVER_IP : CONFIG_TCP_SERVER_PORT` only after the WiFi interface has a valid IP.
- Reconnects automatically on any send failure.

### ESP32-P4 — Host firmware (`examples/host_network_split__power_save/`)

| Source file | Purpose |
|:------------|:--------|
| `main/iperf_example_main.c` | App entry, WiFi init, SNTP, UART TX, iPerf console |

**Key behaviour:**
- Calls `uart_data_init()` — sets up UART1 on GPIO 10/11 at 115200 baud and starts `uart_data_task`.
- `uart_data_task` wakes every 1 second and sends:
  - `2024-12-01T10:30:00Z\r\n` — once SNTP is synced.
  - `NO_SYNC\r\n` — while waiting for SNTP.
- Registers an `IP_EVENT_STA_GOT_IP` handler that starts SNTP automatically once an IP is obtained.
- Exposes an interactive `iperf>` REPL on the console UART for WiFi and iPerf commands.

---

## Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) installed and sourced.
- Two USB cables — one for each board.
- A PC running a simple TCP server on the target IP/port (see [TCP server setup](#tcp-server-setup)).

---

## Step 1 — Build and Flash the ESP32-C6 Slave

### 1.1 — Set target

```bash
cd slave
idf.py set-target esp32c6
```

### 1.2 — Configure TCP server address

Open the configuration menu:

```bash
idf.py menuconfig
```

Navigate to:

```
Example Configuration
└── TCP client config
    ├── TCP server IP address   →  <your server IP>   (default: 192.168.0.107)
    └── TCP server port         →  <your server port>  (default: 23)
```

> The default port 23 is the Telnet port; any listening TCP server will work.

Optionally pre-provision WiFi credentials (so the slave auto-connects on boot
without needing a console command from the host):

```
Example Configuration
└── Wi-Fi default config (pre-provisioning)
    ├── Enable                  →  [X]
    ├── WiFi SSID               →  <your SSID>
    └── WiFi Password           →  <your password>
```

> If you skip this, you must issue `sta_connect <SSID> <password>` from the
> **ESP32-P4** console after flashing both boards.

### 1.3 — Build and flash

```bash
idf.py build
idf.py -p <C6_SERIAL_PORT> flash monitor
```

Replace `<C6_SERIAL_PORT>` with the port for your ESP32-C6 (e.g. `COM3` on
Windows or `/dev/ttyUSB0` on Linux).

**Expected boot log (C6):**

```
I (xxx) esp_hosted_slave: transport: SDIO
I (xxx) demo_app: UART1 initialised — TX GPIO5, RX GPIO4, 115200 baud
I (xxx) demo_app: No IP yet, dropping packet     ← waiting for WiFi
...
I (xxx) wifi: connected to AP "<SSID>"
I (xxx) demo_app: Connected to 192.168.x.x:23
I (xxx) demo_app: UART RX (24 bytes):            ← timestamps arriving from P4
```

---

## Step 2 — Build and Flash the ESP32-P4 Host

### 2.1 — Set target

```bash
cd examples/host_network_split__power_save
idf.py set-target esp32p4
```

### 2.2 — Review SDIO pin configuration

The file `sdkconfig.defaults.esp32p4` already contains the correct SDIO pin
assignments for the custom P4↔C6 wiring.  No manual change should be needed
unless your board uses different GPIOs.

```
# sdkconfig.defaults.esp32p4
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=18
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=19
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=14
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_SLOT_1=15
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_SLOT_1=16
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_SLOT_1=17
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
```

### 2.3 — Build and flash

```bash
idf.py build
idf.py -p <P4_SERIAL_PORT> flash monitor
```

Replace `<P4_SERIAL_PORT>` with the port for your ESP32-P4.

**Expected boot log (P4):**

```
I (xxx) uart_data: TX -> NO_SYNC          ← SNTP not yet synced
...
 ==================================================
 |       Steps to test WiFi throughput            |
 ...
iperf>
```

---

## Step 3 — Connect to WiFi (if not pre-provisioned)

At the `iperf>` prompt on the **ESP32-P4** console:

```
iperf> sta_connect <YOUR_SSID> <YOUR_PASSWORD>
```

Once connected you will see on the P4 console:

```
I (xxx) uart_data: SNTP started
I (xxx) uart_data: SNTP first sync: 1733046600
I (xxx) uart_data: TX -> 2024-12-01T10:30:00Z
I (xxx) uart_data: TX -> 2024-12-01T10:30:01Z
...
```

And on the C6 console:

```
I (xxx) demo_app: UART RX (24 bytes):
I (xxx) demo_app: Connected to 192.168.x.x:23
```

---

## Step 4 — TCP Server Setup

You need a TCP server listening on the IP and port configured in the slave.
Any of the following approaches work:

### Option A — netcat (Linux / macOS / WSL)

```bash
nc -l -p 23
```

Every line the C6 forwards will print in this terminal.

### Option B — ncat (Windows)

```powershell
ncat -l 23
```

### Option C — Python

```python
import socket

with socket.socket() as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', 23))
    s.listen(1)
    print('Waiting for connection...')
    conn, addr = s.accept()
    print(f'Connected from {addr}')
    with conn:
        while True:
            data = conn.recv(256)
            if not data:
                break
            print(data.decode(errors='replace'), end='')
```

**Example output on the TCP server:**

```
2024-12-01T10:30:00Z
2024-12-01T10:30:01Z
2024-12-01T10:30:02Z
PING 0
2024-12-01T10:30:04Z
...
```

> `PING N` lines appear when the P4 has not yet sent data (e.g., before SNTP
> sync) or when no byte arrives within a 1-second window.

---

## Optional — iPerf Network Throughput Test

Once the P4 has a WiFi connection you can run an iPerf TCP throughput test
between the P4 and another machine on the same network.

**On the PC (server):**
```bash
iperf -s -p 5001
```

**On the ESP32-P4 console (client):**
```
iperf> iperf -c <PC_IP> -p 5001 -t 10
```

Refer to [examples/host_network_split__power_save/README_iperf.md](examples/host_network_split__power_save/README_iperf.md) for the full iPerf test procedure.

---

## Changing UART Pins or Baud Rate

### ESP32-P4 side (`examples/host_network_split__power_save/main/iperf_example_main.c`)

```c
#define DATA_UART_PORT      UART_NUM_1
#define DATA_UART_TX_PIN    10        // ← change here
#define DATA_UART_RX_PIN    11        // ← change here
#define DATA_UART_BAUD      115200    // ← change here
#define DATA_SEND_PERIOD_MS 1000      // transmission interval (ms)
```

### ESP32-C6 side (`slave/main/demo_app.c`)

```c
#define DEMO_UART_PORT      UART_NUM_1
#define DEMO_UART_TX_PIN    GPIO_NUM_5   // ← change here
#define DEMO_UART_RX_PIN    GPIO_NUM_4   // ← change here
#define DEMO_UART_BAUD      115200       // ← change here
```

Both sides **must** use the same baud rate.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|:--------|:-------------|:----|
| C6 prints `No IP yet` forever | WiFi not connected | Run `sta_connect` from P4 console or enable pre-provisioning |
| C6 prints `connect() failed` | TCP server not running or wrong IP | Start `nc -l -p 23` on the target machine; verify `CONFIG_TCP_SERVER_IP` |
| P4 sends `NO_SYNC` endlessly | SNTP unreachable or no internet | Check firewall, ensure UDP/123 is open; `pool.ntp.org` must be reachable |
| No UART data arrives at C6 | Wiring error or pin mismatch | Verify P4 GPIO10→C6 GPIO4 and P4 GPIO11←C6 GPIO5 with a multimeter |
| SDIO bus fails to initialise | Loose wiring or wrong GPIO config | Check all 6 SDIO lines + reset; confirm `sdkconfig.defaults.esp32p4` pins |
| C6 prints only `PING N` lines | UART connected but P4 sending nothing | Check P4 monitor — `uart_data_task` should be logging `TX ->` lines |

---

## File Reference

| File | Description |
|:-----|:------------|
| [`slave/main/demo_app.c`](slave/main/demo_app.c) | ESP32-C6: UART RX + TCP forward task |
| [`slave/main/Kconfig.projbuild`](slave/main/Kconfig.projbuild) | ESP32-C6: menuconfig options (TCP IP/port, UART pins, WiFi pre-provisioning) |
| [`examples/host_network_split__power_save/main/iperf_example_main.c`](examples/host_network_split__power_save/main/iperf_example_main.c) | ESP32-P4: UART TX, SNTP, iPerf console |
| [`examples/host_network_split__power_save/sdkconfig.defaults.esp32p4`](examples/host_network_split__power_save/sdkconfig.defaults.esp32p4) | ESP32-P4: SDIO pin assignments |
| [`examples/host_network_split__power_save/sdkconfig.defaults`](examples/host_network_split__power_save/sdkconfig.defaults) | Common host defaults (Network Split + Power Save enabled) |
| [`slave/README.md`](slave/README.md) | General ESP-Hosted slave firmware guide |
| [`examples/host_network_split__power_save/README.md`](examples/host_network_split__power_save/README.md) | Host Network Split + Power Save feature guide |
