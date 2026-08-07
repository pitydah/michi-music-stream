/* Strict semver (x.y.z numeric only; used for downgrade prevention).
 * Mirrors the signer's validation: every component is digit-only (no
 * leading whitespace, no sign, no leading zeros - '01.2.3' is rejected)
 * and fits in uint16_t. */

#include "semver.h"

#include <ctype.h>
#include <string.h>

bool semver_parse(const char *s, uint16_t out[3])
{
    uint16_t v[3] = {0};
    for (int i = 0; i < 3; i++) {
        const char *p = s;
        if (*p == '\0' || !isdigit((unsigned char)*p)) {
            return false;
        }
        if (*p == '0' && isdigit((unsigned char)p[1])) {
            return false; /* leading zero: '01' rejected */
        }
        unsigned long part = 0;
        while (isdigit((unsigned char)*p)) {
            part = part * 10 + (unsigned long)(*p - '0');
            if (part > 65535) {
                return false;
            }
            p++;
        }
        v[i] = (uint16_t)part;
        if (i < 2) {
            if (*p != '.') {
                return false;
            }
            s = p + 1;
        } else if (*p != '\0') {
            return false;
        }
    }
    memcpy(out, v, sizeof(v));
    return true;
}

int semver_cmp(const uint16_t a[3], const uint16_t b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}
