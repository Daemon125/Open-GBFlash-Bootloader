/* guard.c — see guard.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "guard.h"

static size_t gp_page(void)
{
    static size_t p;
    if (!p) p = (size_t)sysconf(_SC_PAGESIZE);
    return p;
}

static size_t gp_round(size_t n, size_t page)
{
    return (n + page - 1u) / page * page;
}

void *gp_alloc_tail(size_t n)
{
    size_t page = gp_page();
    size_t body = gp_round(n ? n : 1u, page);
    size_t total = body + 2u * page;
    unsigned char *base = mmap(0, total, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *data;

    if (base == MAP_FAILED) { perror("mmap"); abort(); }
    if (mprotect(base, page, PROT_NONE) != 0 ||
        mprotect(base + page + body, page, PROT_NONE) != 0) {
        perror("mprotect");
        abort();
    }
    /* Place the object so its last byte is the last byte of the writable
     * region: one byte past the end lands in the trailing guard page. */
    data = base + page + body - n;
    memset(data, 0, n);
    return data;
}

void gp_free(void *p, size_t n)
{
    size_t page = gp_page();
    size_t body = gp_round(n ? n : 1u, page);
    unsigned char *data = (unsigned char *)p;
    unsigned char *base = data + n - body - page;
    if (!p) return;
    (void)munmap(base, body + 2u * page);
}
