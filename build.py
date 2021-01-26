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
thirdparty_directory = os.path.join(parent_directory, 'thirdparty')

object_directory = os.path.join(build_directory, 'objects')

acpica_directory = os.path.join(thirdparty_directory, 'acpica')

if not os.path.exists(object_directory):
    os.makedirs(object_directory)

configuration = 'debug'

if len(sys.argv) >= 2:
    configuration = sys.argv[1]

    if configuration != 'debug' and configuration != 'release':
        print('Unknown configuration \'{}\''.format(configuration), file=sys.stderr)
        exit(1)

acpica_archive = os.path.join(build_directory, 'acpica.a')

if not os.path.exists(acpica_archive):
    objects = [
        (os.path.join(acpica_directory, 'src', name), name[:name.index('.c')] + '.o')
        for name in os.listdir(os.path.join(acpica_directory, 'src'))
    ]

    for source_path, object_name in objects:
        run_command(
            shutil.which('clang'),
            '-target', 'x86_64-unknown-unknown-elf',
            '-I{}'.format(os.path.join(acpica_directory, 'include')),
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
            '-o', os.path.join(object_directory, object_name),
            source_path
        )

    run_command(
        shutil.which('llvm-ar'),
        '-rs',
        acpica_archive,
        *[os.path.join(object_directory, object_name) for _, object_name in objects]
    )


objects_64bit = [
    (os.path.join(source_directory, 'entry64.S'), 'entry64.o'),
    (os.path.join(source_directory, 'main.cpp'), 'main.o'),
    (os.path.join(source_directory, 'acpi_environment.cpp'), 'acpi_environment.o')
]

for source_path, object_name in objects_64bit:
    run_command(
        shutil.which('clang++'),
        '-target', 'x86_64-unknown-unknown-elf',
        '-I{}'.format(os.path.join(acpica_directory, 'include')),
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
        '-o', os.path.join(object_directory, object_name),
        source_path
    )

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'linker64.ld'),
    '-o', os.path.join(build_directory, 'kernel64.elf'),
    acpica_archive,
    *[os.path.join(object_directory, object_name) for _, object_name in objects_64bit]
)

run_command(
    shutil.which('llvm-objcopy'),
    '--output-target=binary',
    '--only-section', '.text',
    '--only-section', '.data',
    '--only-section', '.rodata',
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
        '-o', os.path.join(object_directory, object_name),
        os.path.join(source_directory, source_name)
    )

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'linker32.ld'),
    '-o', os.path.join(build_directory, 'kernel32.elf'),
    *[os.path.join(object_directory, object_name) for _, object_name in objects_32bit]
)