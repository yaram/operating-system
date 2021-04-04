#!/usr/bin/env python

import sys
import os
import subprocess
import shutil

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

parent_directory = os.path.dirname(os.path.realpath(__file__))

build_directory = os.path.join(parent_directory, 'build')

run_command(
    shutil.which('qemu-system-x86_64'),
    '-monitor', 'stdio',
    '-machine', 'q35',
    '-smp', '4',
    '-m', '4G',
    '-vga', 'virtio',
    '-device', 'virtio-keyboard',
    '-device', 'virtio-mouse',
    '-kernel', os.path.join(build_directory, 'kernel32.elf'),
    *sys.argv[1:]
)