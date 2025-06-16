#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// #include <format>
// #include <iostream>

// #define DOCTEST_CONFIG_IMPLEMENT
// #include <doctest/doctest.h>

// int
// main(int argc, char** argv)
// {
//   doctest::Context context;

//   // !!! THIS IS JUST AN EXAMPLE SHOWING HOW DEFAULTS/OVERRIDES ARE SET !!!

//   // defaults
//   context.addFilter("test-case-exclude", "*math*"); // exclude test cases with "math" in their
//   name context.setOption("abort-after", 5);              // stop test execution after 5 failed
//   assertions context.setOption("order-by", "name");            // sort the test cases by their
//   name

//   context.applyCommandLine(argc, argv);

//   // overrides
//   context.setOption("no-breaks", true); // don't break in the debugger when assertions fail

//   int result = 0;
//   int run    = 0;
//   while (++run > 0)
//   {
//     if ((result = context.run()) != 0) // NOLINT
//     {
//       break;
//     }
//     std::cout << std::format("PASSED RUN: {}\n", run);
//   }

//   return result;
// }
