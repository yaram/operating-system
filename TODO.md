## In progress

- better error handling (cascade out-of-memory errors up call stack, maybe no useless "thing here" error messages)
- ELF executable validation

## Not started / ideas

- multiprocessing
- processes subscribe to hardware interrupts
- \[user\] test keyboard input with USB keyboard maybe
- \[user\] test ahci/sata disk access/loading
- dynamic object file loading
- \[user\] release a toolchain (just llvm or do we need binutils?)
- \[user\] first-party/reference/example std library / hardware abstraction library
- custom object/binary format with custom 'linker'
- use semantic user & kernel memory/page range types for more compile-time checking/type safety & runtime bounds checking