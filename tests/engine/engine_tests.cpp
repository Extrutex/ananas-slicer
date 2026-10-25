#include <catch2/catch_all.hpp>

// libslic3r's SVG import needs the nanosvg implementation in exactly one translation unit of a test
// executable (see the other suites' mains).
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
