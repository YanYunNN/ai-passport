import serial, sys, time

DEV = '/dev/cu.usbmodem1301'

def open_port():
    for _ in range(60):
        try:
            return serial.Serial(DEV, 115200, timeout=0.2)
        except Exception:
            time.sleep(0.5)
    return None

port = open_port()
if not port:
    sys.stdout.write('### could not open %s\n' % DEV)
    sys.stdout.flush()
    raise SystemExit(1)
port.dtr = False
port.rts = True
time.sleep(0.05)
port.rts = False
port.dtr = True
sys.stdout.write('--- serial console attached %s @115200 ---\n' % DEV)
sys.stdout.flush()
try:
    while True:
        try:
            b = port.read(4096)
            if b:
                sys.stdout.write(b.decode('utf-8', 'replace'))
                sys.stdout.flush()
            else:
                time.sleep(0.05)
        except Exception as e:
            sys.stdout.write('\n--- (detached) %s; reconnecting ---\n' % e)
            sys.stdout.flush()
            try:
                port.close()
            except Exception:
                pass
            time.sleep(1.0)
            port = None
            for _ in range(60):
                try:
                    port = serial.Serial(DEV, 115200, timeout=0.2)
                    break
                except Exception:
                    time.sleep(0.5)
            if port is None:
                sys.stdout.write('\n??? port gone; stopping\n')
                sys.stdout.flush()
                break
            sys.stdout.write('--- reconnected ---\n')
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    try:
        port.close()
    except Exception:
        pass