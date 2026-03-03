#!/usr/bin/env bash
set -euo pipefail
sudo rmmod virt_sensor || true
dmesg | tail -n 20