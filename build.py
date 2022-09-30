#!/usr/bin/env python

import sys
import os
import time
import subprocess
import concurrent.futures
import shutil
import argparse
import signal

start_time = time.time()

def escape_handler(signum, frame):
    exit(1)

signal.signal(signal.SIGINT, escape_handler)

def run_command(executable, *arguments):
    subprocess.call([executable, *arguments], stdout=sys.stdout, stderr=sys.stderr)

parser = argparse.ArgumentParser(description='Build Operating System')

cpu_count = os.cpu_count() or 1

parser.add_argument('--optimize', action='store_true', help='produce optimized output')
parser.add_argument('--nodebug', dest="debug", action='store_false', help='don\'t produce debug info')
parser.add_argument('--jobs', action='store', default=cpu_count, type=int, help='number of parallel compiler jobs (default: {})'.format(cpu_count))
parser.add_argument('--rebuild', action='store_true', help='rebuild all libraries')

arguments = parser.parse_args()

c_compiler_path = shutil.which('clang')
cpp_compiler_path = shutil.which('clang++')
linker_path = shutil.which('ld.lld')

parent_directory = os.path.dirname(os.path.realpath(__file__))

source_directory = os.path.join(parent_directory, 'src')
build_directory = os.path.join(parent_directory, 'build')
thirdparty_directory = os.path.join(parent_directory, 'thirdparty')

object_directory = os.path.join(build_directory, 'objects')

def build_objects(objects, target, name, *extra_arguments):
    sub_object_directory = os.path.join(object_directory, name)

    if not os.path.exists(sub_object_directory):
        os.makedirs(sub_object_directory)

    if arguments.debug:
        extra_arguments = (*extra_arguments, '-g')

    if arguments.optimize:
        extra_arguments = (*extra_arguments, '-O2', '-DOPTIMIZED')

    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as thread_pool:
        for source_path, object_name in objects:
            is_cpp = source_path.endswith('.cpp')

            thread_pool.submit(
                run_command,
                cpp_compiler_path if is_cpp else c_compiler_path,
                '-target', target,
                '-march=x86-64',
                '-std=gnu++11' if is_cpp else '-std=gnu11',
                '-ffreestanding',
                '-Wall',
                '-fshort-wchar',
                *extra_arguments,
                '-c',
                '-o', os.path.join(sub_object_directory, object_name),
                source_path
            )

def build_objects_64bit(objects, name, *extra_arguments):
    build_objects(objects, 'x86_64-unknown-unknown-elf', name, *extra_arguments)

def build_objects_16bit(objects, name, *extra_arguments):
    build_objects(objects, 'i686-unknown-unknown-code16', name, *extra_arguments)

def do_linking(objects, name, *extra_arguments):
    run_command(
        linker_path,
        *extra_arguments,
        '-o', os.path.join(build_directory, '{}.elf'.format(name)),
        *[os.path.join(object_directory, name, object_name) for _, object_name in objects]
    )

user_source_directory = os.path.join(source_directory, 'user')

acpica_directory = os.path.join(thirdparty_directory, 'acpica')
printf_directory = os.path.join(thirdparty_directory, 'printf')
openlibm_directory = os.path.join(thirdparty_directory, 'openlibm')
edk2_directory = os.path.join(thirdparty_directory, 'edk2')

acpica_archive = os.path.join(build_directory, 'acpica.a')

if arguments.rebuild or not os.path.exists(acpica_archive):
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

if arguments.rebuild or not os.path.exists(user_openlibm_archive):
    objects = [
        (os.path.join(openlibm_directory, 'src', 'common.c'), 'common.o'),
        (os.path.join(openlibm_directory, 'src', 'e_acos.c'), 'e_acos.o'),
        (os.path.join(openlibm_directory, 'src', 'e_acosf.c'), 'e_acosf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_acosh.c'), 'e_acosh.o'),
        (os.path.join(openlibm_directory, 'src', 'e_acoshf.c'), 'e_acoshf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_asin.c'), 'e_asin.o'),
        (os.path.join(openlibm_directory, 'src', 'e_asinf.c'), 'e_asinf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_atan2.c'), 'e_atan2.o'),
        (os.path.join(openlibm_directory, 'src', 'e_atan2f.c'), 'e_atan2f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_atanh.c'), 'e_atanh.o'),
        (os.path.join(openlibm_directory, 'src', 'e_atanhf.c'), 'e_atanhf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_cosh.c'), 'e_cosh.o'),
        (os.path.join(openlibm_directory, 'src', 'e_coshf.c'), 'e_coshf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_exp.c'), 'e_exp.o'),
        (os.path.join(openlibm_directory, 'src', 'e_expf.c'), 'e_expf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_fmod.c'), 'e_fmod.o'),
        (os.path.join(openlibm_directory, 'src', 'e_fmodf.c'), 'e_fmodf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_hypot.c'), 'e_hypot.o'),
        (os.path.join(openlibm_directory, 'src', 'e_hypotf.c'), 'e_hypotf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_j0.c'), 'e_j0.o'),
        (os.path.join(openlibm_directory, 'src', 'e_j0f.c'), 'e_j0f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_j1.c'), 'e_j1.o'),
        (os.path.join(openlibm_directory, 'src', 'e_j1f.c'), 'e_j1f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_jn.c'), 'e_jn.o'),
        (os.path.join(openlibm_directory, 'src', 'e_jnf.c'), 'e_jnf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_lgamma.c'), 'e_lgamma.o'),
        (os.path.join(openlibm_directory, 'src', 'e_lgamma_r.c'), 'e_lgamma_r.o'),
        (os.path.join(openlibm_directory, 'src', 'e_lgammaf.c'), 'e_lgammaf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_lgammaf_r.c'), 'e_lgammaf_r.o'),
        (os.path.join(openlibm_directory, 'src', 'e_log.c'), 'e_log.o'),
        (os.path.join(openlibm_directory, 'src', 'e_log10.c'), 'e_log10.o'),
        (os.path.join(openlibm_directory, 'src', 'e_log10f.c'), 'e_log10f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_log2.c'), 'e_log2.o'),
        (os.path.join(openlibm_directory, 'src', 'e_log2f.c'), 'e_log2f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_logf.c'), 'e_logf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_pow.c'), 'e_pow.o'),
        (os.path.join(openlibm_directory, 'src', 'e_powf.c'), 'e_powf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_remainder.c'), 'e_remainder.o'),
        (os.path.join(openlibm_directory, 'src', 'e_remainderf.c'), 'e_remainderf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_rem_pio2.c'), 'e_rem_pio2.o'),
        (os.path.join(openlibm_directory, 'src', 'e_rem_pio2f.c'), 'e_rem_pio2f.o'),
        (os.path.join(openlibm_directory, 'src', 'e_sinh.c'), 'e_sinh.o'),
        (os.path.join(openlibm_directory, 'src', 'e_sinhf.c'), 'e_sinhf.o'),
        (os.path.join(openlibm_directory, 'src', 'e_sqrt.c'), 'e_sqrt.o'),
        (os.path.join(openlibm_directory, 'src', 'e_sqrtf.c'), 'e_sqrtf.o'),
        (os.path.join(openlibm_directory, 'src', 'k_cos.c'), 'k_cos.o'),
        (os.path.join(openlibm_directory, 'src', 'k_exp.c'), 'k_exp.o'),
        (os.path.join(openlibm_directory, 'src', 'k_expf.c'), 'k_expf.o'),
        (os.path.join(openlibm_directory, 'src', 'k_rem_pio2.c'), 'k_rem_pio2.o'),
        (os.path.join(openlibm_directory, 'src', 'k_sin.c'), 'k_sin.o'),
        (os.path.join(openlibm_directory, 'src', 'k_tan.c'), 'k_tan.o'),
        (os.path.join(openlibm_directory, 'src', 'k_cosf.c'), 'k_cosf.o'),
        (os.path.join(openlibm_directory, 'src', 'k_sinf.c'), 'k_sinf.o'),
        (os.path.join(openlibm_directory, 'src', 'k_tanf.c'), 'k_tanf.o'),
        (os.path.join(openlibm_directory, 'src', 's_asinh.c'), 's_asinh.o'),
        (os.path.join(openlibm_directory, 'src', 's_asinhf.c'), 's_asinhf.o'),
        (os.path.join(openlibm_directory, 'src', 's_atan.c'), 's_atan.o'),
        (os.path.join(openlibm_directory, 'src', 's_atanf.c'), 's_atanf.o'),
        (os.path.join(openlibm_directory, 'src', 's_carg.c'), 's_carg.o'),
        (os.path.join(openlibm_directory, 'src', 's_cargf.c'), 's_cargf.o'),
        (os.path.join(openlibm_directory, 'src', 's_cbrt.c'), 's_cbrt.o'),
        (os.path.join(openlibm_directory, 'src', 's_cbrtf.c'), 's_cbrtf.o'),
        (os.path.join(openlibm_directory, 'src', 's_ceil.c'), 's_ceil.o'),
        (os.path.join(openlibm_directory, 'src', 's_ceilf.c'), 's_ceilf.o'),
        (os.path.join(openlibm_directory, 'src', 's_copysign.c'), 's_copysign.o'),
        (os.path.join(openlibm_directory, 'src', 's_copysignf.c'), 's_copysignf.o'),
        (os.path.join(openlibm_directory, 'src', 's_cos.c'), 's_cos.o'),
        (os.path.join(openlibm_directory, 'src', 's_cosf.c'), 's_cosf.o'),
        (os.path.join(openlibm_directory, 'src', 's_csqrt.c'), 's_csqrt.o'),
        (os.path.join(openlibm_directory, 'src', 's_csqrtf.c'), 's_csqrtf.o'),
        (os.path.join(openlibm_directory, 'src', 's_erf.c'), 's_erf.o'),
        (os.path.join(openlibm_directory, 'src', 's_erff.c'), 's_erff.o'),
        (os.path.join(openlibm_directory, 'src', 's_exp2.c'), 's_exp2.o'),
        (os.path.join(openlibm_directory, 'src', 's_exp2f.c'), 's_exp2f.o'),
        (os.path.join(openlibm_directory, 'src', 's_expm1.c'), 's_expm1.o'),
        (os.path.join(openlibm_directory, 'src', 's_expm1f.c'), 's_expm1f.o'),
        (os.path.join(openlibm_directory, 'src', 's_fabs.c'), 's_fabs.o'),
        (os.path.join(openlibm_directory, 'src', 's_fabsf.c'), 's_fabsf.o'),
        (os.path.join(openlibm_directory, 'src', 's_fdim.c'), 's_fdim.o'),
        (os.path.join(openlibm_directory, 'src', 's_floor.c'), 's_floor.o'),
        (os.path.join(openlibm_directory, 'src', 's_floorf.c'), 's_floorf.o'),
        (os.path.join(openlibm_directory, 'src', 's_fmax.c'), 's_fmax.o'),
        (os.path.join(openlibm_directory, 'src', 's_fmaxf.c'), 's_fmaxf.o'),
        (os.path.join(openlibm_directory, 'src', 's_fmin.c'), 's_fmin.o'),
        (os.path.join(openlibm_directory, 'src', 's_fminf.c'), 's_fminf.o'),
        (os.path.join(openlibm_directory, 'src', 's_fpclassify.c'), 's_fpclassify.o'),
        (os.path.join(openlibm_directory, 'src', 's_frexp.c'), 's_frexp.o'),
        (os.path.join(openlibm_directory, 'src', 's_frexpf.c'), 's_frexpf.o'),
        (os.path.join(openlibm_directory, 'src', 's_ilogb.c'), 's_ilogb.o'),
        (os.path.join(openlibm_directory, 'src', 's_ilogbf.c'), 's_ilogbf.o'),
        (os.path.join(openlibm_directory, 'src', 's_isinf.c'), 's_isinf.o'),
        (os.path.join(openlibm_directory, 'src', 's_isfinite.c'), 's_isfinite.o'),
        (os.path.join(openlibm_directory, 'src', 's_isnormal.c'), 's_isnormal.o'),
        (os.path.join(openlibm_directory, 'src', 's_isnan.c'), 's_isnan.o'),
        (os.path.join(openlibm_directory, 'src', 's_log1p.c'), 's_log1p.o'),
        (os.path.join(openlibm_directory, 'src', 's_log1pf.c'), 's_log1pf.o'),
        (os.path.join(openlibm_directory, 'src', 's_logb.c'), 's_logb.o'),
        (os.path.join(openlibm_directory, 'src', 's_logbf.c'), 's_logbf.o'),
        (os.path.join(openlibm_directory, 'src', 's_modf.c'), 's_modf.o'),
        (os.path.join(openlibm_directory, 'src', 's_modff.c'), 's_modff.o'),
        (os.path.join(openlibm_directory, 'src', 's_nextafter.c'), 's_nextafter.o'),
        (os.path.join(openlibm_directory, 'src', 's_nextafterf.c'), 's_nextafterf.o'),
        (os.path.join(openlibm_directory, 'src', 's_nexttowardf.c'), 's_nexttowardf.o'),
        (os.path.join(openlibm_directory, 'src', 's_remquo.c'), 's_remquo.o'),
        (os.path.join(openlibm_directory, 'src', 's_remquof.c'), 's_remquof.o'),
        (os.path.join(openlibm_directory, 'src', 's_rint.c'), 's_rint.o'),
        (os.path.join(openlibm_directory, 'src', 's_rintf.c'), 's_rintf.o'),
        (os.path.join(openlibm_directory, 'src', 's_round.c'), 's_round.o'),
        (os.path.join(openlibm_directory, 'src', 's_roundf.c'), 's_roundf.o'),
        (os.path.join(openlibm_directory, 'src', 's_scalbln.c'), 's_scalbln.o'),
        (os.path.join(openlibm_directory, 'src', 's_scalbn.c'), 's_scalbn.o'),
        (os.path.join(openlibm_directory, 'src', 's_scalbnf.c'), 's_scalbnf.o'),
        (os.path.join(openlibm_directory, 'src', 's_signbit.c'), 's_signbit.o'),
        (os.path.join(openlibm_directory, 'src', 's_signgam.c'), 's_signgam.o'),
        (os.path.join(openlibm_directory, 'src', 's_sin.c'), 's_sin.o'),
        (os.path.join(openlibm_directory, 'src', 's_sincos.c'), 's_sincos.o'),
        (os.path.join(openlibm_directory, 'src', 's_sinf.c'), 's_sinf.o'),
        (os.path.join(openlibm_directory, 'src', 's_sincosf.c'), 's_sincosf.o'),
        (os.path.join(openlibm_directory, 'src', 's_tan.c'), 's_tan.o'),
        (os.path.join(openlibm_directory, 'src', 's_tanf.c'), 's_tanf.o'),
        (os.path.join(openlibm_directory, 'src', 's_tanh.c'), 's_tanh.o'),
        (os.path.join(openlibm_directory, 'src', 's_tanhf.c'), 's_tanhf.o'),
        (os.path.join(openlibm_directory, 'src', 's_tgammaf.c'), 's_tgammaf.o'),
        (os.path.join(openlibm_directory, 'src', 's_trunc.c'), 's_trunc.o'),
        (os.path.join(openlibm_directory, 'src', 's_truncf.c'), 's_truncf.o'),
        (os.path.join(openlibm_directory, 'src', 's_cpow.c'), 's_cpow.o'),
        (os.path.join(openlibm_directory, 'src', 's_cpowf.c'), 's_cpowf.o'),
        (os.path.join(openlibm_directory, 'src', 'w_cabs.c'), 'w_cabs.o'),
        (os.path.join(openlibm_directory, 'src', 'w_cabsf.c'), 'w_cabsf.o'),
        (os.path.join(openlibm_directory, 'src', 's_fma.c'), 's_fma.o'),
        (os.path.join(openlibm_directory, 'src', 's_fmaf.c'), 's_fmaf.o'),
        (os.path.join(openlibm_directory, 'src', 's_lrint.c'), 's_lrint.o'),
        (os.path.join(openlibm_directory, 'src', 's_lrintf.c'), 's_lrintf.o'),
        (os.path.join(openlibm_directory, 'src', 's_lround.c'), 's_lround.o'),
        (os.path.join(openlibm_directory, 'src', 's_lroundf.c'), 's_lroundf.o'),
        (os.path.join(openlibm_directory, 'src', 's_llrint.c'), 's_llrint.o'),
        (os.path.join(openlibm_directory, 'src', 's_llrintf.c'), 's_llrintf.o'),
        (os.path.join(openlibm_directory, 'src', 's_llround.c'), 's_llround.o'),
        (os.path.join(openlibm_directory, 'src', 's_llroundf.c'), 's_llroundf.o'),
        (os.path.join(openlibm_directory, 'src', 's_nearbyint.c'), 's_nearbyint.o')
    ]

    build_objects_64bit(
        objects,
        'user_openlibm',
        '-I{}'.format(os.path.join(user_source_directory, 'shared')),
        '-I{}'.format(os.path.join(openlibm_directory, 'src')),
        '-I{}'.format(os.path.join(openlibm_directory, 'include')),
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

test_app_objects = [
    (os.path.join(user_source_directory, 'test_app', 'main.cpp'), 'main.o'),
    (os.path.join(source_directory, 'shared', 'memory.cpp'), 'memory.o'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o'),
]

build_objects_64bit(
    test_app_objects,
    'test_app',
    '-I{}'.format(os.path.join(source_directory, 'shared')),
    '-I{}'.format(os.path.join(user_source_directory, 'shared')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-I{}'.format(os.path.join(openlibm_directory, 'include')),
    '-I{}'.format(os.path.join(thirdparty_directory)),
    '-fpie'
)

do_linking(
    test_app_objects,
    'test_app',
    '--relocatable',
    user_openlibm_archive
)

init_objects = [
    (os.path.join(user_source_directory, 'init', 'main.cpp'), 'main.o'),
    (os.path.join(user_source_directory, 'init', 'virtio.cpp'), 'virtio.o'),
    (os.path.join(source_directory, 'shared', 'memory.cpp'), 'memory.o'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o'),
]

build_objects_64bit(
    init_objects,
    'init',
    '-I{}'.format(os.path.join(source_directory, 'shared')),
    '-I{}'.format(os.path.join(user_source_directory, 'shared')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-I{}'.format(os.path.join(user_source_directory, 'test_app')),
    '-D__BSD_VISIBLE=1',
    '-fpie'
)

do_linking(
    init_objects,
    'init',
    '--relocatable'
)

objects_multiprocessor = [
    (os.path.join(source_directory, 'kernel', 'multiprocessor.S'), 'multiprocessor.o')
]

build_objects_16bit(
    objects_multiprocessor,
    'multiprocessor',
    '-mcmodel=kernel',
    '-fno-stack-protector',
    '-mno-red-zone',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2'
)

do_linking(
    objects_multiprocessor,
    'multiprocessor',
    '-e', 'entry',
    '-T', os.path.join(source_directory, 'kernel', 'multiprocessor.ld')
)

run_command(
    shutil.which('llvm-objcopy'),
    '--output-target=binary',
    '--only-section', '.text',
    '--only-section', '.data',
    '--only-section', '.rodata',
    os.path.join(build_directory, 'multiprocessor.elf'),
    os.path.join(build_directory, 'multiprocessor.bin')
)

objects_kernel = [
    (os.path.join(source_directory, 'kernel', 'static_init.S'), 'static_init.o'),
    (os.path.join(source_directory, 'kernel', 'embed.S'), 'embed.o'),
    (os.path.join(source_directory, 'kernel', 'syscall.S'), 'syscall.o'),
    (os.path.join(source_directory, 'kernel', 'preempt.S'), 'preempt.o'),
    (os.path.join(source_directory, 'kernel', 'interrupts.S'), 'interrupts.o'),
    (os.path.join(source_directory, 'kernel', 'main.cpp'), 'main.o'),
    (os.path.join(source_directory, 'kernel', 'process.cpp'), 'process.o'),
    (os.path.join(source_directory, 'kernel', 'console.cpp'), 'console.o'),
    (os.path.join(source_directory, 'kernel', 'paging.cpp'), 'paging.o'),
    (os.path.join(source_directory, 'kernel', 'io.cpp'), 'io.o'),
    (os.path.join(source_directory, 'kernel', 'acpi_environment.cpp'), 'acpi_environment.o'),
    (os.path.join(source_directory, 'shared', 'memory.cpp'), 'memory.o'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o')
]

build_objects_64bit(
    objects_kernel,
    'kernel',
    '-I{}'.format(os.path.join(source_directory, 'shared')),
    '-I{}'.format(os.path.join(acpica_directory, 'include')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-mcmodel=kernel',
    '-fno-stack-protector',
    '-mno-red-zone',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2',
    '-Wframe-larger-than=4096'
)

do_linking(
    objects_kernel,
    'kernel',
    '-e', 'main',
    '-T', os.path.join(source_directory, 'kernel', 'linker.ld'),
    acpica_archive
)

run_command(
    shutil.which('llvm-objcopy'),
    '--output-target=binary',
    '--only-section', '.text',
    '--only-section', '.data',
    '--only-section', '.rodata',
    '--only-section', '.eh_frame',
    os.path.join(build_directory, 'kernel.elf'),
    os.path.join(build_directory, 'kernel.bin')
)

objects_uefi = [
    (os.path.join(source_directory, 'uefi_bootloader', 'main.cpp'), 'main.obj'),
    (os.path.join(source_directory, 'uefi_bootloader', 'embed.S'), 'embed.obj'),
    (os.path.join(printf_directory, 'printf.c'), 'printf.o'),
]

build_objects(
    objects_uefi,
    'x86_64-unknown-windows',
    'uefi_bootloader',
    '-I{}'.format(os.path.join(source_directory, 'kernel')),
    '-I{}'.format(os.path.join(edk2_directory, 'MdePkg', 'Include')),
    '-I{}'.format(os.path.join(edk2_directory, 'MdePkg', 'Include', 'X64')),
    '-I{}'.format(os.path.join(printf_directory)),
    '-mcmodel=kernel',
    '-fno-stack-protector',
    '-mno-red-zone',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2'
)

run_command(
    shutil.which('lld-link'),
    '-entry:efi_main',
    '-subsystem:efi_application',
    '-base:0x800000',
    '-out:{}'.format(os.path.join(build_directory, 'BOOTX64.EFI')),
    *[os.path.join(object_directory, 'uefi_bootloader', object_name) for _, object_name in objects_uefi]
)

end_time = time.time()

print('Total time: {:.3f}s'.format(end_time - start_time))