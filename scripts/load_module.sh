#!/usr/bin/env bash
set -euo pipefail
sudo insmod kernel/virt_sensor.ko || true
ls -l /dev/virt_sensor0
dmesg | tail -n 20