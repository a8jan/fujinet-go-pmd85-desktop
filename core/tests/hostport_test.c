/*
 * pmd85session_parse_hostport: the external-FujiNet address field.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "pmd85session.h"

static int failures;

static void ok(const char *in, const char *host, int port)
{
    char h[64] = "";
    int p = 0;
    if (pmd85session_parse_hostport(in, h, sizeof h, &p) != 0 ||
        strcmp(h, host) != 0 || p != port) {
        fprintf(stderr, "FAIL \"%s\" -> \"%s\":%d, want \"%s\":%d\n",
                in, h, p, host, port);
        failures++;
    }
}

static void bad(const char *in)
{
    char h[64];
    int p;
    if (pmd85session_parse_hostport(in, h, sizeof h, &p) == 0) {
        fprintf(stderr, "FAIL \"%s\" accepted as \"%s\":%d\n", in, h, p);
        failures++;
    }
}

int main(void)
{
    ok("", "127.0.0.1", PMD85SESSION_BECKER_PORT);
    ok("  ", "127.0.0.1", PMD85SESSION_BECKER_PORT);
    ok("192.168.1.20:65504", "192.168.1.20", 65504);
    ok(" fujinet.local:1234 \n", "fujinet.local", 1234);
    ok("fujinet.local", "fujinet.local", PMD85SESSION_BECKER_PORT);
    ok("6000", "127.0.0.1", 6000);
    bad(":65505");
    bad("host:");
    bad("host:0");
    bad("host:70000");
    bad("host:12x");
    bad("bad_host:1");
    bad(".host:1");
    bad("host.:1");
    if (failures) return 1;
    printf("hostport: ok\n");
    return 0;
}
