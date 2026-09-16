// SPDX-License-Identifier: MPL-2.0
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIGIL_OK            0
#define SIGIL_ERR_NOT_FOUND -5

extern int sigil_cnf_parse_boot(const uint8_t *cnf, size_t len,
                                const char *boot_key,
                                char raw[32], char canonical[32]);

static int failures = 0;

static void expect(const char *cnf, const char *key, int want_rc,
                   const char *want_raw, const char *want_canonical) {
    char raw[32] = {0};
    char canonical[32] = {0};
    int rc = sigil_cnf_parse_boot((const uint8_t *)cnf, strlen(cnf), key, raw, canonical);
    if (rc != want_rc) {
        fprintf(stderr, "FAIL [%s] rc=%d want %d\n", cnf, rc, want_rc);
        failures++;
        return;
    }
    if (want_rc != SIGIL_OK) return;
    if (strcmp(raw, want_raw) != 0 || strcmp(canonical, want_canonical) != 0) {
        fprintf(stderr, "FAIL [%s] raw=%s canonical=%s want %s %s\n",
                cnf, raw, canonical, want_raw, want_canonical);
        failures++;
    }
}

int main(void) {
    expect("BOOT=cdrom:\\SCUS_945.03;1\r\nTCB=4\r\n", "BOOT", SIGIL_OK,
           "SCUS_945.03", "SCUS-94503");
    expect("BOOT = cdrom:\\slus_005.94;1\r\nTCB = 4\r\n", "BOOT", SIGIL_OK,
           "slus_005.94", "SLUS-00594");
    expect("BOOT = cdrom:\\Slus_012.48;1\r\n", "BOOT", SIGIL_OK,
           "Slus_012.48", "SLUS-01248");
    expect("BOOT = cdrom:\\MARL\\slus_010.73;1\r\n", "BOOT", SIGIL_OK,
           "slus_010.73", "SLUS-01073");
    expect("BOOT = cdrom:\\SLUS_00.220;1\r\n", "BOOT", SIGIL_OK,
           "SLUS_00.220", "SLUS-00220");
    expect("BOOT = cdrom:\\SCUS-941.02;1\r\n", "BOOT", SIGIL_OK,
           "SCUS-941.02", "SCUS-94102");
    expect("BOOT = cdrom:\\SLUSP012.06;1\r\n", "BOOT", SIGIL_OK,
           "SLUSP012.06", "SLUS-01206");
    expect("BOOT2 = cdrom0:\\sles_503.30;1\r\nVER = 1.00\r\n", "BOOT2", SIGIL_OK,
           "sles_503.30", "SLES-50330");
    expect("BOOT = cdrom:\\SLUS_012.345;1\r\n", "BOOT", SIGIL_ERR_NOT_FOUND, NULL, NULL);
    expect("BOOT = cdrom:\\XSLUS_012.34;1\r\n", "BOOT", SIGIL_ERR_NOT_FOUND, NULL, NULL);
    expect("BOOT2 = cdrom0:\\SLUS_201.52;1\r\n", "BOOT", SIGIL_ERR_NOT_FOUND, NULL, NULL);
    expect("BOOT=cdrom:\\LSP99012.201;1\r\n", "BOOT", SIGIL_ERR_NOT_FOUND, NULL, NULL);
    expect("BOOT=cdrom:\\PSX.EXE;1\r\n", "BOOT", SIGIL_ERR_NOT_FOUND, NULL, NULL);

    if (failures) return 1;
    printf("cnf: all passed\n");
    return 0;
}
