#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check active Ninja objects agree on the derived multiplayer class declarations."""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1])
def ninja(*args):
    return subprocess.run(['ninja', '-C', str(build), '-t', *args],
                          capture_output=True, text=True, check=True).stdout

active = {line.split(': ', 1)[0] for line in ninja('targets', 'all').splitlines()}
headers = ('network/packet.h', 'network/room_member.h',
           'core/internal_network/sockets.h', 'core/internal_network/socket_proxy.h',
           'core/hle/service/sockets/bsd.h')
# Cores that harden room sockets keep their own socket_proxy.h and bsd.h (multiplayer_packets.cmake).
core_owned = ('core/internal_network/socket_proxy.h', 'core/hle/service/sockets/bsd.h')
def hardened_core(path, header):
    if header not in core_owned or not path.endswith('/src/' + header):
        return False
    proxy = pathlib.Path(path[:-len(header)], 'core/internal_network/socket_proxy.h')
    return proxy.is_file() and 'AcceptsProxyPacket' in proxy.read_text(errors='ignore')
seen = dict.fromkeys(headers, 0)
wrong, target = [], None
for line in ninja('deps').splitlines():
    if line and not line.startswith(' '):
        target = line.split(': ', 1)[0]
        continue
    if target not in active:
        continue
    path = line.strip().replace('\\', '/')
    for header in headers:
        if path.endswith('/' + header):
            prefix = '/headless/multiplayer/' + ('socket-headers/' if header.startswith('core/') else '')
            if path.endswith(prefix + header) or hardened_core(path, header):
                seen[header] += 1
            else:
                wrong.append(f'{target}: {path}')
missing = [header for header, count in seen.items() if not count]
if wrong or missing:
    print('\n'.join(wrong[:20]))
    sys.exit(f'Multiplayer headers: {len(wrong)} original-header dependencies, missing {missing}')
print(f'Multiplayer derived headers: {seen} PASS')
