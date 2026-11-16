/*
 * Small helpers shared by the tests: a scratch directory and its removal.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_TEST_UTIL_H
#define PMD85_TEST_UTIL_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* A fresh directory under the system temp dir. 0 on success. */
static inline int test_mkdtemp(char *dst, size_t dstsz, const char *tag)
{
#if defined(_WIN32)
    char base[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof base, base);
    if (n == 0 || n >= sizeof base) return -1;
    snprintf(dst, dstsz, "%s%s-%lu-%lu", base, tag,
             (unsigned long)_getpid(), (unsigned long)GetTickCount());
    return _mkdir(dst);
#else
    const char *tmp = getenv("TMPDIR");
    snprintf(dst, dstsz, "%s/%s-XXXXXX", (tmp && *tmp) ? tmp : "/tmp", tag);
    return mkdtemp(dst) ? 0 : -1;
#endif
}

static inline void test_rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    if (!d) {
        remove(path);
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char p[2048];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            test_rmtree(p);
        else
            remove(p);
    }
    closedir(d);
#if defined(_WIN32)
    _rmdir(path);
#else
    rmdir(path);
#endif
}

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
/* 1 if something accepts connections on 127.0.0.1:port. */
static inline int test_port_in_use(int port)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), ok;
    if (fd < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}
#endif

#endif /* PMD85_TEST_UTIL_H */
