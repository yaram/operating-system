#!/usr/bin/env python

import sys
import os
import subprocess
import shutil

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

parent_directory = os.path.dirname(os.path.realpath(__file__))

build_directory = os.path.join(parent_directory, 'build')

cpu_count = os.cpu_count()

if cpu_count == None:
    cpu_count = 2

run_command(
    shutil.which('qemu-system-x86_64'),
    '-monitor', 'stdio',
    '-machine', 'q35',
    '-smp', str(cpu_count),
    '-m', '4G',
    '-vga', 'virtio',
    '-device', 'virtio-keyboard',
    '-device', 'virtio-mouse',
    '-kernel', os.path.join(build_directory, 'kernel32.elf'),
    *sys.argv[1:]
)