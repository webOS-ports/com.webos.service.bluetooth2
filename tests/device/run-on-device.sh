#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
# SPDX-License-Identifier: Apache-2.0
#
# Run as root on a device with the service installed, next to webos-bluetooth-service-tests.
# 1. The unit tests.
# 2. Calls that crashed the service before the hardening, against the running service; it must answer
#    each one and keep its pid.
# 3. Memory over repeated GATT getStatus calls, each of which leaked its message before.
B=luna://com.webos.service.bluetooth2
DIR=$(dirname "$0")
fail=0

call() { timeout 10 luna-send -n 1 -a com.palm.app.settings "$B/$1" "$2" 2>&1; }
pid() { systemctl show -p MainPID --value webos-bluetooth-service; }
same() {
	if [ "$1" = "$(pid)" ]; then echo "   ok, still pid $1"; else echo "   FAIL: the service restarted"; fail=1; fi
}

echo "== unit tests"
(cd "$DIR" && ./webos-bluetooth-service-tests) | tail -3 || fail=1

P=$(pid)
echo "== le/disableAdvertising before any advertiser was started (dereferenced a null watch)"
call le/disableAdvertising '{"advertiserId":1}'; same "$P"
echo "== gatt/closeServer with an id beyond 65535 (std::stoi threw)"
call gatt/closeServer '{"serverId":"99999999999"}'; same "$P"
echo "== device/internal/getStatus filtered by uuid (std::find over two temporaries)"
call device/internal/getStatus '{"uuid":"0000110b-0000-1000-8000-00805f9b34fb"}'; same "$P"

echo "== memory over 3x400 gatt/getStatus"
R="$(grep VmRSS /proc/$P/status | awk '{print $2}')"
for n in 1 2 3; do
	i=0; while [ $i -lt 400 ]; do luna-send -n 1 -a com.palm.app.settings $B/gatt/getStatus '{}' >/dev/null 2>&1; i=$((i+1)); done
	R="$R -> $(grep VmRSS /proc/$P/status | awk '{print $2}')"
done
echo "   VmRSS kB: $R"; same "$P"

[ $fail = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
