# nRF5340 Dual-Core Data Logger

This project is a custom dual-core data logger built for the **nRF5340 Development Kit** using the **nRF Connect SDK (NCS) v2.5.1**. 

It demonstrates how to effectively utilize both the Application Core and the Network Core simultaneously, communicating seamlessly via Zephyr's Inter-Processor Communication (IPC/RPMsg).

## Architecture Overview

The nRF5340 SOC divides tasks physically between its two processors:

* **Application Core (128 MHz):** Acts as the main brain. It reads sensors (currently simulating data), packages them into large 256-byte packets, stores them in an internal RAM buffer, and manages logic.
* **Network Core (64 MHz):** Dedicated entirely to wireless communication. It passively scans for specific mobile app triggers and handles BLE Extended Advertising to blast data payloads back to the phone.

---

## How It Works

### 1. Data Collection (App Core)
A timer fires every 750ms to simulate reading a 3-axis sensor. The application packages these readings into a **256-byte packet** format:
* `[0]` Header: `0x89`
* `[1]` Node ID: `5`
* `[2..241]` Payload: 240 bytes of sensor data.
* `[242..245]` Packet ID: Sequential identifier to prevent data loss.
* `[246]` Footer: `0xFE`

These packets are buffered into a 10-slot RAM Ring Buffer.

### 2. BLE Scanning & Triggering (Net Core)
The Network Core silently scans the BLE spectrum every 5 seconds. It looks for a specific "Manufacturer Data" signature transmitted by a remote mobile device:
* **Trigger Signature:** `0x59 0x00 0xBB 0xCC`
* **Reset Signature:** `0x59 0x00 0xFF 0xFF`

### 3. IPC Relay
When the Network Core detects the Trigger signature over the air, it sends an `IPC_MSG_TYPE_TRIGGER` command directly to the Application Core through shared memory. 

### 4. BLE Extended Advertising (Data Dump)
Upon receiving the IPC trigger, the Application Core extracts the stored packets from its RAM buffer and pushes them sequentially back through the IPC to the Network Core. 

Because standard BLE advertising only supports 31 bytes, the Network Core uses **BLE Extended Advertising** to broadcast the massive 256-byte payloads over the air. It transmits each packet 3 times rapidly to guarantee the mobile phone intercepts it.

---

## Directory Structure

```text
AWS_DataLogger/
├── Application_Core/      # Zephyr project for nrf5340dk_nrf5340_cpuapp
│   ├── src/main.c         # Data generation and IPC control logic
│   ├── prj.conf           # App Core configurations (GPIO, IPC, CPUNET init)
│   └── CMakeLists.txt
├── network_Core/          # Zephyr project for nrf5340dk_nrf5340_cpunet
│   ├── src/main.c         # Zephyr BLE Controller, Scanner, and Broadcaster
│   ├── prj.conf           # Net Core configurations (BLE, Extended Adv, RTT)
│   └── CMakeLists.txt
└── common/
    └── ipc_msg.h          # Shared header for defining IPC message structures
```

---

## Build & Flash Instructions

Because this repository separates the two cores into isolated projects, they must be built and flashed **in a specific order** using the `west` build tool.

### 1. Flash the Network Core
Flash the network core first so that it doesn't accidentally erase the application core.

```bash
cd network_Core
west build -b nrf5340dk_nrf5340_cpunet -p
west flash
```

### 2. Flash the Application Core
Flash the application core second. In `/prj.conf`, `CONFIG_BOARD_ENABLE_CPUNET=y` is set so that the Zephyr bootloader will automatically power on the network core before initializing the IPC drivers.

```bash
cd ../Application_Core
west build -b nrf5340dk_nrf5340_cpuapp -p
west flash
```

---

## Testing & Logs

**Viewing Application Core Logs:**
The Application core routes logs via UART. Open a serial terminal (like PuTTY or TeraTerm) and connect to **VCOM0** at `115200` baud. You will see:
```text
=== nRF5340 APP CORE — RAM Data Logger ===
[APP] Waiting for NET core IPC bind...
[APP] IPC ready
```

**Viewing Network Core Logs:**
Because the Network core lacks dedicated UART pins in this configuration, logging is routed through **SEGGER RTT**.
1. Open **J-Link RTT Viewer**.
2. Connect to the `nRF5340_XXAA_NET` target.
3. You will see:
```text
=== nRF5340 NET CORE — BLE stack ===
[NET] IPC endpoint bound
[NET] Scan schedule started
```
