import sys, time, frida
TARGET = "android.hardware.gatekeeper@1.0-service-qti"
dev = frida.get_usb_device(timeout=10)
s = dev.attach(TARGET)
sc = s.create_script(open("android-gkhook.js").read())
sc.on("message", lambda m, d: print(m.get("payload", m), flush=True))
sc.load()
print("=== attached to %s ===" % TARGET, flush=True)
time.sleep(int(sys.argv[1]) if len(sys.argv) > 1 else 60)
