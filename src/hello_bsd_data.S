/*
 * The demo application, linked into the kernel image.
 *
 * Not loaded from disk, deliberately: the volume this port tests against
 * is attached read-only and holds an x86_64 build of the same program,
 * which an aarch64 kernel now correctly refuses. Embedding the aarch64
 * image keeps the loader exercised on every boot without needing a
 * writable disk to stage it on, and the loader takes the same path either
 * way — it is handed a pointer and a length and does not care where they
 * came from.
 */
.section .rodata
.balign 16
.global hello_bsd
hello_bsd:
    .incbin "build/hello.bsd"
.global hello_bsd_end
hello_bsd_end:
.global hello_bsd_len
.balign 8
hello_bsd_len:
    .quad hello_bsd_end - hello_bsd
