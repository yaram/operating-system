#!/usr/bin/env python

import sys
import os
import subprocess
import shutil
import signal

def escape_handler(signum, frame):
    exit(1)

signal.signal(signal.SIGINT, escape_handler)

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

parent_directory = os.path.dirname(os.path.realpath(__file__))

build_directory = os.path.join(parent_directory, 'build')

cpu_count = os.cpu_count()

if cpu_count == None:
    cpu_count = 2

qemu_path = shutil.which('qemu-system-x86_64')
qemu_directory = os.path.dirname(qemu_path)

possible_uefi_firmware_paths = [
    os.path.join(qemu_directory, 'edk2-x86_64-code.fd'),
    '/usr/share/edk2/ovmf/OVMF_CODE.fd'
]

uefi_firmware_path = None
for path in possible_uefi_firmware_paths:
    if os.path.exists(path):
        uefi_firmware_path = path
        break

if uefi_firmware_path == None:
    print("Could not find OVMF UEFI firmware", file=sys.stderr)
    exit(1)

run_command(
    qemu_path,
    '-no-reboot',
    '-no-shutdown',
    '-monitor', 'stdio',
    '-machine', 'q35',
    '-smp', '{}'.format(cpu_count),
    '-m', '4G',
    '-vga', 'virtio',
    '-device', 'virtio-keyboard',
    '-device', 'virtio-mouse',
    '-drive', 'if=pflash,format=raw,unit=0,readonly,file={}'.format(uefi_firmware_path),
    '-kernel', os.path.join(build_directory, 'BOOTX64.EFI'),
    *sys.argv[1:]
)