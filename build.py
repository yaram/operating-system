#!/usr/bin/env python

import sys
import os
import subprocess
import shutil

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

configuration = 'debug'

if len(sys.argv) >= 2:
    configuration = sys.argv[1]

optimize = None
debug_info = None

if configuration.lower() == 'debug':
    optimize = False
    debug_info = True
elif configuration.lower() == 'release':
    optimize = True
    debug_info = False
elif configuration.lower() == "relwithdebinfo":
    optimize = True
    debug_info = True
else:
    print('Unknown configuration \'{}\''.format(configuration), file=sys.stderr)
    exit(1)

parent_directory = os.path.dirname(os.path.realpath(__file__))

source_directory = os.path.join(parent_directory, 'src')
build_directory = os.path.join(parent_directory, 'build')
thirdparty_directory = os.path.join(parent_directory, 'thirdparty')

object_directory = os.path.join(build_directory, 'objects')

def build_objects(objects, target, object_subdirectory, *extra_arguments):
    object_subdirectory_full = os.path.join(object_directory, object_subdirectory)

    if not os.path.exists(object_subdirectory_full):
        os.makedirs(object_subdirectory_full)

    for source_path, object_name in objects:
        is_cpp = source_path.endswith('.cpp')

        run_command(
            shutil.which('clang++' if is_cpp else 'clang'),
            '-target', target,
            '-march=x86-64',
            '-std=gnu++11' if is_cpp else '-std=gnu11',
            '-ffreestanding',
            *(['-g'] if debug_info else []),
            *(['-O2'] if optimize else []),
            *extra_arguments,
            '-c',
            '-o', os.path.join(object_directory, object_subdirectory_full, object_name),
            source_path
        )

def build_objects_64bit(objects, object_subdirectory, *extra_arguments):
    build_objects(objects, 'x86_64-unknown-unknown-elf', object_subdirectory, *extra_arguments)

def build_objects_32bit(objects, object_subdirectory, *extra_arguments):
    build_objects(objects, 'i686-unknown-unknown-elf', object_subdirectory, *extra_arguments)

acpica_directory = os.path.join(thirdparty_directory, 'acpica')
printf_directory = os.path.join(thirdparty_directory, 'printf')
openlibm_directory = os.path.join(thirdparty_directory, 'openlibm')

acpica_archive = os.path.join(build_directory, 'acpica.a')

if not os.path.exists(acpica_archive):
    objects = [
        (os.path.join(acpica_directory, 'src', name), name[:name.index('.c')] + '.o')
        for name in os.listdir(os.path.join(acpica_directory, 'src'))
    ]

    build_objects_64bit(
        objects,
        'acpica',
        '-I{}'.format(os.path.join(acpica_directory, 'include')),
        '-mcmodel=kernel',
        '-fno-stack-protector',
        '-mno-red-zone',
        '-mno-mmx',
        '-mno-sse',
        '-mno-sse2'
    )

    run_command(
        shutil.which('llvm-ar'),
        '-rs',
        acpica_archive,
        *[os.path.join(object_directory, 'acpica', object_name) for _, object_name in objects]
    )

user_openlibm_archive = os.path.join(build_directory, 'user_openlibm.a')

if not os.path.exists(user_openlibm_archive):
    objects = [
        (os.path.join(openlibm_directory, 'src', name), name[:name.index('.c')] + '.o')
        for name in filter(lambda name: name.endswith('.c'), os.listdir(os.path.join(openlibm_directory, 'src')))
    ]

    build_objects_64bit(
        objects,
        'user_openlibm',
        '-I{}'.format(os.path.join(source_directory, 'init')),
        '-I{}'.format(os.path.join(openlibm_directory, 'src')),
        '-I{}'.format(os.path.join(openlibm_directory, 'include')),
        '-I{}'.format(os.path.join(openlibm_directory, 'amd64')),
        '-I{}'.format(os.path.join(openlibm_directory, 'ld80')),
        '-D__BSD_VISIBLE=1',
        '-fpie'
    )

    run_command(
        shutil.which('llvm-ar'),
        '-rs',
        user_openlibm_archive,
        *[os.path.join(object_directory, 'user_openlibm', object_name) for _, object_name in objects]
    )

init_objects = [
    (os.path.join(source_directory, 'init', 'main.cpp'), 'main.o'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o'),
]

build_objects_64bit(
    init_objects,
    'init',
    '-I{}'.format(os.path.join(source_directory, 'init')),
    '-I{}'.format(os.path.join(source_directory, 'shared')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-I{}'.format(os.path.join(openlibm_directory, 'include')),
    '-I{}'.format(os.path.join(thirdparty_directory)),
    '-D__BSD_VISIBLE=1',
    '-fpie'
)

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-pie',
    '-o', os.path.join(build_directory, 'init.elf'),
    user_openlibm_archive,
    *[os.path.join(object_directory, 'init', object_name) for _, object_name in init_objects]
)

objects_kernel64 = [
    (os.path.join(source_directory, 'kernel64', 'static_init.S'), 'static_init.o'),
    (os.path.join(source_directory, 'kernel64', 'entry.S'), 'entry.o'),
    (os.path.join(source_directory, 'kernel64', 'syscall.S'), 'syscall.o'),
    (os.path.join(source_directory, 'kernel64', 'preempt.S'), 'preempt.o'),
    (os.path.join(source_directory, 'kernel64', 'interrupts.S'), 'interrupts.o'),
    (os.path.join(source_directory, 'kernel64', 'main.cpp'), 'main.o'),
    (os.path.join(source_directory, 'kernel64', 'console.cpp'), 'console.o'),
    (os.path.join(source_directory, 'kernel64', 'paging.cpp'), 'paging.o'),
    (os.path.join(source_directory, 'kernel64', 'acpi_environment.cpp'), 'acpi_environment.o'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o')
]

build_objects_64bit(
    objects_kernel64,
    'kernel64',
    '-I{}'.format(os.path.join(source_directory, 'shared')),
    '-I{}'.format(os.path.join(acpica_directory, 'include')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-mcmodel=kernel',
    '-fno-stack-protector',
    '-mno-red-zone',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2'
)

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'kernel64', 'linker.ld'),
    '-o', os.path.join(build_directory, 'kernel64.elf'),
    acpica_archive,
    *[os.path.join(object_directory, 'kernel64', object_name) for _, object_name in objects_kernel64]
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

objects_kernel32 = [
    (os.path.join(source_directory, 'kernel32', 'entry.S'), 'entry.o'),
    (os.path.join(source_directory, 'kernel32', 'multiboot.S'), 'multiboot.o'),
]

build_objects_32bit(
    objects_kernel32,
    'kernel32',
    '-mcmodel=kernel',
    '-fno-stack-protector',
    '-mno-red-zone',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2'
)

run_command(
    shutil.which('ld.lld'),
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'kernel32', 'linker.ld'),
    '-o', os.path.join(build_directory, 'kernel32.elf'),
    *[os.path.join(object_directory, 'kernel32', object_name) for _, object_name in objects_kernel32]
)