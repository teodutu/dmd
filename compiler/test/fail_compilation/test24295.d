// https://issues.dlang.org/show_bug.cgi?id=24295

// REQUIRED_ARGS: -betterC

/*
TEST_OUTPUT:
---
fail_compilation/test24295.d(14): Error: array creation in `new int[](1LU)` requires the GC which is not available with -betterC
---
*/

void f()
{
   int[] x = new int[1];
}
