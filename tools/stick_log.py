#!/usr/bin/env python3
"""Print the diagnostics a Stick keeps about recent resets and its last connection.

Usage: tools/stick_log.py /dev/cu.usbmodem1101   (quit Stickwalker Desktop first)
"""
import os
import struct
import sys
import termios
import time
import zlib

REASONS = {4: 'crash', 5: 'interrupt watchdog', 6: 'task watchdog', 7: 'other watchdog', 9: 'brownout'}
ACTIVITIES = {0: 'unknown', 1: 'starting up', 2: 'playing', 3: 'connecting over IR', 4: 'saving',
              5: 'USB transfer', 6: 'asleep'}
RESULTS = ['success', 'no response', 'connection error', 'rejected', 'no Pokémon', 'peer already seen',
           'event already received', 'event not received', 'receive overflow']


def frame(op, sequence, payload=b''):
    raw = b'SW\x01' + bytes([op]) + struct.pack('<HH', sequence, len(payload)) + payload
    raw += struct.pack('<I', zlib.crc32(raw))
    return b'\xc0' + raw.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc') + b'\xc0'


def status(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attributes = termios.tcgetattr(fd)
    attributes[0] = attributes[1] = attributes[3] = 0
    attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attributes)
    os.write(fd, frame(1, 1))
    data, deadline = b'', time.time() + 3
    while time.time() < deadline:
        try:
            data += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.02)
        for chunk in data.split(b'\xc0'):
            raw = chunk.replace(b'\xdb\xdc', b'\xc0').replace(b'\xdb\xdd', b'\xdb')
            if len(raw) >= 12 and raw[:3] == b'SW\x01' and zlib.crc32(raw[:-4]) == struct.unpack('<I', raw[-4:])[0]:
                os.close(fd)
                return raw[9:-4]
    os.close(fd)
    sys.exit('The Stick did not answer. Is Stickwalker Desktop holding the port?')


reply = status(sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem1101')
print('Firmware', reply[:32].rstrip(b'\0').decode())
extra = reply[225:]
if len(extra) < 42 or extra[0] != 1:
    sys.exit('This firmware keeps no diagnostics.')
count = extra[1]
print(f'Abnormal resets since diagnostics began: {count}')
for i in range(min(count, 4)):
    reason, activity, _, uptime = struct.unpack_from('<BBHI', extra, 2 + i * 8)
    print(f'  {REASONS.get(reason, reason)} while {ACTIVITIES.get(activity, activity)}, {uptime} s after boot')
result, phase, command, _, packets, damaged = struct.unpack_from('<BBBBHH', extra, 34)
if not any(extra[34:42]):
    sys.exit('No connection recorded yet.')
print(f'Last connection: {RESULTS[result] if result < len(RESULTS) else result}, handshake phase {phase}, '
      f'{packets} packets, {damaged} damaged, last command 0x{command:02x}')
