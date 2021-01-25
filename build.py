#!/usr/bin/env python

import sys
import os
import subprocess
import shutil
import math

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

parent_directory = os.path.dirname(os.path.realpath(__file__))

source_directory = os.path.join(parent_directory, 'src')
build_directory = os.path.join(parent_directory, 'build')

if not os.path.exists(build_directory):
    os.makedirs(build_directory)

configuration = 'debug'

if len(sys.argv) >= 2:
    configuration = sys.argv[1]

    if configuration != 'debug' and configuration != 'release':
        print('Unknown configuration \'{}\''.format(configuration), file=sys.stderr)
        exit(1)

objects_64bit = [
    ('entry64.S', 'entry64.o'),
    ('main.cpp', 'main.o'),
]

for source_name, object_name in objects_64bit:
    run_command(
        shutil.which('clang++'),
        '-target', 'x86_64-unknown-unknown-elf',
        '-march=x86-64',
        '-mcmodel=kernel',
        '-ffreestanding',
        '-fno-stack-protector',
        '-mno-red-zone',
        '-mno-mmx',
        '-mno-sse',
        '-mno-sse2',
        '-fpic',
        *(['-g'] if configuration == 'debug' else []),
        *(['-O2'] if configuration == 'release' else []),
        '-c',
        '-o', os.path.join(build_directory, object_name),
        os.path.join(source_directory, source_name)
    )

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '--pie',
    '-T', os.path.join(source_directory, 'linker64.ld'),
    '-o', os.path.join(build_directory, 'kernel64.elf'),
    *[os.path.join(build_directory, object_name) for _, object_name in objects_64bit]
)

run_command(
    shutil.which('llvm-objcopy'),
    '--output-target=binary',
    '--only-section', '.text',
    '--only-section', '.data',
    '--only-section', '.rodata',
    '--only-section', '.kernel64',
    os.path.join(build_directory, 'kernel64.elf'),
    os.path.join(build_directory, 'kernel64.bin')
)

objects_32bit = [
    ('entry.S', 'entry.o'),
    ('multiboot.S', 'multiboot.o'),
]

for source_name, object_name in objects_32bit:
    run_command(
        shutil.which('clang++'),
        '-target', 'i686-unknown-unknown-elf',
        '-march=x86-64',
        '-mcmodel=kernel',
        '-ffreestanding',
        '-fno-stack-protector',
        '-mno-red-zone',
        '-mno-mmx',
        '-mno-sse',
        '-mno-sse2',
        *(['-g'] if configuration == 'debug' else []),
        *(['-O2'] if configuration == 'release' else []),
        '-c',
        '-o', os.path.join(build_directory, object_name),
        os.path.join(source_directory, source_name)
    )

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'linker32.ld'),
    '-o', os.path.join(build_directory, 'kernel32.elf'),
    *[os.path.join(build_directory, object_name) for _, object_name in objects_32bit]
)