#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <iomanip>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

using namespace snmalloc;

void test(size_t level)
{
    if (level == 0)
        return;
    std::cout << "Level " << level << std::endl;

    size_t size = 128;
    size_t current = Alloc::Config::Backend::get_peak_usage();
    ScopedAllocator prod_alloc;
    size_t allocated = 0;
    
    std::vector<void*> allocs;

    while (true) {
        allocs.push_back(prod_alloc->alloc(size));

        allocated += size;

        if (current != Alloc::Config::Backend::get_peak_usage())
        {
            current = Alloc::Config::Backend::get_peak_usage();
            std::cout << "Peak usage:    " << current << " for " << allocated << " bytes" << std::endl;
        }

        if (allocated > 8 * 1024 * 1024)
            break;
    }

    for(auto a: allocs)
    {
        prod_alloc->dealloc(a);
    }

    current = Alloc::Config::Backend::get_current_usage();
    std::cout << "Current usage: " << current << "@" << level << std::endl;

    test(level - 1);

    current = Alloc::Config::Backend::get_current_usage();
    std::cout << "Final usage:   " << current << "@" << level << std::endl;
}   

int main()
{
    test(20);

    auto current = Alloc::Config::Backend::get_current_usage();
    std::cout << "Final usage:   " << current << std::endl;

    return 0;
}
