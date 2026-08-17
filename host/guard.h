/* guard.h — guard-page allocation.
 *
 * WHY THIS EXISTS: AddressSanitizer does not run on this machine. On
 * macOS 27.0 (build 26A5406e) with Apple clang 17.0.0 an -fsanitize=address
 * binary hangs at startup inside the runtime's shadow-mapping search:
 *
 *     ==NNN==AddressSanitizer: libc interceptors initialized
 *     ==NNN==FindDynamicShadowStart, space_size = 0x0fffffffffff
 *     <spins at 100% CPU forever>
 *
 * — reproducible with a three-line hello-world, so it is the environment, not
 * this code. UndefinedBehaviorSanitizer works normally and IS used
 * (-fsanitize=undefined,local-bounds catches every out-of-range index into the
 * fixed-size arrays inside bl_proto, which is the exact overflow class that
 * matters here). Guard pages cover the rest: an object allocated with
 * gp_alloc_tail() ends flush against a PROT_NONE page, so any access one byte
 * past it is a SIGSEGV rather than silent corruption.
 *
 * The Makefile keeps a `make test SAN=asan` path so that on a machine where
 * ASan works, it is one word away.
 */

#ifndef GUARD_H
#define GUARD_H

#include <stddef.h>

/* Returns a pointer p such that [p, p+n) is readable and writable and the byte
 * at p+n is the first byte of an unmapped page. Also guarded below the
 * allocation, though page rounding leaves slack there. Aborts on failure. */
void *gp_alloc_tail(size_t n);

/* Free an allocation made by gp_alloc_tail with the same n. */
void  gp_free(void *p, size_t n);

#endif /* GUARD_H */
