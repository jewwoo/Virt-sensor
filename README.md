# virt-sensor(Virtual Sensor) — Linux char device + epoll daemon + HTTP API

embedded Linux style stack:
- **Kernel module (C)** exposes a virtual sensor as a **character device**: `/dev/virt_sensor0`
- **User-space daemon (C)** consumes the device using **epoll** (event-driven, no busy-loop)
- **HTTP API** lets you query telemetry and configure the driver at runtime (via **ioctl**)



---

## Features

### Kernel module (`kernel/`)
- Creates `/dev/virt_sensor0`
- Periodically updates a virtual “temperature” value (in milli-degrees Celsius)
- Implements:
  - `read()` → returns JSON line with `temp_milli_c` + `interval_ms`
  - `poll()` → signals when a fresh sample is ready
  - `ioctl(VS_IOC_SET_INTERVAL_MS)` → update sampling interval at runtime

### Daemon (`daemon/`)
- Uses **epoll** to wait on:
  - device readiness (from driver `poll()`)
  - incoming HTTP connections
- Maintains:
  - `current_temp_milli_c`
  - rolling average over a fixed window (default: 50 samples)

### HTTP API (served on `127.0.0.1:8080`)
- `GET /current` → current sensor reading
- `GET /stats` → rolling statistics
- `GET /config?interval_ms=500` → set driver sampling interval (ms)

---



---

## Requirements

This project must be **built and run on Linux**.

Recommended on Windows: **WSL2 (Ubuntu)**.

You need:
- `gcc`, `make`
- Linux kernel headers matching your running kernel

---

## Build & run (Ubuntu / Debian / WSL2)

### 1) Install dependencies
```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r)

cd kernel
make

cd ../daemon
make

./virt_sensord

virt_sensord: epoll on /dev/virt_sensor0 + http://127.0.0.1:8080


### API USage

curl http://127.0.0.1:8080/current


### Unload 

sudo rmmod virt_sensor
dmesg | tail -n 20
