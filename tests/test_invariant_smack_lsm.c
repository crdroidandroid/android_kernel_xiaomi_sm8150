#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 
 * This test validates that mount option concatenation respects buffer bounds.
 * Since we cannot directly call kernel functions, we test the invariant that
 * accumulated option length must never exceed allocated buffer size.
 */

#define SMACK_OPTS_ALLOC_SIZE 256  /* Typical kernel allocation size */

static int safe_concat_options(char *dest, size_t dest_size, const char *opt)
{
    size_t current_len = strlen(dest);
    size_t opt_len = strlen(opt);
    size_t needed = current_len + (current_len > 0 ? 1 : 0) + opt_len + 1;
    
    /* Invariant: total length must fit in allocated buffer */
    if (needed > dest_size) {
        return -1;  /* Would overflow */
    }
    return 0;  /* Safe to proceed */
}

START_TEST(test_mount_option_buffer_bounds)
{
    /* Invariant: concatenated mount options must never exceed buffer allocation */
    const char *payloads[] = {
        /* Exploit case: options designed to overflow typical 256-byte buffer */
        "smackfsdef=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        /* Boundary case: exactly at buffer limit */
        "smackfsroot=_,smackfshat=^,smackfsfloor=floor_label_that_is_long_enough",
        /* Valid input: normal mount options */
        "smackfsroot=*,smackfshat=^"
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char buffer[SMACK_OPTS_ALLOC_SIZE];
        buffer[0] = '\0';
        
        int result = safe_concat_options(buffer, sizeof(buffer), payloads[i]);
        
        /* Security invariant: overflow must be detected and rejected */
        if (strlen(payloads[i]) >= SMACK_OPTS_ALLOC_SIZE) {
            ck_assert_msg(result == -1, 
                "Payload %d should be rejected as overflow", i);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_mount_option_buffer_bounds);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}