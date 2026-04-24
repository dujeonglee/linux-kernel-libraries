#!/bin/bash
# Automated run of the bh_manager test harness.
# Builds the module, loads it, waits for scenarios to complete, unloads,
# and dumps the bhm_test lines from dmesg.
#
# Usage:
#   ./run_test.sh           # default 20s scenario window
#   ./run_test.sh 30        # 30s scenario window
#
# Intended to run inside the Lima VM. From the Mac:
#   limactl shell kbuild -- /path/to/run_test.sh

set -euo pipefail

cd "$(dirname "$0")"

WAIT_SEC=${1:-20}

echo "==> build"
make -s all

echo "==> clear dmesg + insmod"
sudo dmesg -C || true
sudo insmod bhm_test.ko

echo "==> scenarios running (${WAIT_SEC}s)"
sleep "${WAIT_SEC}"

echo "==> rmmod"
sudo rmmod bhm_test

echo
echo "==> dmesg (bhm_test lines)"
sudo dmesg | grep bhm_test || echo "(no bhm_test lines)"
