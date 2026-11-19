/*
 * The Settings window. Three kinds of option, by when they take effect (as
 * in the GNOME app's Preferences):
 *
 *   display    how the picture is drawn (aspect, scaling) -- at once;
 *   machine    model, ROM module, peripherals -- the machine is rebuilt and
 *              cold-started at once (FujiNet stays up, the link reconnects);
 *              the colors apply without a restart;
 *   session    FujiNet and sound decide what the session starts, so they
 *              apply when the window closes, by restarting it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#import "AppDelegate.h"
#import "DisplayView.h"

#include <string.h>

/* what a checkbox's change does (its tag) */
enum {
    KIND_PERIPHERAL = 0,  /* stored, the machine rebuilt */
    KIND_KEYBOARD,        /* stored, a cold restart */
    KIND_SESSION,         /* stored, applied on close */
    KIND_PLAIN,           /* stored, read where it is used */
    KIND_TAPE,            /* the tape options */
    KIND_INPUT,           /* a peripheral, and the pads' rows follow */
};

#define PAD_ROWS_MAX 8

static NSString *str(const char *s)
{
    return s ? ([NSString stringWithUTF8String:s] ?: @"") : @"";
}

@implementation PmdSettingsWindow {
    pmd85session *_session;
    __weak PmdAppDelegate *_app;
    NSWindow *_window;
    BOOL _sessionDirty;

    NSPopUpButton *_model, *_module, *_colors;
    NSTextField *_moduleFile;
    NSTextField *_addr;
    NSStackView *_pads;
    unsigned _padGen;
    NSTimer *_padTimer;
}

- (instancetype)initWithSession:(pmd85session *)session app:(PmdAppDelegate *)app
{
    self = [super init];
    if (self) {
        _session = session;
        _app = app;
    }
    return self;
}

/* ---- controls ------------------------------------------------------------------ */

- (NSTextField *)note:(NSString *)text
{
    NSTextField *l = [NSTextField wrappingLabelWithString:text];
    l.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    l.textColor = NSColor.secondaryLabelColor;
    l.preferredMaxLayoutWidth = 380;
    [l.widthAnchor constraintLessThanOrEqualToConstant:380].active = YES;
    return l;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    l.identifier = @"pmd-heading";
    return l;
}

- (NSButton *)check:(NSString *)title key:(const char *)key def:(int)def kind:(int)kind
{
    NSButton *b = [NSButton checkboxWithTitle:title target:self action:@selector(checkChanged:)];
    b.state = pmd85session_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    b.identifier = str(key);
    b.tag = kind;
    return b;
}

- (NSPopUpButton *)popup:(const char *(*)(int))nameAt selected:(int)sel action:(SEL)action
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    const char *label;
    for (int i = 0; (label = nameAt(i)) != NULL; i++) [p addItemWithTitle:str(label)];
    if (sel >= 0 && sel < p.numberOfItems) [p selectItemAtIndex:sel];
    p.target = self;
    p.action = action;
    return p;
}

static const char *aspect_name(int i)
{
    static const char *const n[] = { "TV (4:3)", "Square pixels", "Integer scale" };
    return i >= 0 && i < 3 ? n[i] : NULL;
}

static const char *scaling_name(int i)
{
    static const char *const n[] = { "Sharp", "Nearest", "Smooth" };
    return i >= 0 && i < 3 ? n[i] : NULL;
}

static const char *pad_choice(int i)
{
    static const char *const n[] = { "Automatic", "Joystick 1", "Joystick 2" };
    return i >= 0 && i < 3 ? n[i] : NULL;
}

/* A page: rows of [label, control], a control alone, or a note under the
 * control before it. */
- (NSView *)page:(NSArray<NSArray<NSView *> *> *)rows
{
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger i = 0; i < grid.numberOfRows; i++) {
        NSGridRow *row = [grid rowAtIndex:i];
        NSView *first = [row cellAtIndex:0].contentView;
        /* a heading spans both columns, flush left, with room above it */
        if ([first.identifier isEqualToString:@"pmd-heading"]) {
            [row mergeCellsInRange:NSMakeRange(0, 2)];
            [row cellAtIndex:0].xPlacement = NSGridCellPlacementLeading;
            if (i > 0) row.topPadding = 10;
        }
    }
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:grid];
    [grid.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:20].active = YES;
    [grid.trailingAnchor constraintLessThanOrEqualToAnchor:holder.trailingAnchor constant:-20].active = YES;
    [grid.topAnchor constraintEqualToAnchor:holder.topAnchor constant:16].active = YES;
    [grid.bottomAnchor constraintLessThanOrEqualToAnchor:holder.bottomAnchor constant:-16].active = YES;
    return holder;
}

static NSView *blank(void)
{
    return [NSGridCell emptyContentView];
}

/* ---- pages ---------------------------------------------------------------------- */

- (NSView *)machinePage
{
    pmd85session *s = _session;
    _model = [self popup:pmd85session_model_name selected:pmd85session_model(s)
                  action:@selector(modelChanged:)];
    _module = [self popup:pmd85session_module_name selected:pmd85session_module(s)
                   action:@selector(moduleChanged:)];
    _moduleFile = [NSTextField labelWithString:@""];
    _moduleFile.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_moduleFile.widthAnchor constraintLessThanOrEqualToConstant:260].active = YES;
    NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self
                                          action:@selector(chooseModuleFile:)];
    NSStackView *file = [NSStackView stackViewWithViews:@[ _moduleFile, choose ]];
    file.spacing = 8;
    [self refreshModule];

    return [self page:@[
        @[ [self heading:@"Machine"], blank() ],
        @[ blank(), [self note:@"Changing these restarts the PMD (FujiNet keeps running)."] ],
        @[ [NSTextField labelWithString:@"Model:"], _model ],
        @[ [NSTextField labelWithString:@"ROM module:"], _module ],
        @[ blank(), [self note:@"FujiNet boots FujiNet CONFIG on a PMD 85-2 or 85-2A; other "
                               @"models start it from the monitor."] ],
        @[ [NSTextField labelWithString:@"Module file:"], file ],
        @[ blank(), [self check:@"256 KB RAM expansion (PMD 85-2A and 85-3)" key:"ram256k" def:0
                           kind:KIND_PERIPHERAL] ],
        @[ blank(), [self check:@"PMD 85-2 compatibility (PMD 85-3)" key:"compat_mode" def:0
                           kind:KIND_PERIPHERAL] ],
        @[ blank(), [self note:@"The 85-3 starts in the 85-2 mode, which auto-runs the ROM module."] ],
        @[ [self heading:@"Peripherals"], blank() ],
        @[ blank(), [self check:@"MIF 85 music interface" key:"mif85" def:0 kind:KIND_PERIPHERAL] ],
        @[ blank(), [self note:@"SAA1099 sound on ports ECh-EFh."] ],
        @[ blank(), [self check:@"PMD 32 disk drive" key:"pmd32" def:0 kind:KIND_INPUT] ],
        @[ blank(), [self note:@"On the GPIO port (takes joystick port A). Its disks are in "
                               @"Machine ▸ PMD 32 Disks."] ],
        @[ [self heading:@"Tape"], blank() ],
        @[ blank(), [self check:@"Flash load" key:"tape_flash" def:1 kind:KIND_TAPE] ],
        @[ blank(), [self note:@"A program's body goes into memory at once; off loads in real "
                               @"time at 1200 Bd."] ],
        @[ blank(), [self check:@"Tape sound" key:"tape_sound" def:0 kind:KIND_TAPE] ],
        @[ [self heading:@"Keyboard"], blank() ],
        @[ blank(), [self check:@"Swap Z and Y (QWERTZ keyboards)" key:"exchange_zy" def:1
                           kind:KIND_KEYBOARD] ],
        @[ blank(), [self check:@"Numeric keypad types digits and operators" key:"numpad" def:1
                           kind:KIND_KEYBOARD] ],
        @[ blank(), [self note:@"F1–F12 are K0–K11 (FujiNet CONFIG starts with K10 = F11: "
                               @"if the Mac takes F11 to show the desktop, turn that off in "
                               @"System Settings ▸ Keyboard ▸ Keyboard Shortcuts, or press "
                               @"Fn+F11). Esc and Control are STOP, Tab is C-D, ` is WRK, "
                               @"Page Up/Down are RCL/CLR. The app's keys are Command ones: "
                               @"Cmd+R RESET, Cmd+Shift+R cold restart, Cmd+Shift+C colors, "
                               @"Cmd+T the tape, Cmd+P play/stop, Option+F12 or Cmd+D the debugger."] ],
    ]];
}

- (NSView *)displayPage
{
    pmd85session *s = _session;
    _colors = [self popup:pmd85session_color_name selected:pmd85session_color_profile(s)
                   action:@selector(colorsChanged:)];
    NSPopUpButton *aspect = [self popup:aspect_name
                               selected:pmd85session_get_int(s, "aspect_mode", PMD85SESSION_ASPECT_TV)
                                 action:@selector(aspectChanged:)];
    NSPopUpButton *scaling = [self popup:scaling_name
                                selected:pmd85session_get_int(s, "scaling", PMD_SCALING_SHARP)
                                  action:@selector(scalingChanged:)];
    return [self page:@[
        @[ [self heading:@"Picture"], blank() ],
        @[ [NSTextField labelWithString:@"Colors:"], _colors ],
        @[ blank(), [self note:@"Black & White is the TV picture (white, half bright, blinking), "
                               @"Green the same on a green monitor; RGBM and ColorACE are the "
                               @"color modifications (Cmd+Shift+C)."] ],
        @[ [NSTextField labelWithString:@"Aspect:"], aspect ],
        @[ [NSTextField labelWithString:@"Scaling:"], scaling ],
        @[ blank(), [self note:@"Sharp: even, crisp pixels at any size. Nearest: crisp, uneven. "
                               @"Smooth: soft. (Integer scale is always crisp.)"] ],
    ]];
}

- (NSView *)inputPage
{
    _pads = [NSStackView stackViewWithViews:@[]];
    _pads.orientation = NSUserInterfaceLayoutOrientationVertical;
    _pads.alignment = NSLayoutAttributeLeading;
    _pads.spacing = 6;
    [self fillPads];
    return [self page:@[
        @[ [self heading:@"Gamepads"], blank() ],
        @[ blank(), [self check:@"Joystick 4004/482" key:"joystick" def:1 kind:KIND_INPUT] ],
        @[ blank(), [self note:@"The interface on the GPIO ports (the machine restarts). Each "
                               @"gamepad drives a joystick: the D-pad steers, A, B, the shoulders "
                               @"and the triggers fire. Joystick 1 is GPIO port A (not while the "
                               @"PMD 32 is connected), joystick 2 port B."] ],
        @[ [NSTextField labelWithString:@"Pads:"], _pads ],
        @[ blank(), [self check:@"Left stick steers" key:"analog_stick" def:1 kind:KIND_PLAIN] ],
        @[ blank(), [self note:@"The analog stick works as the joystick too, besides the D-pad."] ],
        @[ [self heading:@"Mouse 602"], blank() ],
        @[ blank(), [self check:@"Mouse 602 on port 8Ch" key:"mouse602" def:0 kind:KIND_INPUT] ],
        @[ blank(), [self note:@"The pointer over the PMD's screen moves the mouse and its buttons "
                               @"are the mouse's (the middle one presses both). PMD 85-1, -2, "
                               @"-2A, -3 and Consul 2717; the machine restarts."] ],
        @[ blank(), [self check:@"Hide the pointer over the screen" key:"mouse_hide_pointer" def:1
                           kind:KIND_PLAIN] ],
    ]];
}

- (NSView *)fujinetPage
{
    pmd85session *s = _session;
    _addr = [NSTextField textFieldWithString:str(pmd85session_get_str(s, "fujinet_external_addr", ""))];
    _addr.placeholderString = @"127.0.0.1:65505";
    [_addr.widthAnchor constraintEqualToConstant:220].active = YES;
    _addr.enabled = pmd85session_get_int(s, "fujinet_external", 0) != 0;
    _addr.delegate = self;
    return [self page:@[
        @[ [self heading:@"FujiNet"], blank() ],
        @[ blank(), [self note:[NSString stringWithFormat:
            @"The PMD talks DriveWire to FujiNet through a Becker port, now %@. These apply when "
            @"this window closes.", str(pmd85session_becker_endpoint(s))]] ],
        @[ blank(), [self check:@"Built-in FujiNet" key:"enable_fujinet" def:1 kind:KIND_SESSION] ],
        @[ blank(), [self check:@"Use an external FujiNet" key:"fujinet_external" def:0
                           kind:KIND_SESSION] ],
        @[ blank(), [self note:@"A FujiNet running elsewhere, e.g. fujinet-pc on another machine."] ],
        @[ [NSTextField labelWithString:@"Address:"], _addr ],
        @[ [self heading:@"Sound"], blank() ],
        @[ blank(), [self check:@"Sound" key:"enable_audio" def:1 kind:KIND_SESSION] ],
    ]];
}

/* ---- the window ------------------------------------------------------------------ */

- (void)build
{
    NSArray<NSArray *> *pages = @[
        @[ @"Machine", [self machinePage] ],
        @[ @"Display", [self displayPage] ],
        @[ @"Input", [self inputPage] ],
        @[ @"FujiNet", [self fujinetPage] ],
    ];
    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    CGFloat high = 0;
    for (NSArray *p in pages) {
        NSView *holder = p[1];
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:p[0]];
        item.label = p[0];
        item.view = holder;
        [tabs addTabViewItem:item];
        [holder layoutSubtreeIfNeeded];
        high = MAX(high, ((NSView *)holder.subviews.firstObject).fittingSize.height);
    }
    const NSSize size = NSMakeSize(560, MIN(high + 90, 760));
    tabs.frame = NSMakeRect(0, 0, size.width, size.height);
    tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Settings";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    _window.contentView = tabs;
    [_window center];
    /* PMD85_PREFS_PAGE=input: open on that page (development, as on GNOME) */
    const char *pg = getenv("PMD85_PREFS_PAGE");
    if (pg && *pg) {
        for (NSTabViewItem *it in tabs.tabViewItems)
            if ([((NSString *)it.identifier).lowercaseString isEqualToString:str(pg).lowercaseString])
                [tabs selectTabViewItem:it];
    }
}

- (void)show
{
    if (!_window) [self build];
    [self sync];
    if (!_padTimer) {
        __weak PmdSettingsWindow *weak = self;
        _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
            (void)t;
            PmdSettingsWindow *me = weak;
            if (me && pmd85session_gamepad_generation(me->_session) != me->_padGen) [me fillPads];
        }];
        _padTimer.tolerance = 0.25;
    }
    [_window makeKeyAndOrderFront:nil];
}

/* What the menu may have changed meanwhile. */
- (void)sync
{
    [_model selectItemAtIndex:pmd85session_model(_session)];
    [_colors selectItemAtIndex:pmd85session_color_profile(_session)];
    [self refreshModule];
    for (NSView *v in [self allControls:_window.contentView])
        if ([v isKindOfClass:[NSButton class]] && ((NSButton *)v).identifier.length &&
            ((NSButton *)v).action == @selector(checkChanged:)) {
            NSButton *b = (NSButton *)v;
            const int on = pmd85session_get_int(_session, b.identifier.UTF8String,
                                                b.state == NSControlStateValueOn);
            b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
        }
}

- (NSArray<NSView *> *)allControls:(NSView *)root
{
    NSMutableArray<NSView *> *out = [NSMutableArray array];
    if ([root isKindOfClass:[NSTabView class]]) {
        for (NSTabViewItem *it in ((NSTabView *)root).tabViewItems)
            [out addObjectsFromArray:[self allControls:it.view]];
        return out;
    }
    for (NSView *v in root.subviews) {
        [out addObject:v];
        [out addObjectsFromArray:[self allControls:v]];
    }
    return out;
}

- (void)windowDidBecomeKey:(NSNotification *)note
{
    (void)note;
    [self sync];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [_window makeFirstResponder:nil];   /* commit a field edit */
    [_padTimer invalidate];
    _padTimer = nil;
    pmd85session_settings_flush(_session);
    if (_sessionDirty) {
        _sessionDirty = NO;
        [_app restartSession];
    }
}

/* ---- handlers ---------------------------------------------------------------------- */

- (void)checkChanged:(NSButton *)b
{
    const char *key = b.identifier.UTF8String;
    const int on = b.state == NSControlStateValueOn;
    switch (b.tag) {
    case KIND_PERIPHERAL:
        pmd85session_set_peripheral(_session, key, on);
        break;
    case KIND_INPUT:
        pmd85session_set_peripheral(_session, key, on);
        [self fillPads];
        break;
    case KIND_KEYBOARD:
        pmd85session_set_int(_session, key, on);
        pmd85session_hard_reset(_session);
        break;
    case KIND_SESSION:
        pmd85session_set_int(_session, key, on);
        _sessionDirty = YES;
        if (!strcmp(key, "fujinet_external")) _addr.enabled = on;
        break;
    case KIND_TAPE: {
        int flash = pmd85session_get_int(_session, "tape_flash", 1);
        int sound = pmd85session_get_int(_session, "tape_sound", 0);
        if (!strcmp(key, "tape_flash")) flash = on;
        else sound = on;
        pmd85session_set_tape_options(_session, flash, sound);
        break;
    }
    default:
        pmd85session_set_int(_session, key, on);
        break;
    }
}

- (void)modelChanged:(NSPopUpButton *)p
{
    if ((int)p.indexOfSelectedItem != pmd85session_model(_session))
        pmd85session_set_model(_session, (int)p.indexOfSelectedItem);
}

- (void)refreshModule
{
    const char *path = pmd85session_module_file(_session);
    [_module selectItemAtIndex:pmd85session_module(_session)];
    _moduleFile.stringValue = !(path && *path) ? @"None chosen: .rmm or .mrm"
                            : pmd85session_module_file_ok(_session) ? str(path).lastPathComponent
                            : @"Missing: choose it again";
    _moduleFile.toolTip = path && *path ? str(path) : nil;
}

- (void)chooseModuleFile:(id)sender
{
    (void)sender;
    __weak PmdSettingsWindow *weak = self;
    [_app chooseModuleFileFrom:_window done:^(BOOL chosen) {
        (void)chosen;
        [weak refreshModule];
    }];
}

- (void)moduleChanged:(NSPopUpButton *)p
{
    const int m = (int)p.indexOfSelectedItem;
    if (m == pmd85session_module(_session)) return;
    if (m == PMD85SESSION_MODULE_FILE && !pmd85session_module_file_ok(_session)) {
        [self chooseModuleFile:nil];   /* switches, or puts the popup back */
        return;
    }
    pmd85session_set_module(_session, m);
    [self refreshModule];
}

- (void)colorsChanged:(NSPopUpButton *)p
{
    pmd85session_set_color_profile(_session, (int)p.indexOfSelectedItem);
}

- (void)aspectChanged:(NSPopUpButton *)p
{
    pmd85session_set_int(_session, "aspect_mode", (int)p.indexOfSelectedItem);
    [_app applyDisplaySettings];
}

- (void)scalingChanged:(NSPopUpButton *)p
{
    pmd85session_set_int(_session, "scaling", (int)p.indexOfSelectedItem);
    [_app applyDisplaySettings];
}

/* The external FujiNet's address: valid text (or none, meaning the default)
 * is stored; anything else is shown in red and not. */
- (void)controlTextDidChange:(NSNotification *)note
{
    if (note.object != _addr) return;
    NSString *text = [_addr.stringValue stringByTrimmingCharactersInSet:
                                            NSCharacterSet.whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (pmd85session_parse_hostport(text.UTF8String, host, sizeof host, &port) != 0) {
        _addr.textColor = NSColor.systemRedColor;
        return;
    }
    _addr.textColor = NSColor.controlTextColor;
    if (!strcmp(pmd85session_get_str(_session, "fujinet_external_addr", ""), text.UTF8String))
        return;
    pmd85session_set_str(_session, "fujinet_external_addr", text.UTF8String);
    if (pmd85session_get_int(_session, "fujinet_external", 0)) _sessionDirty = YES;
}

/* ---- the gamepads ---------------------------------------------------------------- */

- (void)fillPads
{
    for (NSView *v in [_pads.arrangedSubviews copy]) {
        [_pads removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    _padGen = pmd85session_gamepad_generation(_session);
    const int n = MIN(pmd85session_gamepad_count(_session), PAD_ROWS_MAX);
    const unsigned avail = pmd85session_joysticks(_session);
    if (n <= 0) {
        NSTextField *none = [NSTextField labelWithString:
            @"No gamepads connected. Plug one in: it takes the first free joystick."];
        none.textColor = NSColor.secondaryLabelColor;
        [_pads addArrangedSubview:none];
        return;
    }
    for (int i = 0; i < n; i++) {
        char name[96];
        name[0] = 0;
        pmd85session_gamepad_name(_session, i, name, sizeof name);
        NSTextField *label = [NSTextField labelWithString:str(name)];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.toolTip = label.stringValue;
        [label.widthAnchor constraintLessThanOrEqualToConstant:170].active = YES;
        NSPopUpButton *p = [self popup:pad_choice
                              selected:pmd85session_gamepad_assignment(_session, i) + 1
                                action:@selector(padChanged:)];
        p.tag = i;
        const int port = pmd85session_gamepad_effective_port(_session, i);
        NSTextField *eff = [NSTextField labelWithString:
            port >= 0 ? [NSString stringWithFormat:@"drives joystick %d", port + 1]
                      : (avail ? @"not used: no joystick free" : @"not used: the interface is off")];
        eff.textColor = NSColor.secondaryLabelColor;
        NSStackView *row = [NSStackView stackViewWithViews:@[ label, p, eff ]];
        row.spacing = 8;
        [_pads addArrangedSubview:row];
    }
}

- (void)padChanged:(NSPopUpButton *)p
{
    /* Automatic = -1, Joystick N = N - 1 */
    pmd85session_gamepad_assign(_session, (int)p.tag, (int)p.indexOfSelectedItem - 1);
    [self fillPads];
}

@end
