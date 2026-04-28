#include "unity.h"
#include <stdio.h>

unsigned unity_tests_run    = 0;
unsigned unity_tests_passed = 0;
unsigned unity_tests_failed = 0;
const char *unity_current_test = "(none)";

void unity_fail(const char *file, int line, const char *msg)
{
    unity_tests_failed++;
    printf("  [FAIL] %s\n    %s:%d: %s\n",
           unity_current_test, file, line, msg ? msg : "");
}

void unity_print_summary(void)
{
    printf("\n========================================\n");
    printf("Tests run:    %u\n", unity_tests_run);
    printf("Tests passed: %u\n", unity_tests_passed);
    printf("Tests failed: %u\n", unity_tests_failed);
    printf("========================================\n");
    if (unity_tests_failed == 0u) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("SOME TESTS FAILED\n");
    }
    printf("========================================\n");
}
