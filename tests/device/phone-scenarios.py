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

# MAP and PBAP against a real phone, through the installed service and bluez5:
#
#   phone-scenarios.py <phone address> [scenario-name...]
#
# The phone must be paired, in range, and allowed to share messages and contacts (Android asks on the first
# connect). Nothing read from the phone is printed: only counts, states and paths. Pulled phone books are
# deleted again.

import json
import os
import pty
import select
import signal
import subprocess
import sys
import threading
import time

URI = 'luna://com.webos.service.bluetooth2/'
APP_ID = 'com.palm.app.settings'
SERVICE = 'webos-bluetooth-service'


class Client:
    """A luna-send on a pseudo-terminal (it flushes only to a terminal): one call, or a subscription."""

    def __init__(self, method, payload, interactive=True):
        master, slave = pty.openpty()
        args = ['luna-send', '-a', APP_ID] + (['-i'] if interactive else ['-n', '1']) + [URI + method, json.dumps(payload)]
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
                data = os.read(self.master, 65536)
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

    def wait(self, count=1, timeout=30.0, until=None):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if until is not None and any(until(r) for r in self.replies):
                    return list(self.replies)
                if until is None and len(self.replies) >= count:
                    return list(self.replies)
            time.sleep(0.1)
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


def call(method, payload, timeout=60.0):
    client = Client(method, payload, interactive=False)
    replies = client.wait(1, timeout)
    client.stop()
    return replies[0] if replies else None


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def short(reply):
    if not reply:
        return 'no reply'
    return '%s %s' % (reply.get('returnValue'), reply.get('errorText', ''))


def drop_link(phone):
    # What the service sees when the phone goes away: the link and every session on it end underneath it.
    subprocess.run(['bluetoothctl', 'disconnect', phone.upper()], capture_output=True, timeout=30)
    time.sleep(4)


def disconnect_all(phone):
    """Starts each scenario with no MAP or PBAP session left from an earlier one."""
    status = call('map/getStatus', {'address': phone})
    for session in (status or {}).get('status', []):
        if session.get('Connected') and session.get('sessionId'):
            call('map/disconnect', {'address': phone, 'sessionId': session['sessionId']})
    call('pbap/disconnect', {'address': phone})


def map_connect(phone, subscribe=False):
    reply = call('map/connect', {'address': phone, 'subscribe': subscribe} if subscribe else {'address': phone})
    check(reply and reply.get('returnValue'), 'map connect: %s' % short(reply))
    return reply['sessionId']


# ---------------------------------------------------------------- scenarios

def map_lists_folders_and_messages(phone):
    session = map_connect(phone)
    folders = call('map/getFolderList', {'address': phone, 'sessionId': session})
    check(folders and folders.get('returnValue'), 'getFolderList: %s' % short(folders))
    messages = call('map/getMessageList', {'address': phone, 'sessionId': session, 'folder': 'inbox',
                                            'filter': {'maxCount': 3, 'subjectLength': 1}})
    check(messages and messages.get('returnValue'), 'getMessageList: %s' % short(messages))
    print('      %d folder(s), %d message(s) listed' % (len(folders.get('folders', [])), len(messages.get('messages', []))))
    reply = call('map/disconnect', {'address': phone, 'sessionId': session})
    check(reply and reply.get('returnValue'), 'map disconnect: %s' % short(reply))


def map_session_the_phone_ends(phone):
    """D6: connected without a subscription, the session ends underneath the service; a new connect works."""
    map_connect(phone)
    drop_link(phone)
    status = call('map/getStatus', {'address': phone})
    connected = [s for s in (status or {}).get('status', []) if s.get('Connected')]
    check(not connected, 'still reported connected after the link dropped: %r' % status)
    session = map_connect(phone)
    call('map/disconnect', {'address': phone, 'sessionId': session})


def map_own_disconnect_ends_notifications(phone):
    """D12: the client's own disconnect ends the session's message notification subscription."""
    session = map_connect(phone)
    notifications = Client('map/getMessageNotification', {'address': phone, 'sessionId': session, 'subscribe': True})
    first = notifications.wait(1)
    check(first and first[0].get('returnValue'), 'getMessageNotification: %s' % short(first[0] if first else None))
    reply = call('map/disconnect', {'address': phone, 'sessionId': session})
    check(reply and reply.get('returnValue'), 'map disconnect: %s' % short(reply))
    replies = notifications.wait(until=lambda r: r.get('subscribed') is False, timeout=10)
    notifications.stop()
    check(any(r.get('subscribed') is False for r in replies[1:]), 'the notification subscriber was not told the session ended')


def map_subscriber_leaving_disconnects(phone):
    """A subscribed connect's client going away disconnects the session it opened."""
    client = Client('map/connect', {'address': phone, 'subscribe': True})
    first = client.wait(1)
    check(first and first[0].get('returnValue'), 'subscribed connect: %s' % short(first[0] if first else None))
    client.stop()
    time.sleep(4)
    status = call('map/getStatus', {'address': phone})
    connected = [s for s in (status or {}).get('status', []) if s.get('Connected')]
    check(not connected, 'session stayed connected after its subscriber went away: %r' % status)


def pbap_folder_set_on_success(phone):
    """D9: a folder change the phone refuses does not move later pulls; a pull lands in the folder set."""
    reply = call('pbap/connect', {'address': phone})
    check(reply and (reply.get('returnValue') or reply.get('errorCode') == 136 or 'already' in reply.get('errorText', '')),
          'pbap connect: %s' % short(reply))
    ok = call('pbap/setPhoneBook', {'address': phone, 'repository': 'internal', 'object': 'pb'})
    check(ok and ok.get('returnValue'), 'setPhoneBook internal/pb: %s' % short(ok))
    size = call('pbap/getSize', {'address': phone})
    check(size and size.get('returnValue'), 'getSize: %s' % short(size))
    refused = call('pbap/setPhoneBook', {'address': phone, 'repository': 'internal', 'object': 'nosuchbook'})
    print('      phone book size %s; change to an unknown folder: %s' % (size.get('size'), short(refused)))

    pull = Client('pbap/pullPhoneBook', {'address': phone, 'destinationFile': 'bt2-phone-test.vcf', 'maxListCount': 2,
                                         'subscribe': True})
    replies = pull.wait(until=lambda r: r.get('status') in ('completed', 'error') or r.get('returnValue') is False, timeout=60)
    pull.stop()
    check(replies and replies[0].get('returnValue'), 'pullPhoneBook: %s' % short(replies[0] if replies else None))
    destination = replies[0].get('destinationFile', '')
    states = [r.get('status') for r in replies if r.get('status')]
    try:
        check('completed' in states, 'the pull did not complete: %r' % states)
        folder = phone.replace(':', '_') + '/internal/'
        expected = folder + ('nosuchbook/' if refused and refused.get('returnValue') else 'pb/')
        check(expected in destination, 'pulled to %s, expected the folder %s' % (destination, expected))
        print('      pulled to %s (%d bytes)' % (destination, os.path.getsize(destination) if os.path.exists(destination) else -1))
    finally:
        if destination and os.path.exists(destination):
            os.remove(destination)
    call('pbap/disconnect', {'address': phone})


SCENARIOS = [
    map_lists_folders_and_messages,
    map_session_the_phone_ends,
    map_own_disconnect_ends_notifications,
    map_subscriber_leaving_disconnects,
    pbap_folder_set_on_success,
]


def main_pid():
    return subprocess.run(['systemctl', 'show', '-p', 'MainPID', '--value', SERVICE], capture_output=True, text=True).stdout.strip()


def main(argv):
    if len(argv) < 2:
        print('usage: %s <phone address> [scenario...]' % argv[0])
        return 2
    phone = argv[1].lower()
    wanted = set(argv[2:])
    pid = main_pid()
    failures = ran = 0
    for scenario in SCENARIOS:
        if wanted and scenario.__name__ not in wanted:
            continue
        ran += 1
        try:
            disconnect_all(phone)
            scenario(phone)
            check(main_pid() == pid, 'the service restarted')
            print('PASS  %s' % scenario.__name__)
        except Failure as e:
            failures += 1
            print('FAIL  %s: %s' % (scenario.__name__, e))
        sys.stdout.flush()
    print('%d of %d scenarios passed' % (ran - failures, ran))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
