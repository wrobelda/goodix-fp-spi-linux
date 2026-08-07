"""Attach to both channels at once and log each to its own file.

The command channel (client -> trusted app) and the file-service listener
channel (trusted app -> normal world) live in different processes, so a single
attach only ever sees half the conversation. This drives both, which is what
makes a capture of a working SAVE useful: the commands and the file operations
they cause, in one session, timestamped so they can be interleaved afterwards.

Usage:  python3 android-capture-both.py <seconds> <outdir>
"""
import os
import sys
import time

import frida

SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 600
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else '.'

TARGETS = [
    ('qseecomd', 'android-listener-hook.js', 'listener.log'),
    ('android.hardware.biometrics.fingerprint@2.1-service',
     'android-gfhook.js', 'command.log'),
]

here = os.path.dirname(os.path.abspath(__file__))
dev = frida.get_usb_device(timeout=10)
start = time.time()
sessions = []

for proc, js, logname in TARGETS:
    path = os.path.join(OUTDIR, logname)
    fh = open(path, 'w', buffering=1)

    def make_handler(fh=fh):
        def on_message(message, data):
            payload = message.get('payload', message)
            if isinstance(payload, dict):
                payload = payload.get('msg', payload)
            fh.write('[%8.3f] %s\n' % (time.time() - start, payload))
        return on_message

    try:
        session = dev.attach(proc)
        script = session.create_script(open(os.path.join(here, js)).read())
        script.on('message', make_handler())
        script.load()
        sessions.append(session)
        print('attached %s -> %s' % (proc, path), flush=True)
    except Exception as exc:                # noqa: BLE001
        print('FAILED %s: %s' % (proc, exc), flush=True)

if not sessions:
    sys.exit('nothing attached')

print('capturing for %ds' % SECONDS, flush=True)
time.sleep(SECONDS)
print('done', flush=True)
