#include <snmalloc/snmalloc.h>

// This test checks that the malloc and free overrides can be used

int main()
{
    // Test that malloc can be handled by snmalloc's free.
    auto p = malloc(16);
    snmalloc::libc::free(p);

    // Test that snmalloc's malloc can be handled by free.
    auto q = snmalloc::libc::malloc(16);
    free(q);

    return 0;
}