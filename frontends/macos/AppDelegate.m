/*
 * PmdAppDelegate: the main window (the picture, the PMD's LEDs and the PMD 32
 * light in the title bar, the status in its subtitle), the menu bar, the
 * session's lifecycle, and the FujiNet console log.
 *
 * The same surface as the GNOME app (frontends/gnome/window.c) in the Mac's
 * terms: the app's keys are Command ones -- the PMD has its own Shift and
 * Ctrl (STOP), and Option types characters -- and closing the main window
 * quits, as in the Atari 800 app.
 *
 *   Cmd+R RESET, Cmd+Shift+R cold restart, Cmd+Shift+C next colors,
 *   Ctrl+Cmd+F full screen, Cmd+T the Tape window, Cmd+P tape play/stop,
 *   Cmd+[ / Cmd+] previous/next file, Cmd+O load tape, Cmd+S save tape,
 *   Cmd+L FujiNet's console log, Cmd+, Settings.
 *
 * Structure from fujinet-go-coco-desktop's AppDelegate.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "SettingsWindow.h"
#import "TapeWindow.h"

#include <stdlib.h>
#include <string.h>

#define TOAST_SECONDS 4.0

static NSString *str(const char *s)
{
    return s ? ([NSString stringWithUTF8String:s] ?: @"") : @"";
}

static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    return types;
}

/* ---- the title bar's lights: the PMD's three LEDs and the PMD 32's ---------- */

@interface PmdLights : NSView
@property (nonatomic) int leds;        /* bit 0 yellow, 1 red, 2 blue */
@property (nonatomic) int disk;        /* -1 no PMD 32, 0 idle, 1 busy */
@end

@implementation PmdLights

- (BOOL)isFlipped { return YES; }

- (NSSize)intrinsicContentSize { return NSMakeSize(76, 22); }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    static const CGFloat rgb[3][3] = {
        { 0xe8 / 255.0, 0xc8 / 255.0, 0x32 / 255.0 },
        { 0xe0 / 255.0, 0x40 / 255.0, 0x40 / 255.0 },
        { 0x40 / 255.0, 0x80 / 255.0, 0xf0 / 255.0 },
    };
    const CGFloat y = (self.bounds.size.height - 10) / 2;
    for (int i = 0; i < 3; i++) {
        const CGFloat a = (_leds & (1 << i)) ? 1.0 : 0.18;
        [[NSColor colorWithSRGBRed:rgb[i][0] green:rgb[i][1] blue:rgb[i][2] alpha:a] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(4 + i * 15, y, 10, 10)] fill];
    }
    if (_disk >= 0) {
        NSImage *img = [NSImage imageWithSystemSymbolName:@"externaldrive"
                                 accessibilityDescription:@"PMD 32"];
        [img drawInRect:NSMakeRect(52, y - 3, 18, 16)
               fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:_disk ? 1.0 : 0.3
         respectFlipped:YES
                  hints:nil];
    }
}

- (void)setLeds:(int)leds
{
    if (leds == _leds) return;
    _leds = leds;
    [self setNeedsDisplay:YES];
}

- (void)setDisk:(int)disk
{
    if (disk == _disk) return;
    _disk = disk;
    [self setNeedsDisplay:YES];
}

@end

/* ---- the delegate ------------------------------------------------------------ */

@implementation PmdAppDelegate {
    pmd85session *_session;
    NSWindow *_window;
    DisplayView *_display;
    PmdLights *_lights;
    NSTextField *_toast;
    NSUInteger _toastSerial;
    NSTimer *_statusTimer;
    id<NSObject> _activity;
    BOOL _quitting;
    BOOL _fujiRestarting;
    int _lastRomLoads;
    unsigned _padEvent;

    NSMenu *_disksMenu;

    PmdSettingsWindow *_settings;
    PmdTapeWindow *_tape;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    pmd85_log_cursor _logCursor;
    BOOL _logPlaceholder;
}

- (instancetype)initWithSession:(pmd85session *)session
{
    self = [super init];
    if (self) _session = session;
    return self;
}

/* ---- helpers ------------------------------------------------------------------ */

- (void)toast:(NSString *)message
{
    if (!message.length || !_toast) return;
    _toast.stringValue = message;
    _toast.hidden = NO;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial == self->_toastSerial) self->_toast.hidden = YES;
    });
}

- (void)applyDisplaySettings
{
    [_display setAspectMode:pmd85session_get_int(_session, "aspect_mode", PMD85SESSION_ASPECT_TV)];
    [_display setScaling:pmd85session_get_int(_session, "scaling", PMD_SCALING_SHARP)];
}

- (void)showStartWarnings
{
    const char *w = pmd85session_fujinet_warning(_session);
    if (!pmd85session_is_running(_session))
        [self toast:str(pmd85session_last_error(_session))];
    else if (w && *w)
        [self toast:str(w)];
}

- (void)restartSession
{
    pmd85session_start_opts opts;
    pmd85session_stop(_session);
    pmd85session_default_opts(_session, &opts);
    pmd85session_start(_session, &opts);
    [self showStartWarnings];
}

- (void)openPanel:(NSOpenPanel *)panel from:(NSWindow *)parent
             then:(void (^)(NSModalResponse r))then
{
    [panel beginSheetModalForWindow:(parent ?: _window) completionHandler:then];
}

/* ---- status: the subtitle, the lights, the toasts it has news for ------------ */

- (void)statusTick
{
    pmd85session *s = _session;
    if (_quitting || !s) return;
    const int page = pmd85session_mega_page(s);
    int last = 0;
    const int loads = pmd85session_rom_loads(s, &last);
    NSString *link;

    if (!pmd85session_is_running(s))
        link = @"stopped";
    else if (pmd85session_fujinet_link(s))
        link = pmd85session_fujinet_external(s) ? @"external FujiNet" : @"FujiNet linked";
    else if (pmd85session_fujinet_running(s) || pmd85session_fujinet_external(s))
        link = @"FujiNet not linked";
    else
        link = @"no FujiNet";

    NSMutableString *sub = [NSMutableString stringWithFormat:@"%@ · %@ · %@",
        str(pmd85session_model_name(pmd85session_model(s))),
        str(pmd85session_color_name(pmd85session_color_profile(s))), link];
    /* the page only when there are pages: an image over 32 KB */
    if (page >= 0 && pmd85session_mega_size(s) > 32 * 1024)
        [sub appendFormat:@" · MRM #%d", page];
    pmd85session_tape_info t;
    pmd85session_tape_get_info(s, &t);
    if (t.blocks > 0) {
        NSString *blk = strcmp(t.block, "..") ? str(t.block) : @"data";
        if (t.playing && t.progress_max > 0)
            [sub appendFormat:@" · Tape ▶ %@ %d%%", blk, t.progress * 100 / t.progress_max];
        else
            [sub appendFormat:@" · Tape %@ (%d/%d)%@", blk, t.current + 1, t.blocks,
                              t.recorded ? @", recorded" : @""];
    }
    if (![_window.subtitle isEqualToString:sub]) _window.subtitle = sub;

    _lights.leds = pmd85session_leds(s);
    pmd85session_disk_info d;
    pmd85session_disk_get_info(s, &d);
    _lights.disk = !d.present ? -1 : d.activity ? 1 : 0;
    NSString *tip = d.activity
        ? [NSString stringWithFormat:@"PMD 85 LEDs; PMD 32: drive %c %@", 'A' + (d.activity - 1) % 4,
                                     d.activity > 4 ? @"writing" : @"busy"]
        : @"The PMD 85 keyboard LEDs";
    if (![_lights.toolTip isEqualToString:tip]) _lights.toolTip = tip;

    {   /* a gamepad plugged in or out */
        char msg[160];
        const unsigned serial = pmd85session_gamepad_last_event(s, msg, sizeof msg);
        if (serial != _padEvent) {
            _padEvent = serial;
            if (msg[0]) [self toast:str(msg)];
        }
    }
    if (loads != _lastRomLoads) {
        _lastRomLoads = loads;
        [self toast:[NSString stringWithFormat:@"ROM module loaded from FujiNet (%d KB)",
                                               (last + 1023) / 1024]];
    }
}

/* ---- the machine -------------------------------------------------------------- */

- (void)selectModel:(NSMenuItem *)sender
{
    const int model = (int)sender.tag;
    const int module = pmd85session_module(_session);
    pmd85session_set_model(_session, model);
    NSString *name = str(pmd85session_model_name(model));
    const char *hint = pmd85session_module_hint(_session, model, module);
    if (!pmd85session_module_supported(model, module))
        [self toast:[NSString stringWithFormat:@"%@ (no %@ module on this model)", name,
                                               str(pmd85session_module_name(module))]];
    else if (hint)
        [self toast:[NSString stringWithFormat:@"%@. %@", name, str(hint)]];
    else
        [self toast:name];
}

- (void)chooseModuleFileFrom:(NSWindow *)parent done:(void (^)(BOOL chosen))done
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose a ROM module file: .rmm (a 32 KB ROM pack) or .mrm (MegaModule)";
    panel.allowedContentTypes = typesForExtensions(@[ @"rmm", @"mrm" ]);
    const char *cur = pmd85session_module_file(_session);
    if (cur && *cur)
        panel.directoryURL = [NSURL fileURLWithPath:[str(cur) stringByDeletingLastPathComponent]];
    [self openPanel:panel from:parent then:^(NSModalResponse r) {
        BOOL chosen = NO;
        if (r == NSModalResponseOK && panel.URL) {
            if (pmd85session_set_module_file(self->_session, panel.URL.fileSystemRepresentation) == 0) {
                [self toast:[@"ROM module: " stringByAppendingString:panel.URL.lastPathComponent]];
                chosen = YES;
            } else {
                [self toast:str(pmd85session_last_error(self->_session))];
            }
        }
        if (done) done(chosen);
    }];
}

- (void)chooseModuleFile:(id)sender
{
    (void)sender;
    [self chooseModuleFileFrom:nil done:nil];
}

- (void)selectModule:(NSMenuItem *)sender
{
    const int module = (int)sender.tag;
    /* "Module file" with nothing (left) to load asks for one first */
    if (module == PMD85SESSION_MODULE_FILE && !pmd85session_module_file_ok(_session)) {
        [self chooseModuleFileFrom:nil done:nil];
        return;
    }
    pmd85session_set_module(_session, module);
}

- (void)selectColor:(NSMenuItem *)sender
{
    pmd85session_set_color_profile(_session, (int)sender.tag);
}

/* the subtitle shows it */
- (void)nextColor:(id)sender
{
    (void)sender;
    pmd85session_set_color_profile(_session, (pmd85session_color_profile(_session) + 1) %
                                                 PMD85SESSION_COLOR_COUNT);
}

- (void)resetMachine:(id)sender
{
    (void)sender;
    pmd85session_reset(_session);
    [self toast:@"RESET"];
}

- (void)coldRestart:(id)sender
{
    (void)sender;
    pmd85session_hard_reset(_session);
    [self toast:@"Cold restart"];
}

/* ---- the PMD 32's disks -----------------------------------------------------
 * Machine > PMD 32 Disks, built each time it opens: whether the drive is
 * connected (and the command that changes it), then a submenu per drive. */

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (menu != _disksMenu) return;
    pmd85session_disk_info info;
    pmd85session_disk_get_info(_session, &info);
    [menu removeAllItems];

    NSMenuItem *state = [[NSMenuItem alloc] initWithTitle:info.enabled ? @"Connected" : @"Not connected"
                                                   action:NULL keyEquivalent:@""];
    state.enabled = NO;
    [menu addItem:state];
    NSMenuItem *conn = [[NSMenuItem alloc]
        initWithTitle:info.enabled ? @"Disconnect the PMD 32" : @"Connect the PMD 32"
               action:@selector(pmd32Connect:) keyEquivalent:@""];
    conn.target = self;
    conn.tag = info.enabled ? 0 : 1;
    [menu addItem:conn];
    [menu addItem:[NSMenuItem separatorItem]];

    for (int d = 0; d < 4; d++) {
        NSString *base = info.path[d][0] ? str(info.path[d]).lastPathComponent : nil;
        NSMenuItem *drive = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%c: %@", 'A' + d, base ?: @"empty"]
                   action:NULL keyEquivalent:@""];
        NSMenu *sub = [[NSMenu alloc] initWithTitle:drive.title];
        sub.autoenablesItems = YES;
        NSMenuItem *it = [sub addItemWithTitle:@"Insert Disk…" action:@selector(diskInsert:)
                                 keyEquivalent:@""];
        it.target = self;
        it.tag = d;
        if (base) {
            it = [sub addItemWithTitle:@"Eject" action:@selector(diskEject:) keyEquivalent:@""];
            it.target = self;
            it.tag = d;
        }
        it = [sub addItemWithTitle:@"Write Protected" action:@selector(diskProtect:) keyEquivalent:@""];
        it.target = self;
        it.tag = d;
        it.state = info.wp[d] ? NSControlStateValueOn : NSControlStateValueOff;
        drive.submenu = sub;
        [menu addItem:drive];
    }
}

/* Connect / Disconnect: a peripheral, so the machine restarts with it (or
 * without it; the disks stay in the settings for next time). */
- (void)pmd32Connect:(NSMenuItem *)sender
{
    const int on = sender.tag != 0;
    pmd85session_set_peripheral(_session, "pmd32", on);
    [self toast:on ? @"PMD 32 connected: the machine restarted with the disks in"
                   : @"PMD 32 disconnected: the machine restarted without it"];
}

- (void)diskInsert:(NSMenuItem *)sender
{
    const int d = (int)sender.tag;
    pmd85session_disk_info info;
    pmd85session_disk_get_info(_session, &info);
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = [NSString stringWithFormat:@"Insert a disk into drive %c", 'A' + d];
    panel.allowedContentTypes = typesForExtensions(@[ @"p32" ]);
    {   /* start where this drive's disk, or another one, came from */
        const char *near = info.path[d][0] ? info.path[d] : NULL;
        for (int k = 0; !near && k < 4; k++)
            if (info.path[k][0]) near = info.path[k];
        if (near)
            panel.directoryURL = [NSURL fileURLWithPath:[str(near) stringByDeletingLastPathComponent]];
    }
    [self openPanel:panel from:nil then:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        if (pmd85session_disk_insert(self->_session, d, panel.URL.fileSystemRepresentation,
                                     info.wp[d]) != 0) {
            [self toast:str(pmd85session_last_error(self->_session))];
            return;
        }
        NSString *base = panel.URL.lastPathComponent;
        [self toast:info.enabled
            ? [NSString stringWithFormat:@"Drive %c: %@", 'A' + d, base]
            : [NSString stringWithFormat:@"Drive %c: %@ -- the PMD 32 is not connected "
                                         @"(PMD 32 Disks ▸ Connect the PMD 32)", 'A' + d, base]];
    }];
}

- (void)diskEject:(NSMenuItem *)sender
{
    const int d = (int)sender.tag;
    if (pmd85session_disk_eject(_session, d) != 0)
        [self toast:str(pmd85session_last_error(_session))];
    else
        [self toast:[NSString stringWithFormat:@"Drive %c ejected", 'A' + d]];
}

- (void)diskProtect:(NSMenuItem *)sender
{
    const int d = (int)sender.tag;
    pmd85session_disk_info info;
    pmd85session_disk_get_info(_session, &info);
    if (pmd85session_disk_protect(_session, d, !info.wp[d]) != 0)
        [self toast:str(pmd85session_last_error(_session))];
}

/* ---- the tape deck ------------------------------------------------------------ */

- (void)openTapeAppend:(BOOL)append from:(NSWindow *)parent
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = append ? @"Append a tape's files to the tape in the deck"
                           : @"Load a PMD 85 tape (.ptp, .pmd)";
    panel.allowedContentTypes = typesForExtensions(@[ @"ptp", @"pmd" ]);
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (t.path[0])
        panel.directoryURL = [NSURL fileURLWithPath:[str(t.path) stringByDeletingLastPathComponent]];
    [self openPanel:panel from:parent then:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        pmd85session *s = self->_session;
        pmd85session_tape_info before;
        pmd85session_tape_get_info(s, &before);
        const char *path = panel.URL.fileSystemRepresentation;
        const int rc = append ? pmd85session_tape_append(s, path) : pmd85session_tape_load(s, path);
        if (rc < 0) {
            [self toast:str(pmd85session_last_error(s))];
            return;
        }
        pmd85session_tape_info after;
        pmd85session_tape_get_info(s, &after);
        NSString *base = panel.URL.lastPathComponent;
        const int added = after.blocks - before.blocks;
        if (rc > 0)
            [self toast:[NSString stringWithFormat:@"%@: %@", base, str(pmd85session_last_error(s))]];
        else if (append && before.blocks > 0)
            [self toast:[NSString stringWithFormat:@"%@: %d file%@ added to the tape", base, added,
                                                   added == 1 ? @"" : @"s"]];
        else
            [self toast:[NSString stringWithFormat:@"Tape: %@, %d file%@. Load it on the PMD "
                                                   @"(e.g. MGLD nn), then Play", base, after.blocks,
                                                   after.blocks == 1 ? @"" : @"s"]];
    }];
}

- (void)saveTapeFrom:(NSWindow *)parent
{
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.message = @"Save the tape (its recordings and changes too) as a .ptp file";
    panel.allowedContentTypes = typesForExtensions(@[ @"ptp", @"pmd" ]);
    panel.allowsOtherFileTypes = YES;
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (t.path[0]) {
        panel.directoryURL = [NSURL fileURLWithPath:[str(t.path) stringByDeletingLastPathComponent]];
        panel.nameFieldStringValue = str(t.path).lastPathComponent;
    } else {
        panel.nameFieldStringValue = @"tape.ptp";
    }
    [panel beginSheetModalForWindow:(parent ?: _window) completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        NSString *path = panel.URL.path;
        NSString *ext = path.pathExtension.lowercaseString;
        /* a tape without its extension gets .ptp, the format it is written in */
        if (![ext isEqualToString:@"ptp"] && ![ext isEqualToString:@"pmd"])
            path = [path stringByAppendingPathExtension:@"ptp"];
        if (pmd85session_tape_save(self->_session, path.fileSystemRepresentation) != 0)
            [self toast:str(pmd85session_last_error(self->_session))];
        else
            [self toast:[@"Tape saved as " stringByAppendingString:path.lastPathComponent]];
    }];
}

- (void)tapeResult:(int)rc done:(NSString *)done
{
    if (rc != 0)
        [self toast:str(pmd85session_last_error(_session))];
    else if (done)
        [self toast:done];
}

- (void)ejectTapeFrom:(NSWindow *)parent
{
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (!t.recorded) {
        [self tapeResult:pmd85session_tape_eject(_session) done:@"Tape ejected"];
        return;
    }
    /* what the PMD saved to tape is only in the deck so far */
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Eject the Tape?";
    a.informativeText = @"The tape has recordings or changes that are not saved to a file.";
    [a addButtonWithTitle:@"Cancel"];
    [a addButtonWithTitle:@"Save As…"];
    NSButton *eject = [a addButtonWithTitle:@"Eject"];
    eject.hasDestructiveAction = YES;
    NSWindow *on = parent ?: _window;
    [a beginSheetModalForWindow:on completionHandler:^(NSModalResponse r) {
        if (r == NSAlertSecondButtonReturn)
            dispatch_async(dispatch_get_main_queue(), ^{ [self saveTapeFrom:on]; });
        else if (r == NSAlertThirdButtonReturn)
            [self tapeResult:pmd85session_tape_eject(self->_session) done:@"Tape ejected"];
    }];
}

- (int)tapeFile:(int)delta
{
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (t.blocks <= 0) return -1;
    pmd85session_tape_block *b = calloc((size_t)t.blocks, sizeof *b);
    if (!b) return -1;
    int n = pmd85session_tape_blocks(_session, b, t.blocks, NULL), to = -1;
    if (n > t.blocks) n = t.blocks;
    for (int i = t.current + delta; i >= 0 && i < n; i += delta)
        if (b[i].number >= 0) {
            to = i;
            break;
        }
    free(b);
    return to;
}

- (int)tapeStep:(int)delta
{
    const int to = [self tapeFile:delta];
    if (to < 0) return -1;
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (t.playing) pmd85session_tape_stop(_session);
    return pmd85session_tape_seek(_session, to);
}

- (void)tapeStepBy:(int)delta
{
    if ([self tapeStep:delta] != 0) return;
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    [self toast:[NSString stringWithFormat:@"Tape at %@ (%d of %d)", str(t.block), t.current + 1,
                                           t.blocks]];
}

- (void)tapeWindow:(id)sender
{
    (void)sender;
    if (!_tape) _tape = [[PmdTapeWindow alloc] initWithSession:_session app:self];
    [_tape show];
}

- (void)tapeLoad:(id)sender { (void)sender; [self openTapeAppend:NO from:nil]; }
- (void)tapeAppend:(id)sender { (void)sender; [self openTapeAppend:YES from:nil]; }
- (void)tapeSave:(id)sender { (void)sender; [self saveTapeFrom:nil]; }
- (void)tapeEject:(id)sender { (void)sender; [self ejectTapeFrom:nil]; }
- (void)tapePrev:(id)sender { (void)sender; [self tapeStepBy:-1]; }
- (void)tapeNext:(id)sender { (void)sender; [self tapeStepBy:1]; }

- (void)tapePlayStop:(id)sender
{
    (void)sender;
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    [self tapeResult:(t.playing ? pmd85session_tape_stop(_session) : pmd85session_tape_play(_session))
                done:nil];
}

- (void)tapeRewind:(id)sender
{
    (void)sender;
    [self tapeResult:pmd85session_tape_rewind(_session) done:@"Tape rewound"];
}

/* ---- FujiNet -------------------------------------------------------------------- */

/* The web UI in the system browser (as in the Atari 800 app): its Google
 * Drive and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
- (void)fujinetWebUI:(id)sender
{
    (void)sender;
    if (pmd85session_fujinet_external(_session)) {
        [self toast:@"FujiNet is external: use its own web UI"];
        return;
    }
    if (!pmd85session_fujinet_running(_session)) {
        [self toast:@"The built-in FujiNet is not running"];
        return;
    }
    NSURL *url = [NSURL URLWithString:str(pmd85session_fujinet_webui_url(_session))];
    if (url) [[NSWorkspace sharedWorkspace] openURL:url];
}

/* The restart waits for FujiNet's listener (a second or so): off the main
 * thread. */
- (void)fujinetRestart:(id)sender
{
    (void)sender;
    if (pmd85session_fujinet_external(_session)) {
        [self toast:@"FujiNet is external: restart it where it runs"];
        return;
    }
    if (!pmd85session_get_int(_session, "enable_fujinet", 1)) {
        [self toast:@"The built-in FujiNet is off (Settings)"];
        return;
    }
    _fujiRestarting = YES;
    [self toast:@"Restarting FujiNet…"];
    pmd85session *s = _session;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const int rc = pmd85session_fujinet_restart(s);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_fujiRestarting = NO;
            if (self->_quitting) return;
            [self toast:rc == 0 ? @"FujiNet restarted" : str(pmd85session_last_error(s))];
        });
    });
}

- (void)importSD:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Import ROM modules (.rmm, .mrm) to FujiNet's SD card";
    panel.allowedContentTypes = typesForExtensions(@[ @"rmm", @"mrm" ]);   /* all FujiNet serves a PMD */
    panel.allowsMultipleSelection = YES;
    [self openPanel:panel from:nil then:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        int ok = 0, failed = 0;
        NSString *last = @"";
        for (NSURL *u in panel.URLs) {
            char dest[1024];
            if (pmd85session_import_media(self->_session, u.fileSystemRepresentation, dest,
                                          sizeof dest) == 0) {
                last = str(dest).lastPathComponent;
                ok++;
            } else {
                failed++;
            }
        }
        if (failed && !ok)
            [self toast:str(pmd85session_last_error(self->_session))];
        else if (failed)
            [self toast:[NSString stringWithFormat:@"%d copied to FujiNet's SD card, %d could not be",
                                                   ok, failed]];
        else if (ok == 1)
            [self toast:[NSString stringWithFormat:@"Copied %@ to FujiNet's SD card: mount it from "
                                                   @"CONFIG", last]];
        else if (ok > 1)
            [self toast:[NSString stringWithFormat:@"%d copied to FujiNet's SD card: mount them "
                                                   @"from CONFIG", ok]];
    }];
}

- (void)openSDFolder:(id)sender
{
    (void)sender;
    const char *path = pmd85session_sd_path(_session);
    if (!path || !*path) {
        [self toast:@"No FujiNet SD folder"];
        return;
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:str(path)
                              withIntermediateDirectories:YES attributes:nil error:NULL];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:str(path) isDirectory:YES]];
}

/* ---- the console log --------------------------------------------------------
 * Appended to as FujiNet writes (the Atari 800 app's scheme): the text is
 * never replaced, so the reader's selection and scroll position stay put and
 * only a reader at the bottom follows. A FujiNet restart (a new log) clears
 * it. */

- (void)refreshLog
{
    static char buf[64 * 1024];
    const NSUInteger keep = 256 * 1024;
    for (int round = 0; round < 16; round++) {
        int reset = 0;
        const int n = pmd85session_fujinet_read_log(_session, &_logCursor, buf, sizeof buf, &reset);
        if (n <= 0 && !reset) return;
        NSScrollView *scroll = _logView.enclosingScrollView;
        BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                 NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
        NSTextStorage *store = _logView.textStorage;
        if (reset || _logPlaceholder) {
            [store replaceCharactersInRange:NSMakeRange(0, store.length) withString:@""];
            _logPlaceholder = NO;
            atEnd = YES;
        }
        NSString *add = n > 0 ? ([NSString stringWithUTF8String:buf] ?:
                                 [[NSString alloc] initWithBytes:buf length:(NSUInteger)n
                                                        encoding:NSISOLatin1StringEncoding])
                              : @"";
        [store appendAttributedString:[[NSAttributedString alloc] initWithString:add attributes:@{
            NSFontAttributeName: _logView.font, NSForegroundColorAttributeName: NSColor.textColor }]];
        if (store.length > keep) {
            const NSUInteger from = store.length - keep * 3 / 4;
            NSRange line = [store.string lineRangeForRange:NSMakeRange(from, 0)];
            [store deleteCharactersInRange:NSMakeRange(0, NSMaxRange(line))];
        }
        if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
        if (n < (int)sizeof buf - 1) return;
    }
}

- (void)fujinetLog:(id)sender
{
    (void)sender;
    if (pmd85session_fujinet_external(_session)) {
        [self toast:@"FujiNet is external: see its own console"];
        return;
    }
    if (!_logWindow) {
        NSRect frame = NSMakeRect(0, 0, 860, 600);
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
        scroll.hasVerticalScroller = YES;
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
        _logView.editable = NO;
        _logView.richText = NO;
        _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _logView.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = _logView;
        memset(&_logCursor, 0, sizeof _logCursor);
        _logView.string = @"(no FujiNet output yet)";
        _logPlaceholder = YES;
        _logWindow = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _logWindow.title = @"FujiNet Console Log";
        _logWindow.releasedWhenClosed = NO;
        _logWindow.delegate = self;
        _logWindow.contentView = scroll;
        [_logWindow center];
    }
    [self refreshLog];
    if (!_logTimer) {
        __weak PmdAppDelegate *weak = self;
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weak refreshLog];
        }];
        _logTimer.tolerance = 0.2;
    }
    [_logWindow makeKeyAndOrderFront:nil];
}

/* ---- the other windows -------------------------------------------------------- */

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) _settings = [[PmdSettingsWindow alloc] initWithSession:_session app:self];
    [_settings show];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSAttributedString *credits = [[NSAttributedString alloc]
        initWithString:@"Self-contained Tesla PMD 85 with built-in FujiNet.\n"
                       @"Emulation by GPMD85Emulator (Roman and Martin Borík).\n"
                       @"https://fujinet.online/"
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                          NSForegroundColorAttributeName: NSColor.labelColor }];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName: @"FujiNet Go PMD 85",
        NSAboutPanelOptionCredits: credits,
    }];
}

- (void)openWebsite:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://fujinet.online/"]];
}

/* ---- the menu bar ------------------------------------------------------------- */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:sel keyEquivalent:key];
    item.target = self;
    return item;
}

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
           modifiers:(NSEventModifierFlags)mods
{
    NSMenuItem *item = [self item:title action:sel key:key];
    item.keyEquivalentModifierMask = mods;
    return item;
}

- (NSMenuItem *)submenu:(NSString *)title of:(NSMenu *)parent
{
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
    it.submenu = [[NSMenu alloc] initWithTitle:title];
    [parent addItem:it];
    return it;
}

/* items for each name of a session table, tagged with its index */
- (void)radio:(NSMenu *)menu action:(SEL)action names:(const char *(*)(int))nameAt
{
    const char *label;
    for (int i = 0; (label = nameAt(i)) != NULL; i++) {
        NSMenuItem *it = [self item:str(label) action:action key:@""];
        it.tag = i;
        [menu addItem:it];
    }
}

- (NSMenu *)topMenu:(NSString *)title in:(NSMenu *)bar
{
    NSMenuItem *it = [[NSMenuItem alloc] init];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    it.submenu = m;
    [bar addItem:it];
    return m;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go PMD 85";
    NSMenu *bar = [[NSMenu alloc] init];
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    const NSEventModifierFlags cmdShift = NSEventModifierFlagCommand | NSEventModifierFlagShift;

    /* The application menu: a menu bar built in code gets none of the
     * standard items for free. */
    NSMenu *app = [self topMenu:appName in:bar];
    [app addItem:[self item:[@"About " stringByAppendingString:appName]
                     action:@selector(showAbout:) key:@""]];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:[self item:@"Settings…" action:@selector(showSettings:) key:@","]];
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *services = [app addItemWithTitle:@"Services" action:NULL keyEquivalent:@""];
    services.submenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu = services.submenu;
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                   action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [app addItemWithTitle:@"Hide Others"
                                        action:@selector(hideOtherApplications:)
                                 keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                   action:@selector(terminate:) keyEquivalent:@"q"];

    /* Machine */
    NSMenu *machine = [self topMenu:@"Machine" in:bar];
    [self radio:[self submenu:@"Model" of:machine].submenu action:@selector(selectModel:)
          names:pmd85session_model_name];
    {
        NSMenu *mod = [self submenu:@"ROM Module" of:machine].submenu;
        [self radio:mod action:@selector(selectModule:) names:pmd85session_module_name];
        [mod addItem:[NSMenuItem separatorItem]];
        [mod addItem:[self item:@"Choose Module File…" action:@selector(chooseModuleFile:)
                            key:@""]];
    }
    {
        NSMenuItem *disks = [self submenu:@"PMD 32 Disks" of:machine];
        _disksMenu = disks.submenu;
        _disksMenu.delegate = self;
    }
    [machine addItem:[NSMenuItem separatorItem]];
    [machine addItem:[self item:@"RESET" action:@selector(resetMachine:) key:@"r" modifiers:cmd]];
    [machine addItem:[self item:@"Cold Restart" action:@selector(coldRestart:) key:@"r"
                      modifiers:cmdShift]];

    /* Tape */
    NSMenu *tape = [self topMenu:@"Tape" in:bar];
    [tape addItem:[self item:@"Tape Window" action:@selector(tapeWindow:) key:@"t"]];
    [tape addItem:[NSMenuItem separatorItem]];
    [tape addItem:[self item:@"Load Tape…" action:@selector(tapeLoad:) key:@"o"]];
    [tape addItem:[self item:@"Append Tape…" action:@selector(tapeAppend:) key:@""]];
    [tape addItem:[self item:@"Save Tape As…" action:@selector(tapeSave:) key:@"s"]];
    [tape addItem:[NSMenuItem separatorItem]];
    [tape addItem:[self item:@"Play" action:@selector(tapePlayStop:) key:@"p"]];
    [tape addItem:[self item:@"Previous File" action:@selector(tapePrev:) key:@"["]];
    [tape addItem:[self item:@"Next File" action:@selector(tapeNext:) key:@"]"]];
    [tape addItem:[self item:@"Rewind" action:@selector(tapeRewind:) key:@""]];
    [tape addItem:[self item:@"Eject" action:@selector(tapeEject:) key:@""]];

    /* View */
    NSMenu *view = [self topMenu:@"View" in:bar];
    {
        NSMenu *colors = [self submenu:@"Colors" of:view].submenu;
        [self radio:colors action:@selector(selectColor:) names:pmd85session_color_name];
        [colors addItem:[NSMenuItem separatorItem]];
        [colors addItem:[self item:@"Next Colors" action:@selector(nextColor:) key:@"c"
                         modifiers:cmdShift]];
    }
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen"
                                     action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;

    /* FujiNet */
    NSMenu *fuji = [self topMenu:@"FujiNet" in:bar];
    [fuji addItem:[self item:@"FujiNet Web UI" action:@selector(fujinetWebUI:) key:@""]];
    [fuji addItem:[self item:@"Console Log" action:@selector(fujinetLog:) key:@"l"]];
    [fuji addItem:[self item:@"Restart FujiNet" action:@selector(fujinetRestart:) key:@""]];
    [fuji addItem:[NSMenuItem separatorItem]];
    [fuji addItem:[self item:@"Import to SD Card…" action:@selector(importSD:) key:@""]];
    [fuji addItem:[self item:@"Open SD Folder" action:@selector(openSDFolder:) key:@""]];

    /* Window: AppKit lists the open windows in whichever menu is the
     * windowsMenu. */
    NSMenu *win = [self topMenu:@"Window" in:bar];
    [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [win addItem:[NSMenuItem separatorItem]];
    [win addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
    NSApp.windowsMenu = win;

    NSMenu *help = [self topMenu:@"Help" in:bar];
    [help addItem:[self item:@"FujiNet Website" action:@selector(openWebsite:) key:@""]];
    NSApp.helpMenu = help;

    NSApp.mainMenu = bar;
}

/* Check marks, enabled items and the Play/Stop title, as the machine is. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = item.action;
    pmd85session *s = _session;
    if (!s || _quitting) return NO;

    if (a == @selector(selectModel:)) {
        item.state = item.tag == pmd85session_model(s) ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (a == @selector(selectModule:)) {
        item.state = item.tag == pmd85session_module(s) ? NSControlStateValueOn : NSControlStateValueOff;
        return pmd85session_module_supported(pmd85session_model(s), (int)item.tag) != 0;
    }
    if (a == @selector(selectColor:)) {
        item.state = item.tag == pmd85session_color_profile(s) ? NSControlStateValueOn
                                                               : NSControlStateValueOff;
        return YES;
    }
    if (a == @selector(fujinetRestart:))
        return !_fujiRestarting;
    if (a == @selector(tapePlayStop:) || a == @selector(tapeRewind:) || a == @selector(tapeEject:) ||
        a == @selector(tapeSave:) || a == @selector(tapePrev:) || a == @selector(tapeNext:)) {
        pmd85session_tape_info t;
        pmd85session_tape_get_info(s, &t);
        if (a == @selector(tapePlayStop:)) {
            item.title = t.playing ? @"Stop" : @"Play";
            return t.blocks > 0;
        }
        if (a == @selector(tapePrev:)) return [self tapeFile:-1] >= 0;
        if (a == @selector(tapeNext:)) return [self tapeFile:1] >= 0;
        return t.blocks > 0;
    }
    return YES;
}

/* ---- the window --------------------------------------------------------------- */

- (void)windowDidResignKey:(NSNotification *)note
{
    /* focus gone: nothing the PMD was given stays held */
    if (!_quitting && _session && note.object == _window)
        pmd85session_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    /* closing the main window quits, whatever else is open (as on GNOME) */
    if (note.object == _window && !_quitting)
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

- (void)windowDidEnterFullScreen:(NSNotification *)note
{
    if (note.object == _window) [_window makeFirstResponder:_display];
}

/* ---- lifecycle --------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    /* An emulator is latency-critical: no App Nap, no timer coalescing. */
    _activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                          reason:@"Emulating a Tesla PMD 85"];
    [self buildMenus];

    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 960, 760)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go PMD 85";
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.contentMinSize = NSMakeSize(288, 256);

    NSView *content = _window.contentView;
    _display = [[DisplayView alloc] initWithSession:_session];
    _display.frame = content.bounds;
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:_display];

    /* the toast: a label over the top of the picture */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.72];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.lineBreakMode = NSLineBreakByTruncatingTail;
    _toast.hidden = YES;
    _toast.frame = NSMakeRect(0, content.bounds.size.height - 30, content.bounds.size.width, 24);
    _toast.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_toast];

    /* the lights, at the right of the title bar */
    _lights = [[PmdLights alloc] initWithFrame:NSMakeRect(0, 0, 76, 22)];
    _lights.disk = -1;
    _lights.toolTip = @"The PMD 85 keyboard LEDs";
    NSTitlebarAccessoryViewController *acc = [[NSTitlebarAccessoryViewController alloc] init];
    acc.view = _lights;
    acc.layoutAttribute = NSLayoutAttributeTrailing;
    [_window addTitlebarAccessoryViewController:acc];

    [self applyDisplaySettings];
    _lastRomLoads = pmd85session_rom_loads(_session, NULL);
    _padEvent = pmd85session_gamepad_last_event(_session, NULL, 0);

    if (![_window setFrameUsingName:@"PMD85MainWindow"]) [_window center];
    [_window setFrameAutosaveName:@"PMD85MainWindow"];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    __weak PmdAppDelegate *weak = self;
    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weak statusTick];
    }];
    _statusTimer.tolerance = 0.1;
    [self statusTick];
    [self showStartWarnings];

    /* developer affordances, as the GNOME app's */
    if (getenv("PMD85_OPEN_SETTINGS")) [self showSettings:nil];
    if (getenv("PMD85_OPEN_TAPE")) [self tapeWindow:nil];
    if (getenv("PMD85_OPEN_LOG")) [self fujinetLog:nil];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    _quitting = YES;
    /* close the windows while the session is alive: their handlers use it */
    _window.delegate = nil;
    for (NSWindow *w in [[NSApp windows] copy])
        if (w != _window) [w close];
    [_statusTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    if (_activity) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    pmd85session_settings_flush(_session);
    pmd85session_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
    (void)app;
    return YES;
}

@end
