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
  being paired is removed;
- the ANCS parser, on notification source values and data source blocks split every way.

Build them with the component, off by default:

    cmake -DBUILD_TESTING=ON ...
    ninja webos-bluetooth-service-tests

They change into no directory: the binary socket tests create `bt-test-sockets/` in the current one. On
the build host the x86-64 build runs through its recipe sysroot's loader:

    <sysroot>/usr/lib/ld-linux-x86-64.so.2 --library-path <sysroot>/usr/lib \
        build/tests/webos-bluetooth-service-tests

Each fix, put back into the service, makes at least one test fail; most of them only under
AddressSanitizer, so run the suite with `-fsanitize=address,undefined` too.

The ANCS tests feed `ancsparser.cpp` data source responses cut into blocks at the awkward places: a value,
an attribute length or an id split across blocks, a block that starts with attribute 0 (which used to be taken
for a new header), the rest of an attribute nobody asked for.

## Fuzzing (`tests/fuzz/`)

`fuzz_ancsparser.cpp` is a libFuzzer target for the ANCS parser, which parses what a phone sends. It needs clang:

    cmake -DBUILD_TESTING=ON -DBUILD_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ ...
    ninja fuzz-ancsparser
    ./tests/fuzz-ancsparser -max_total_time=3600 corpus/

Besides AddressSanitizer and UBSan it aborts when the parser's state stops making sense (a pending length with
no attribute, a length beyond 65535).

## Scenarios on a fake SIL (`tests/fakesil/`, `tests/scenarios/`)

The profiles need a bus and a remote device, so they are driven on a target instead: the real service, built with
sanitizers, on `fake.so`, a SIL with no Bluetooth behind it. A test tells the fake through a FIFO what the
"remote side" does next: make a call fail or wait, end a session, report a transfer the service never started.
Every call the service makes into the SIL is logged for the test to check.

`run-scenarios.py` covers what can only go wrong with a device: a MAP session the phone ends after a connect
without subscription, a subscribed connect that fails, a client's own disconnect and its notification
subscription, PBAP folders per device and on success only, two subscribers to one phone book pull, mesh requests
that are refused, and an OPP state change for a transfer that is not ours. Each scenario restarts the service on a
fresh fake. Run on the service before these fixes, every scenario fails.

On a qemux86-64 LuneOS image (as root):

1. build with `-DBUILD_TESTING=ON`, with ASan and UBSan, and with the image's loader
   (`-Wl,--dynamic-linker=/usr/lib/ld-linux-x86-64.so.2`; clang defaults to `/lib64`);
2. install `webos-bluetooth-service` and `tests/fake.so` (to `/usr/lib/bluetooth-sils/`);
3. put in `/var/systemd/system/env/webos-bluetooth-service.env`:

       WEBOS_BLUETOOTH_SIL=fake
       ASAN_OPTIONS=log_path=/var/log/bt2-asan/asan:detect_leaks=1
       UBSAN_OPTIONS=log_path=/var/log/bt2-asan/ubsan:print_stacktrace=1

4. run `run-scenarios.py`. It fails on any scenario, and on any sanitizer report that points at the service or
   the fake: every error, and every leak with one of their frames in it. luna-service2 keeps some transport data
   until exit; those leaks have only its own frames and are left out.

`luna-send` writes its replies only to a terminal, so the script runs it on a pseudo-terminal. On the image the
role file may not let the service answer `com.webos.lunasend-*`; add it to the outbound list there.

## Hardening (`tests/hardening/`)

`run-valgrind.sh <webos-bluetooth-service-tests> [recipe-sysroot]` runs the unit tests under valgrind
memcheck and fails on any memory error or definite leak.

## On the device (`tests/device/`)

Copy `run-on-device.sh` and the device's `webos-bluetooth-service-tests` into one directory and run the
script as root with the service installed. It runs the unit tests, then sends the running service the
calls that crashed it before the hardening (`le/disableAdvertising` before any advertiser,
`gatt/closeServer` with an id beyond 65535, a device list filtered by uuid) and checks that it answers
each and keeps its pid, then watches its memory over 1200 `gatt/getStatus` calls.

`phone-scenarios.py <phone address>` runs MAP and PBAP against a real, paired phone through bluez5: folders and
messages, a session the phone ends (the link is dropped under it) and a new connect, the client's own disconnect
ending its notification subscription, a subscriber whose leaving disconnects, and a phone book pull into the
folder set, not one a refused change named. Android asks to allow message and contact access on first use; a
refused or timed-out prompt shows as an OBEX connect that times out or is disconnected at once, until sharing is
enabled in the phone's settings for this device. Nothing read from the phone is printed and pulled files are
deleted.
