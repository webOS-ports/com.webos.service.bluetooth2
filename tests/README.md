# com.webos.service.bluetooth2 test and hardening harness

Three layers, smallest first.

## Unit tests (`tests/unit/`)

GoogleTest suites for the parts that work without a bus, linked with the service's own sources (all but
`main.cpp`). They pin down what the `herrie/hardening` audit corrected:

- file names from Luna clients stay inside the storage directory: no `..`, no absolute path, no symbolic
  link leading out; `checkFileIsValid()` takes regular files only;
- GATT server, client and instance ids out of range are rejected, not thrown or truncated;
- the SPP binary socket: buffering before a client connects, both directions, a client that hung up
  (without SIGPIPE), a second client replacing the first, back-pressure and large writes arriving whole
  and in order, removing the socket with a retry pending;
- the channel manager's queued data and read-subscription timers, and the pair state when the device
  being paired is removed.

Build them with the component, off by default:

    cmake -DBUILD_TESTING=ON ...
    ninja webos-bluetooth-service-tests

They change into no directory: the binary socket tests create `bt-test-sockets/` in the current one. On
the build host the x86-64 build runs through its recipe sysroot's loader:

    <sysroot>/usr/lib/ld-linux-x86-64.so.2 --library-path <sysroot>/usr/lib \
        build/tests/webos-bluetooth-service-tests

Each fix, put back into the service, makes at least one test fail; most of them only under
AddressSanitizer, so run the suite with `-fsanitize=address,undefined` too.

## Hardening (`tests/hardening/`)

`run-valgrind.sh <webos-bluetooth-service-tests> [recipe-sysroot]` runs the unit tests under valgrind
memcheck and fails on any memory error or definite leak.

## On the device (`tests/device/`)

Copy `run-on-device.sh` and the device's `webos-bluetooth-service-tests` into one directory and run the
script as root with the service installed. It runs the unit tests, then sends the running service the
calls that crashed it before the hardening (`le/disableAdvertising` before any advertiser,
`gatt/closeServer` with an id beyond 65535, a device list filtered by uuid) and checks that it answers
each and keeps its pid, then watches its memory over 1200 `gatt/getStatus` calls.
