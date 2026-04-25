#!/bin/bash
# Automated run of the bh_manager test harness.
# Builds, loads, waits for scenarios, unloads, and verifies expected
# dmesg patterns. Also runs a kmemleak scan if available.
#
# Usage:
#   ./run_test.sh           # default 25s scenario window
#   ./run_test.sh 35        # 35s scenario window

set -euo pipefail

cd "$(dirname "$0")"

WAIT_SEC=${1:-25}
KMEMLEAK=/sys/kernel/debug/kmemleak
FAIL=0

# ---------- helpers ----------

color() {
	local code=$1; shift
	printf "\033[%sm%s\033[0m" "$code" "$*"
}

check() {
	local desc=$1 pattern=$2
	if echo "$DMESG" | grep -E -q -- "$pattern"; then
		echo "$(color '32' PASS): $desc"
	else
		echo "$(color '31' FAIL): $desc"
		echo "       pattern: $pattern"
		FAIL=1
	fi
}

check_absent() {
	local desc=$1 pattern=$2
	if echo "$DMESG" | grep -E -q -- "$pattern"; then
		echo "$(color '31' FAIL): $desc"
		echo "       unexpected: $pattern"
		FAIL=1
	else
		echo "$(color '32' PASS): $desc"
	fi
}

# ---------- build + run ----------

echo "==> build"
make -s all

echo "==> clear dmesg + insmod"
sudo dmesg -C || true

# kmemleak: clear accumulated suspects before our test so we can attribute
# anything found later to this run.
if [ -w "$KMEMLEAK" ]; then
	echo "==> clearing kmemleak baseline"
	echo clear | sudo tee "$KMEMLEAK" >/dev/null
fi

sudo insmod bhm_test.ko

echo "==> scenarios running (${WAIT_SEC}s)"
sleep "${WAIT_SEC}"

echo "==> rmmod"
sudo rmmod bhm_test

DMESG=$(sudo dmesg | grep bhm_test || true)

echo
echo "==> dmesg (bhm_test lines)"
echo "$DMESG"
echo

# ---------- verification ----------

echo "==> verification"

# SC1: hysteresis
check "SC1 global L0→L1"       'cb\[global +\].*level=1 tx=1[0-9]{2}Mbps'
check "SC1 global L1→L2"       'cb\[global +\].*level=2 tx=5[0-9]{2}Mbps'
check "SC1 global L2→L0"       'cb\[global +\].*level=0.*ov=0'
check "SC1 wlan0  L0→L1"       'cb\[wlan0 +\].*level=1'
check "SC1 wlan0  L1→L2"       'cb\[wlan0 +\].*level=2'
check "SC1 wlan0  L2→L0"       'cb\[wlan0 +\].*level=0'

# SC2: filter separation — wlan0 must NOT fire between SC2 start and SC3 start
SC2_DMESG=$(echo "$DMESG" | awk '/SC2:/{f=1} /SC3:/{f=0} f')
if echo "$SC2_DMESG" | grep -E -q -- 'cb\[wlan0 +\]'; then
	echo "$(color '31' FAIL): SC2 wlan0 silent during bhmtest1 traffic"
	echo "       unexpected wlan0 activity during SC2"
	FAIL=1
else
	echo "$(color '32' PASS): SC2 wlan0 silent during bhmtest1 traffic"
fi
check "SC2 global reacts to bhmtest1" 'cb\[global +\].*level=1.*ov=0'

# SC3: override net-change filter
# Between "fast toggle" log and "stable force" log, expect NO cb[override]
SC3_FAST=$(echo "$DMESG" | awk '/SC3: fast toggle/{f=1} /SC3: stable force/{f=0} f')
if echo "$SC3_FAST" | grep -E -q -- 'cb\[override\]'; then
	echo "$(color '31' FAIL): SC3 fast-toggle suppressed"
	echo "       unexpected cb during fast toggle"
	FAIL=1
else
	echo "$(color '32' PASS): SC3 fast-toggle suppressed"
fi
check "SC3 stable force fires ov=1" 'cb\[override\].*ov=1'
check "SC3 stable clear fires ov=0" 'cb\[override\].*ov=0'

# SC4: accessor
check "SC4 accessor_fn invoked" 'accessor_fn: .*bh=[0-9a-fx]+.*current_level='

# SC5: threaded NAPI (conditionally present)
if echo "$DMESG" | grep -q "SC5: threaded NAPI"; then
	check "SC5 tnapi L0→L1" 'cb\[tnapi +\].*level=1'
fi

# SC6: CPU affinity
check "SC6 at least one CPU test" 'SC6: preferred cpu='
# Count accessor_fn invocations during SC6. Should match the number of
# "preferred cpu=" lines (one per tested CPU).
SC6_DMESG=$(echo "$DMESG" | awk '/SC6:/{f=1} /all scenarios done/{f=0} f')
N_PREF=$(echo "$SC6_DMESG" | grep -c 'SC6: preferred cpu=' || true)
N_ACC=$(echo "$SC6_DMESG" | grep -c 'accessor_fn:' || true)
if [ "$N_PREF" -gt 0 ] && [ "$N_ACC" -ge "$N_PREF" ]; then
	echo "$(color '32' PASS): SC6 accessor ran for each preferred CPU ($N_ACC/$N_PREF)"
else
	echo "$(color '31' FAIL): SC6 accessor count mismatch (acc=$N_ACC pref=$N_PREF)"
	FAIL=1
fi

# ---------- kmemleak scan ----------

if [ -r "$KMEMLEAK" ]; then
	echo
	echo "==> kmemleak scan"
	echo scan | sudo tee "$KMEMLEAK" >/dev/null
	sleep 5
	LEAKS=$(sudo cat "$KMEMLEAK" 2>/dev/null || true)
	if [ -z "$LEAKS" ]; then
		echo "$(color '32' PASS): kmemleak — no leaks reported"
	else
		if echo "$LEAKS" | grep -q -E 'bh_manager|bhm_|bhm_test'; then
			echo "$(color '31' FAIL): kmemleak — leaks referencing our module:"
			echo "$LEAKS" | head -40
			FAIL=1
		else
			echo "$(color '33' WARN): kmemleak — leaks present but none look like ours"
			echo "$LEAKS" | head -10
		fi
	fi
else
	echo "==> kmemleak unavailable (skipped)"
fi

# ---------- summary ----------

echo
if [ "$FAIL" = "0" ]; then
	echo "$(color '1;32' '==> ALL CHECKS PASSED')"
	exit 0
else
	echo "$(color '1;31' '==> SOME CHECKS FAILED')"
	exit 1
fi
