#!/usr/bin/env python

import sys
import os
import subprocess
import shutil

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

gdb = False

for argument in sys.argv[1:]:
    if argument == 'gdb':
        gdb = True

parent_directory = os.path.dirname(os.path.realpath(__file__))

build_directory = os.path.join(parent_directory, 'build')

run_command(
    shutil.which('qemu-system-x86_64'),
    *(['-S', '-s'] if gdb else []),
    '-monitor', 'stdio',
    '-machine', 'q35',
    '-m', '4G',
    "-vga", 'virtio',
    '-kernel', os.path.join(build_directory, 'kernel32.elf')
)