#include "burning_utility.h"
#include <iostream>

int main() {
    std::cout << "Testing basic header inclusion..." << std::endl;
    const char* platform = GetPlatformInfo();
    std::cout << "Platform: " << (platform ? platform : "Unknown") << std::endl;
    return 0;
}