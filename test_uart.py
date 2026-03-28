import serial, time

s = serial.Serial('/dev/ttyS7', 115200, timeout=3, dsrdtr=False, rtscts=False)
print("Port offen. RTS-Reset...")
s.dtr = True
s.rts = True
time.sleep(0.5)
s.rts = False   # EN freigeben -> Board bootet
time.sleep(0.2)
data = s.read(200)
print(f"Bytes empfangen: {len(data)}")
print(f"Inhalt: {repr(data[:80])}")
s.close()
