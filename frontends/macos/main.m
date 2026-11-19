/*
 * FujiNet Go PMD 85 -- the macOS (AppKit) frontend's entry point.
 *
 * From fujinet-go-coco-desktop's main.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include <stdio.h>
#include <string.h>

#include "pmd85session.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        /* The bundled FujiNet runtime (libfujinet.dylib in
         * Contents/Frameworks, the pristine runtime tree in
         * Contents/Resources/fujinet) takes priority; without it the session
         * falls back to its default search and, failing that, runs without
         * FujiNet. */
        pmd85session_paths paths;
        memset(&paths, 0, sizeof paths);
        NSString *lib = [[[NSBundle mainBundle] privateFrameworksPath]
            stringByAppendingPathComponent:@"libfujinet.dylib"];
        NSString *src = [[[NSBundle mainBundle] resourcePath]
            stringByAppendingPathComponent:@"fujinet"];
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:lib])
            paths.fujinet_lib = lib.fileSystemRepresentation;
        if ([fm fileExistsAtPath:[src stringByAppendingPathComponent:@"fnconfig.ini"]])
            paths.fujinet_runtime_src = src.fileSystemRepresentation;

        pmd85session *session = pmd85session_new(&paths);
        if (!session) {
            fprintf(stderr, "fatal: could not create the session\n");
            return 1;
        }
        pmd85session_start_opts opts;
        pmd85session_default_opts(session, &opts);
        if (pmd85session_start(session, &opts) != 0)
            fprintf(stderr, "session start: %s\n", pmd85session_last_error(session));

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        PmdAppDelegate *delegate = [[PmdAppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app activateIgnoringOtherApps:YES];
        [app run];

        pmd85session_free(session);
    }
    return 0;
}
