#!/usr/bin/env python3
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

# Drives the installed service, running on the fake SIL (tests/fakesil), through what a remote device does: a
# connect that fails, a session the phone ends, a transfer the service does not know. Each scenario restarts the
# service on a fresh fake SIL, so they do not depend on each other.
#
# Run as root on the target, with the service's environment file selecting the fake SIL (see tests/README.md):
#
#   run-scenarios.py [scenario-name...]
#
# luna-send only flushes its output to a terminal, so every client runs on a pseudo-terminal.

import json
import os
import pty
import select
import signal
import subprocess
import sys
import threading
import time

SERVICE = 'webos-bluetooth-service'
URI = 'luna://com.webos.service.bluetooth2/'
APP_ID = 'com.palm.app.settings'
FAKE_DIR = os.environ.get('FAKE_SIL_DIR', '/run/bt-fake-sil')
# Where the service's ASAN_OPTIONS/UBSAN_OPTIONS log_path writes, if it is built with sanitizers.
SANITIZER_LOGS = os.environ.get('SANITIZER_LOG_DIR', '/var/log/bt2-asan')
OUR_MODULES = ('/usr/sbin/webos-bluetooth-service', '/bluetooth-sils/fake.so')
ADAPTER = '00:11:22:33:44:00'
PHONE1 = 'aa:bb:cc:dd:ee:01'
PHONE2 = 'aa:bb:cc:dd:ee:02'
PHONE3 = 'aa:bb:cc:dd:ee:03'


class Client:
    """A luna-send on a pseudo-terminal: one call, or a subscription kept until stop()."""

    def __init__(self, method, payload, interactive=True, uri=URI, name=None):
        master, slave = pty.openpty()
        args = ['luna-send'] + (['-m', name] if name else ['-a', APP_ID]) + (['-i'] if interactive else ['-n', '1'])
        args += [uri + method, json.dumps(payload)]
        self.proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        self.master = master
        self.replies = []
        self.buffer = b''
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while True:
            try:
                ready, _, _ = select.select([self.master], [], [], 0.2)
                if not ready:
                    if self.proc.poll() is not None:
                        break
                    continue
                data = os.read(self.master, 4096)
            except OSError:
                break
            if not data:
                break
            self.buffer += data
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                line = line.strip()
                if line.startswith(b'{'):
                    try:
                        with self.lock:
                            self.replies.append(json.loads(line))
                    except ValueError:
                        pass

    def wait(self, count=1, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if len(self.replies) >= count:
                    return list(self.replies)
            time.sleep(0.05)
        with self.lock:
            return list(self.replies)

    def all(self):
        with self.lock:
            return list(self.replies)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        self.thread.join(1)
        try:
            os.close(self.master)
        except OSError:
            pass


def call(method, payload, timeout=5.0):
    client = Client(method, payload, interactive=False)
    replies = client.wait(1, timeout)
    client.stop()
    return replies[0] if replies else None


def ctl(command, timeout=5.0):
    acks = os.path.join(FAKE_DIR, 'acks')
    before = count_lines(acks)
    fd = os.open(os.path.join(FAKE_DIR, 'ctl'), os.O_WRONLY | os.O_NONBLOCK)
    os.write(fd, (command + '\n').encode())
    os.close(fd)
    end = time.time() + timeout
    while time.time() < end:
        if count_lines(acks) > before:
            last = read_lines(acks)[-1]
            if not last.endswith('-> ok'):
                raise RuntimeError('fake SIL: ' + last)
            return
        time.sleep(0.02)
    raise RuntimeError('fake SIL did not carry out: ' + command)


def read_lines(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def count_lines(path):
    return len(read_lines(path))


def calls():
    return read_lines(os.path.join(FAKE_DIR, 'calls'))


def settle(seconds=0.5):
    time.sleep(seconds)


def restart_service():
    subprocess.run(['systemctl', 'stop', SERVICE], check=False)
    # The mesh network the last run created stays in DB8, and only the service may remove it: with the service
    # stopped, luna-send takes its name.
    for kind in ('meshtoken', 'meshappkey', 'meshnodeinfo'):
        client = Client('del', {'query': {'from': 'com.webos.service.bluetooth2.%s:1' % kind}}, interactive=False,
                        uri='luna://com.webos.service.db/', name='com.webos.service.bluetooth2')
        client.wait(1)
        client.stop()
    subprocess.run(['rm', '-rf', FAKE_DIR], check=False)
    subprocess.run(['systemctl', 'reset-failed', SERVICE], check=False, stderr=subprocess.DEVNULL)
    subprocess.run(['systemctl', 'start', SERVICE], check=True)
    end = time.time() + 15
    while time.time() < end:
        if os.path.exists(os.path.join(FAKE_DIR, 'ctl')):
            status = call('adapter/getStatus', {}, timeout=2)
            if status and status.get('adapters'):
                break
        time.sleep(0.3)
    else:
        raise RuntimeError('service did not come up on the fake SIL')
    reply = call('adapter/setState', {'powered': True})
    if not reply or not reply.get('returnValue'):
        raise RuntimeError('could not power the fake adapter: %r' % reply)
    for phone in (PHONE1, PHONE2, PHONE3):
        ctl('device %s 1 MAP-0' % phone)
    settle(0.3)


def main_pid():
    out = subprocess.run(['systemctl', 'show', '-p', 'MainPID', '--value', SERVICE], capture_output=True, text=True)
    return out.stdout.strip()


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


# ---------------------------------------------------------------- scenarios

def map_session_ended_without_subscription():
    """D6: a session the phone ends is cleaned up when it was connected without a subscription."""
    first = call('map/connect', {'address': PHONE1, 'instanceName': 'MAP-0'})
    check(first and first.get('returnValue'), 'first connect failed: %r' % first)
    ctl('map.session %s_MAP-0 0' % PHONE1)
    settle()
    again = call('map/connect', {'address': PHONE1, 'instanceName': 'MAP-0'})
    check(again and again.get('returnValue'), 'connect after the phone ended the session was refused: %r' % again)


def map_failed_subscribed_connect_leaves_no_watch():
    """D7: a subscribed connect that fails leaves no watch; the next subscriber's leaving disconnects."""
    ctl('mode map.connect fail')
    failed = Client('map/connect', {'address': PHONE1, 'instanceName': 'MAP-0', 'subscribe': True})
    reply = failed.wait(1)
    check(reply and not reply[0].get('returnValue'), 'connect was meant to fail: %r' % reply)
    ctl('mode map.connect ok')
    second = Client('map/connect', {'address': PHONE1, 'instanceName': 'MAP-0', 'subscribe': True})
    reply = second.wait(1)
    check(reply and reply[0].get('returnValue'), 'second connect failed: %r' % reply)
    before = len(calls())
    second.stop()
    settle(1.0)
    disconnects = [c for c in calls()[before:] if c.startswith('map.disconnect')]
    failed.stop()
    check(disconnects, 'the second subscriber went away and the session was not disconnected')


def map_local_disconnect_ends_notifications():
    """D12: a disconnect asked for by the client ends the session's message notification subscription."""
    connect = call('map/connect', {'address': PHONE1, 'instanceName': 'MAP-0'})
    check(connect and connect.get('returnValue'), 'connect failed: %r' % connect)
    session = connect['sessionId']
    notifications = Client('map/getMessageNotification', {'address': PHONE1, 'sessionId': session, 'subscribe': True})
    first = notifications.wait(1)
    check(first and first[0].get('returnValue'), 'getMessageNotification failed: %r' % first)
    disconnect = call('map/disconnect', {'address': PHONE1, 'sessionId': session})
    check(disconnect and disconnect.get('returnValue'), 'disconnect failed: %r' % disconnect)
    replies = notifications.wait(2, 3)
    notifications.stop()
    ended = [r for r in replies[1:] if r.get('subscribed') is False]
    check(ended, 'the notification subscriber was not told the session ended: %r' % replies)


def pbap_folder_is_per_device_and_set_on_success():
    """D9: the phone book folder is the one each device changed to, and only once it did."""
    for phone in (PHONE2, PHONE3):
        reply = call('pbap/connect', {'address': phone})
        check(reply and reply.get('returnValue'), 'pbap connect %s failed: %r' % (phone, reply))
    ok = call('pbap/setPhoneBook', {'address': PHONE2, 'repository': 'internal', 'object': 'pb'})
    check(ok and ok.get('returnValue'), 'setPhoneBook failed: %r' % ok)
    ctl('mode pbap.setPhoneBook fail')
    refused = call('pbap/setPhoneBook', {'address': PHONE3, 'repository': 'sim1', 'object': 'cch'})
    check(refused and not refused.get('returnValue'), 'setPhoneBook was meant to fail: %r' % refused)
    ctl('mode pbap.setPhoneBook ok')

    before = len(calls())
    for phone in (PHONE2, PHONE3):
        reply = call('pbap/pullPhoneBook', {'address': phone, 'destinationFile': 'book.vcf', 'subscribe': True})
        check(reply and reply.get('returnValue'), 'pullPhoneBook %s failed: %r' % (phone, reply))
    pulls = [c for c in calls()[before:] if c.startswith('pbap.pullPhoneBook')]
    check(len(pulls) == 2, 'expected two pulls, got %r' % pulls)
    phone2, phone3 = pulls
    folder2 = PHONE2.replace(':', '_') + '/'
    folder3 = PHONE3.replace(':', '_') + '/'
    check(phone2.endswith(folder2 + 'internal/pb/book.vcf'), 'device 2 did not pull into the folder it changed to: %s' % phone2)
    # Device 3 never changed folder: it pulls at its root, not into the one it failed to change to, nor into
    # device 2's.
    check(phone3.endswith(folder3 + 'book.vcf'), 'device 3 did not pull at its root: %s' % phone3)


def pbap_second_subscriber_to_a_pull_is_told():
    """D12: a second subscribed pull of the same transfer gets its completion too."""
    reply = call('pbap/connect', {'address': PHONE2})
    check(reply and reply.get('returnValue'), 'pbap connect failed: %r' % reply)
    first = Client('pbap/pullPhoneBook', {'address': PHONE2, 'destinationFile': 'a.vcf', 'subscribe': True})
    second = Client('pbap/pullPhoneBook', {'address': PHONE2, 'destinationFile': 'b.vcf', 'subscribe': True})
    check(first.wait(1) and second.wait(1), 'pulls did not answer')
    ctl('pbap.transfer %s /media/internal/pbap/a.vcf /org/bluez/obex/client/session0/transfer0 completed' % PHONE2)
    one = first.wait(2, 3)
    two = second.wait(2, 3)
    first.stop()
    second.stop()
    check(len(one) >= 2, 'first subscriber got no completion: %r' % one)
    check(len(two) >= 2, 'second subscriber got no completion: %r' % two)


def mesh_rejected_requests_leave_no_watch():
    """D12: setOnOff and createNetwork that are refused do not keep getting other clients' results."""
    onoff = Client('mesh/model/onOff/set', {'destAddress': 2, 'appKeyIndex': 0, 'state': True, 'subscribe': True})
    reply = onoff.wait(1)
    check(reply and not reply[0].get('returnValue'), 'setOnOff without a network was meant to fail: %r' % reply)

    ctl('mode mesh.createNetwork fail')
    refused = Client('mesh/createNetwork', {})
    reply = refused.wait(1)
    check(reply and not reply[0].get('returnValue'), 'createNetwork was meant to fail: %r' % reply)
    ctl('mode mesh.createNetwork ok')

    # A network that is created is answered only with its id.
    created = Client('mesh/createNetwork', {})
    settle()
    ctl('mesh.networkid 1234')
    ctl('mesh.onoff 1')
    settle(1.0)
    late_onoff = onoff.all()[1:]
    late_network = refused.all()[1:]
    got_network = created.all()
    for client in (onoff, refused, created):
        client.stop()
    check(any('networkId' in r for r in got_network), 'the client that created the network got no id: %r' % got_network)
    check(not late_onoff, 'the refused setOnOff still got results: %r' % late_onoff)
    check(not late_network, 'the refused createNetwork still got the network id: %r' % late_network)


def opp_unknown_transfer_touches_no_request():
    """E12: a state change for a transfer that is not ours does not finish the first request."""
    monitor = Client('opp/monitorTransfer', {'subscribe': True})
    waiting = Client('opp/awaitTransferRequest', {'subscribe': True})
    check(monitor.wait(1) and waiting.wait(1), 'subscriptions did not answer')
    ctl('opp.request 5 %s photo.jpg 1000' % PHONE2)
    requests = waiting.wait(2)
    check(len(requests) >= 2, 'no transfer request arrived: %r' % requests)
    request_id = requests[1].get('request', {}).get('requestId')
    check(request_id, 'request without id: %r' % requests[1])
    accept = call('opp/acceptTransferRequest', {'requestId': request_id})
    check(accept and accept.get('returnValue'), 'accept failed: %r' % accept)
    settle()
    seen = len(monitor.all())
    ctl('opp.state 99 1000 1')
    settle(1.0)
    after_unknown = monitor.all()[seen:]
    ctl('opp.state 5 1000 1')
    settle(1.0)
    after_ours = monitor.all()[seen + len(after_unknown):]
    monitor.stop()
    waiting.stop()
    check(not after_unknown, 'a transfer the service does not know changed a request: %r' % after_unknown)
    check(after_ours, 'our own transfer finishing was not reported')


SCENARIOS = [
    map_session_ended_without_subscription,
    map_failed_subscribed_connect_leaves_no_watch,
    map_local_disconnect_ends_notifications,
    pbap_folder_is_per_device_and_set_on_success,
    pbap_second_subscriber_to_a_pull_is_told,
    mesh_rejected_requests_leave_no_watch,
    opp_unknown_transfer_touches_no_request,
]


def sanitizer_findings():
    """Reports from sanitizer logs that point at the service: every error, and every leak with one of its frames
    above the allocator. Leaks of luna-service2's own transport data (glib and luna-service2 frames only, the
    same each run) are known and left out; without a symbolizer on the target a suppression cannot name them."""
    findings = []
    if not os.path.isdir(SANITIZER_LOGS):
        return findings
    for name in sorted(os.listdir(SANITIZER_LOGS)):
        record = None
        for line in read_lines(os.path.join(SANITIZER_LOGS, name)):
            if 'runtime error' in line or 'ERROR: AddressSanitizer' in line:
                findings.append('%s: %s' % (name, line.strip()))
            if 'leak of' in line:
                record = [line.strip(), False]
                continue
            if record is not None:
                frame = line.strip()
                if not frame:
                    if record[1]:
                        findings.append('%s: %s' % (name, record[0]))
                    record = None
                elif frame.startswith('#') and not frame.startswith('#0 ') and any(m in frame for m in OUR_MODULES):
                    record[1] = True
    return findings


def main(argv):
    wanted = set(argv[1:])
    failures = 0
    ran = 0
    if os.path.isdir(SANITIZER_LOGS):
        for name in os.listdir(SANITIZER_LOGS):
            os.remove(os.path.join(SANITIZER_LOGS, name))
    for scenario in SCENARIOS:
        if wanted and scenario.__name__ not in wanted:
            continue
        ran += 1
        restart_service()
        pid = main_pid()
        try:
            scenario()
            check(main_pid() == pid, 'the service restarted (pid %s -> %s)' % (pid, main_pid()))
            print('PASS  %s' % scenario.__name__)
        except (Failure, RuntimeError) as e:
            failures += 1
            print('FAIL  %s: %s' % (scenario.__name__, e))
        sys.stdout.flush()
    subprocess.run(['systemctl', 'stop', SERVICE], check=False)
    print('%d of %d scenarios passed' % (ran - failures, ran))
    findings = sanitizer_findings()
    for finding in findings:
        print('SANITIZER  %s' % finding)
    return 1 if failures or findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
