// Do not override the malloc family of functions in the pass through case
// as infinite recursion will occur.
#ifndef SNMALLOC_PASS_THROUGH 
#  define SNMALLOC_STATIC_LIBRARY_PREFIX
#  include <snmalloc/override/malloc.cc>
#endif