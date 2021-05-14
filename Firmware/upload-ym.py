#!/usr/bin/python3
import serial
import sys
import subprocess
import signal
import traceback
import os
import argparse
import serial


def signal_term_handler(signal, frame):
    global intrflg
    print('got SIGTERM...')
    sys.quit()


signal.signal(signal.SIGTERM, signal_term_handler)

# Define command line argument interface
parser = argparse.ArgumentParser(description='ymodem upload file to smoothie')
parser.add_argument('file', help='filename to be uploaded')
parser.add_argument('device', help='Smoothie Serial Device')
parser.add_argument('-v', '--verbose', action='store_true', default=False, help='verbose output')
parser.add_argument('-f', '--flash', action='store_true', default=False, help='flash')
parser.add_argument('-1', '--notonek', action='store_true', default=False, help='256 blocks')
args = parser.parse_args()

file_path = args.file
dev = "/dev/tty{}".format(args.device)

print("Uploading file: {} to {}".format(file_path, dev))

ser = serial.Serial(dev, 115200, timeout=0.1)
ser.flushInput()  # Flush startup text in serial input

gotok = False
while not gotok:
    ser.write(b'\n')

    rep = ser.read_until()
    s = rep.decode(encoding='latin1', errors='ignore')
    gotok = s.startswith('ok')

ser.flushInput()
ser.write(b'ry -q\n')
ser.close()

fin = open(dev, "rb", buffering=0)
fout = open(dev, "wb", buffering=0)

if args.verbose:
    varg = '-vvv'
else:
    varg = '-q'

ok = False

try:
    if not args.notonek:
        p = subprocess.Popen(['sx', '--ymodem', '-k', varg, file_path], bufsize=0, stdin=fin, stdout=fout, stderr=sys.stderr)
    else:
        p = subprocess.Popen(['sx', '--ymodem', varg, file_path], bufsize=0, stdin=fin, stdout=fout, stderr=sys.stderr)

    result, err = p.communicate()
    if p.returncode != 0:
        print("Failed: {}".format(err))
        ok = False
    else:
        print("uploaded ok")
        ok = True

except:
    print('Exception: {}'.format(traceback.format_exc()))

fin.close()
fout.close()

if ok and args.flash:
    # we use serial here
    s = serial.Serial(dev, 115200)
    s.flushInput()  # Flush startup text in serial input

    s.write(b'rm flashme.bin\n')
    s.flushInput()
    s.write(b'mv smoothiev2.bin flashme.bin\n')
    s.flushInput()
    s.write(b'flash\n')
    s.flushInput()
    s.close()
