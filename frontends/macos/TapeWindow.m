/*
 * The Tape window. The deck is the emulator thread's: everything here goes
 * through the session, and the view follows the deck's published state
 * (100 ms). Double-click (or Return) on a file moves the tape's position
 * there; the context menu also sets the STOP marker, deletes and moves
 * files (Delete, Option+Up/Down).
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "TapeWindow.h"

#import "AppDelegate.h"

#include <stdlib.h>
#include <string.h>

static NSString *str(const char *s)
{
    return s ? ([NSString stringWithUTF8String:s] ?: @"") : @"";
}

static NSImage *symbol(NSString *name, NSString *desc)
{
    NSImage *img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:desc];
    return img ?: [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];   /* never nil */
}

/* The table: Return seeks, Delete deletes, Option+arrows move. */
@interface PmdTapeTable : NSTableView
@property (nonatomic, weak) id keyTarget;
@end

@implementation PmdTapeTable

- (void)keyDown:(NSEvent *)e
{
    NSString *c = e.charactersIgnoringModifiers;
    const unichar ch = c.length ? [c characterAtIndex:0] : 0;
    const BOOL opt = (e.modifierFlags & NSEventModifierFlagOption) != 0;
    SEL sel = NULL;
    if (ch == 0x0D || ch == 0x03) sel = @selector(seek:);
    else if (ch == 0x7F || ch == NSDeleteFunctionKey) sel = @selector(deleteBlocks:);
    else if (opt && ch == NSUpArrowFunctionKey) sel = @selector(moveUp:);
    else if (opt && ch == NSDownArrowFunctionKey) sel = @selector(moveDown:);
    if (sel && [_keyTarget respondsToSelector:sel]) {
        [NSApp sendAction:sel to:_keyTarget from:self];
        return;
    }
    [super keyDown:e];
}

@end

@implementation PmdTapeWindow {
    pmd85session *_session;
    __weak PmdAppDelegate *_app;
    NSWindow *_window;
    PmdTapeTable *_table;
    NSScrollView *_scroll;
    NSView *_empty;
    NSButton *_play, *_rewind, *_prev, *_next, *_flash, *_sound, *_menuButton;
    NSProgressIndicator *_progress;
    NSTextField *_progressText;
    NSTimer *_timer;

    pmd85session_tape_block *_blocks;
    int _nblocks;
    BOOL _haveList;
    pmd85session_tape_info _info;
    BOOL _hex;
}

- (instancetype)initWithSession:(pmd85session *)session app:(PmdAppDelegate *)app
{
    self = [super init];
    if (self) {
        _session = session;
        _app = app;
        _hex = pmd85session_get_int(session, "tape_hex", 1) != 0;   /* as the monitor */
    }
    return self;
}

- (void)dealloc
{
    free(_blocks);
}

/* ---- building it ------------------------------------------------------------- */

- (NSButton *)button:(NSString *)sym tip:(NSString *)tip action:(SEL)action
{
    NSButton *b = [NSButton buttonWithImage:symbol(sym, tip) target:self action:action];
    b.bezelStyle = NSBezelStyleTexturedRounded;
    b.toolTip = tip;
    return b;
}

- (NSButton *)toggle:(NSString *)sym tip:(NSString *)tip action:(SEL)action
{
    NSButton *b = [self button:sym tip:tip action:action];
    [b setButtonType:NSButtonTypePushOnPushOff];
    return b;
}

- (void)addColumn:(NSString *)ident title:(NSString *)title width:(CGFloat)w
{
    NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ident];
    c.title = title;
    c.width = w;
    c.minWidth = w > 30 ? 40 : w;
    /* the File column takes what is left */
    c.resizingMask = [ident isEqualToString:@"name"] ? NSTableColumnAutoresizingMask
                                                     : NSTableColumnUserResizingMask;
    if (w <= 30) {
        c.maxWidth = w;
        c.resizingMask = NSTableColumnNoResizing;
    }
    [_table addTableColumn:c];
}

- (NSMenu *)headerMenu
{
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"Tape"];
    [m addItemWithTitle:@"" action:NULL keyEquivalent:@""];   /* the pull-down's own title */
    [[m addItemWithTitle:@"Load Tape…" action:@selector(load:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Append Tape…" action:@selector(append:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Save Tape As…" action:@selector(save:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Eject" action:@selector(eject:) keyEquivalent:@""] setTarget:self];
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem *stops = [m addItemWithTitle:@"Playing Stops" action:NULL keyEquivalent:@""];
    NSMenu *sm = [[NSMenu alloc] initWithTitle:@"Playing Stops"];
    const struct { NSString *t; int mode; } modes[] = {
        { @"At the Next File", PMD85SESSION_TAPE_STOP_HEADER },
        { @"At the STOP Marker", PMD85SESSION_TAPE_STOP_MARKER },
        { @"At the Tape's End", PMD85SESSION_TAPE_STOP_END },
    };
    for (int i = 0; i < 3; i++) {
        NSMenuItem *it = [sm addItemWithTitle:modes[i].t action:@selector(autostop:) keyEquivalent:@""];
        it.target = self;
        it.tag = modes[i].mode;
    }
    stops.submenu = sm;
    [[m addItemWithTitle:@"Hexadecimal Numbers" action:@selector(hex:) keyEquivalent:@""] setTarget:self];
    return m;
}

- (NSMenu *)rowMenu
{
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"File"];
    const struct { NSString *t; SEL a; } items[] = {
        { @"Set Tape Position", @selector(seek:) },
        { @"Set STOP Marker", @selector(mark:) },
        { @"Clear STOP Marker", @selector(unmark:) },
        { nil, NULL },
        { @"Move Up", @selector(moveUp:) },
        { @"Move Down", @selector(moveDown:) },
        { @"Delete", @selector(deleteBlocks:) },
        { nil, NULL },
        { @"Select All", @selector(selectAllBlocks:) },
    };
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (!items[i].t) {
            [m addItem:[NSMenuItem separatorItem]];
            continue;
        }
        [[m addItemWithTitle:items[i].t action:items[i].a keyEquivalent:@""] setTarget:self];
    }
    return m;
}

- (void)build
{
    const NSRect frame = NSMakeRect(0, 0, 460, 560);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable |
                                                    NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Tape";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    _window.contentMinSize = NSMakeSize(400, 260);

    /* the transport: Play/Stop, Rewind, previous/next file, the progress,
     * the options, the menu */
    _play = [self button:@"play.fill" tip:@"Play" action:@selector(playStop:)];
    _play.bezelStyle = NSBezelStyleRounded;
    _play.bezelColor = [NSColor colorWithSRGBRed:0x55 / 255.0 green:0x79 / 255.0 blue:0x92 / 255.0
                                           alpha:1.0];   /* the PMD's key blue */
    _play.contentTintColor = NSColor.whiteColor;
    _rewind = [self button:@"backward.end.alt.fill" tip:@"Rewind to the first file"
                    action:@selector(rewind:)];
    _prev = [self button:@"backward.end.fill" tip:@"Previous file (data blocks passed over)"
                  action:@selector(prev:)];
    _next = [self button:@"forward.end.fill" tip:@"Next file (data blocks passed over)"
                  action:@selector(next:)];
    _progress = [[NSProgressIndicator alloc] init];
    _progress.indeterminate = NO;
    _progress.minValue = 0;
    _progress.maxValue = 1;
    _progress.style = NSProgressIndicatorStyleBar;
    _progressText = [NSTextField labelWithString:@""];
    _progressText.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                                          weight:NSFontWeightRegular];
    _progressText.lineBreakMode = NSLineBreakByTruncatingTail;
    _progressText.textColor = NSColor.secondaryLabelColor;
    NSStackView *prog = [NSStackView stackViewWithViews:@[ _progress, _progressText ]];
    prog.orientation = NSUserInterfaceLayoutOrientationVertical;
    prog.alignment = NSLayoutAttributeLeading;
    prog.spacing = 2;
    [prog setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_progress.widthAnchor constraintEqualToAnchor:prog.widthAnchor].active = YES;

    _flash = [self toggle:@"bolt.fill"
                      tip:@"Flash load: a file's body goes into memory at once (its header still "
                          @"plays in real time)"
                   action:@selector(option:)];
    _sound = [self toggle:@"speaker.slash.fill"
                      tip:@"Tape sound: the tape's signal on the speaker while it plays"
                   action:@selector(option:)];

    NSPopUpButton *menu = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    menu.menu = [self headerMenu];
    menu.bezelStyle = NSBezelStyleTexturedRounded;
    ((NSPopUpButtonCell *)menu.cell).arrowPosition = NSPopUpNoArrow;
    NSMenuItem *first = menu.menu.itemArray.firstObject;
    first.image = symbol(@"ellipsis.circle", @"More");
    menu.toolTip = @"Load, append, save, eject; where playing stops";
    _menuButton = menu;

    NSStackView *bar = [NSStackView stackViewWithViews:@[
        _play, _rewind, _prev, _next, prog, _flash, _sound, menu ]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 6;
    bar.edgeInsets = NSEdgeInsetsMake(8, 12, 8, 12);
    [bar setCustomSpacing:14 afterView:_play];
    [bar setCustomSpacing:12 afterView:_next];
    [bar setCustomSpacing:12 afterView:prog];

    /* the files */
    _table = [[PmdTapeTable alloc] initWithFrame:NSZeroRect];
    _table.keyTarget = self;
    _table.allowsMultipleSelection = YES;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.style = NSTableViewStyleFullWidth;
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    [self addColumn:@"marker" title:@"" width:22];
    [self addColumn:@"name" title:@"File" width:200];
    [self addColumn:@"start" title:@"Start" width:70];
    [self addColumn:@"length" title:@"Length" width:70];
    [self addColumn:@"bad" title:@"" width:22];
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(rowDoubleClicked:);
    _table.menu = [self rowMenu];

    _scroll = [[NSScrollView alloc] init];
    _scroll.documentView = _table;
    _scroll.hasVerticalScroller = YES;

    /* no tape */
    NSImageView *icon = [NSImageView imageViewWithImage:symbol(@"recordingtape", @"No tape")];
    icon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:44
                                                                              weight:NSFontWeightLight];
    icon.contentTintColor = NSColor.tertiaryLabelColor;
    NSTextField *title = [NSTextField labelWithString:@"No Tape"];
    title.font = [NSFont boldSystemFontOfSize:18];
    NSTextField *desc = [NSTextField wrappingLabelWithString:
        @"Load a .ptp or .pmd tape, or save to tape on the PMD (MGSV) to record one."];
    desc.alignment = NSTextAlignmentCenter;
    desc.textColor = NSColor.secondaryLabelColor;
    [desc.widthAnchor constraintLessThanOrEqualToConstant:320].active = YES;
    NSButton *load = [NSButton buttonWithTitle:@"Load Tape…" target:self action:@selector(load:)];
    load.keyEquivalent = @"\r";
    NSStackView *col = [NSStackView stackViewWithViews:@[ icon, title, desc, load ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.spacing = 10;
    col.translatesAutoresizingMaskIntoConstraints = NO;
    _empty = [[NSView alloc] init];
    [_empty addSubview:col];
    [col.centerXAnchor constraintEqualToAnchor:_empty.centerXAnchor].active = YES;
    [col.centerYAnchor constraintEqualToAnchor:_empty.centerYAnchor].active = YES;

    NSBox *line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;
    NSView *content = [[NSView alloc] initWithFrame:frame];
    for (NSView *v in @[ bar, line, _scroll, _empty ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor constraintEqualToAnchor:content.topAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [line.topAnchor constraintEqualToAnchor:bar.bottomAnchor],
        [line.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_empty.topAnchor constraintEqualToAnchor:line.bottomAnchor],
        [_empty.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_empty.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_empty.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    _window.contentView = content;
    if (![_window setFrameUsingName:@"PMD85TapeWindow"]) [_window center];
    [_window setFrameAutosaveName:@"PMD85TapeWindow"];
}

- (void)show
{
    if (!_window) [self build];
    [self refresh];
    if (!_timer) {
        __weak PmdTapeWindow *weak = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weak refresh];
        }];
        _timer.tolerance = 0.05;
    }
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_table];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    _timer = nil;
}

/* ---- the view follows the deck ------------------------------------------------- */

- (NSString *)number:(int)v
{
    if (v < 0) return @"";
    return _hex ? [NSString stringWithFormat:@"%04X", v] : [NSString stringWithFormat:@"%d", v];
}

/* The rows anew from the deck's list. The selection stays when the list kept
 * its length (an edit that moved blocks re-selects them itself). */
- (void)rebuild
{
    int n = pmd85session_tape_blocks(_session, NULL, 0, NULL);
    pmd85session_tape_block *b = calloc((size_t)(n > 0 ? n : 1), sizeof *b);
    if (!b) return;
    n = pmd85session_tape_blocks(_session, b, n, NULL);
    const BOOL same = n == _nblocks;
    NSIndexSet *sel = _table.selectedRowIndexes;
    free(_blocks);
    _blocks = b;
    _nblocks = n;
    [_table reloadData];
    if (same)
        [_table selectRowIndexes:sel byExtendingSelection:NO];
    _haveList = YES;
}

- (void)refresh
{
    pmd85session_tape_info t;
    pmd85session_tape_get_info(_session, &t);
    if (!_haveList || t.list_serial != _info.list_serial) {
        _info = t;
        [self rebuild];
    } else {
        const BOOL markers = t.current != _info.current || t.marker != _info.marker ||
                             t.playing != _info.playing;
        _info = t;
        if (markers)
            [_table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, (NSUInteger)_nblocks)]
                              columnIndexes:[NSIndexSet indexSetWithIndex:
                                                (NSUInteger)[_table columnWithIdentifier:@"marker"]]];
    }

    const BOOL have = t.blocks > 0;
    _scroll.hidden = !have;
    _empty.hidden = have;

    NSString *name = str(t.path).lastPathComponent;
    _window.subtitle = [NSString stringWithFormat:@"%@%@",
                        !have ? @"No tape" : name.length ? name : @"New tape",
                        t.recorded ? @" • not saved" : @""];

    _play.image = symbol(t.playing ? @"stop.fill" : @"play.fill", t.playing ? @"Stop" : @"Play");
    _play.toolTip = t.playing ? @"Stop" : @"Play";
    _play.enabled = have;
    _rewind.enabled = have;
    _prev.enabled = [_app tapeFile:-1] >= 0;
    _next.enabled = [_app tapeFile:1] >= 0;

    if (t.playing && t.progress_max > 0) {
        _progress.doubleValue = (double)t.progress / t.progress_max;
        _progressText.stringValue = _hex
            ? [NSString stringWithFormat:@"%@  %04X / %04X", str(t.block), t.progress, t.progress_max]
            : [NSString stringWithFormat:@"%@  %d / %d", str(t.block), t.progress, t.progress_max];
    } else {
        _progress.doubleValue = 0;
        _progressText.stringValue = have ? [NSString stringWithFormat:@"%@  (%d of %d)", str(t.block),
                                                                       t.current + 1, t.blocks]
                                         : @"";
    }

    /* the options as the settings are (Settings may change them) */
    const int flash = pmd85session_get_int(_session, "tape_flash", 1) != 0;
    const int sound = pmd85session_get_int(_session, "tape_sound", 0) != 0;
    _flash.state = flash ? NSControlStateValueOn : NSControlStateValueOff;
    _sound.state = sound ? NSControlStateValueOn : NSControlStateValueOff;
    _sound.image = symbol(sound ? @"speaker.wave.2.fill" : @"speaker.slash.fill", @"Tape sound");
}

/* ---- the table ----------------------------------------------------------------- */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return _nblocks;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    if (row < 0 || row >= _nblocks) return nil;
    const pmd85session_tape_block *b = &_blocks[row];
    NSString *ident = col.identifier;
    if ([ident isEqualToString:@"marker"] || [ident isEqualToString:@"bad"]) {
        NSImageView *iv = [tv makeViewWithIdentifier:ident owner:self];
        if (!iv) {
            iv = [[NSImageView alloc] init];
            iv.identifier = ident;
        }
        NSImage *img = nil;
        if ([ident isEqualToString:@"bad"]) {
            if (b->bad) img = symbol(@"exclamationmark.triangle.fill", @"CRC or length error");
            iv.contentTintColor = NSColor.systemOrangeColor;
            iv.toolTip = b->bad ? @"CRC or length error" : nil;
        } else if (row == _info.current) {
            img = symbol(_info.playing ? @"play.fill" : @"arrowtriangle.right.fill", @"Position");
            iv.contentTintColor = NSColor.controlAccentColor;
            iv.toolTip = _info.playing ? @"Playing" : @"Plays next";
        } else if (row == _info.marker) {
            img = symbol(@"stop.fill", @"STOP marker");
            iv.contentTintColor = NSColor.systemRedColor;
            iv.toolTip = @"STOP marker";
        } else {
            iv.toolTip = nil;
        }
        iv.image = img;
        return iv;
    }
    NSTextField *tf = [tv makeViewWithIdentifier:ident owner:self];
    if (!tf) {
        tf = [NSTextField labelWithString:@""];
        tf.identifier = ident;
        tf.font = [NSFont monospacedSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    if ([ident isEqualToString:@"name"]) {
        tf.stringValue = str(b->name);
        tf.alignment = NSTextAlignmentLeft;
    } else {
        tf.stringValue = [self number:[ident isEqualToString:@"start"] ? b->start : b->length];
        tf.alignment = NSTextAlignmentRight;
    }
    return tf;
}

/* ---- transport --------------------------------------------------------------- */

- (void)result:(int)rc
{
    if (rc != 0) [_app toast:str(pmd85session_last_error(_session))];
    [self refresh];
}

- (void)playStop:(id)sender
{
    (void)sender;
    [self result:_info.playing ? pmd85session_tape_stop(_session) : pmd85session_tape_play(_session)];
}

- (void)rewind:(id)sender { (void)sender; [self result:pmd85session_tape_rewind(_session)]; }
- (void)prev:(id)sender { (void)sender; [self result:[_app tapeStep:-1]]; }
- (void)next:(id)sender { (void)sender; [self result:[_app tapeStep:1]]; }

- (void)option:(NSButton *)sender
{
    int flash = pmd85session_get_int(_session, "tape_flash", 1);
    int sound = pmd85session_get_int(_session, "tape_sound", 0);
    const int on = sender.state == NSControlStateValueOn;
    if (sender == _flash) flash = on;
    else sound = on;
    pmd85session_set_tape_options(_session, flash, sound);
    [self refresh];
}

/* double-click: the tape's position to that file */
- (void)rowDoubleClicked:(id)sender
{
    (void)sender;
    const NSInteger row = _table.clickedRow;
    if (row < 0) return;
    if (_info.playing) pmd85session_tape_stop(_session);
    [self result:pmd85session_tape_seek(_session, (int)row)];
}

/* ---- the row actions ------------------------------------------------------------ */

/* the row a menu item is for: the one right-clicked, else the first selected */
- (int)targetRow
{
    if (_table.clickedRow >= 0) return (int)_table.clickedRow;
    return _table.selectedRow >= 0 ? (int)_table.selectedRow : -1;
}

/* the selected rows, ascending; the right-clicked one counts as selected */
- (NSIndexSet *)selection
{
    NSIndexSet *sel = _table.selectedRowIndexes;
    const NSInteger clicked = _table.clickedRow;
    if (clicked >= 0 && ![sel containsIndex:(NSUInteger)clicked])
        return [NSIndexSet indexSetWithIndex:(NSUInteger)clicked];
    return sel;
}

- (int *)rowsOf:(NSIndexSet *)set count:(int *)n
{
    int *rows = calloc(set.count ? set.count : 1, sizeof *rows);
    __block int k = 0;
    [set enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        (void)stop;
        rows[k++] = (int)i;
    }];
    *n = k;
    return rows;
}

- (void)seek:(id)sender
{
    (void)sender;
    if (_info.playing) pmd85session_tape_stop(_session);
    [self result:pmd85session_tape_seek(_session, [self targetRow])];
}

- (void)mark:(id)sender { (void)sender; [self result:pmd85session_tape_mark(_session, [self targetRow])]; }
- (void)unmark:(id)sender { (void)sender; [self result:pmd85session_tape_mark(_session, -1)]; }

- (void)deleteBlocks:(id)sender
{
    (void)sender;
    int n = 0;
    NSIndexSet *set = [self selection];
    int *rows = [self rowsOf:set count:&n];
    const int row = [self targetRow];
    const int rc = pmd85session_tape_delete(_session, row, rows, n);
    const int at = n > 0 ? rows[0] : row;
    free(rows);
    [_table deselectAll:nil];
    [self result:rc];
    if (rc == 0 && _nblocks > 0) {
        /* the row after the deleted ones, ready for the next */
        const int r = at < _nblocks ? at : _nblocks - 1;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
    }
}

- (void)move:(BOOL)up
{
    int n = 0;
    NSIndexSet *set = [self selection];
    int *rows = [self rowsOf:set count:&n];
    const int row = [self targetRow];
    const int first = n > 0 ? rows[0] : row, last = n > 0 ? rows[n - 1] : row;
    const int rc = pmd85session_tape_move(_session, up, row, rows, n);
    free(rows);
    [self result:rc];
    if (rc == 0) {
        const int d = up ? -1 : 1;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndexesInRange:
                                     NSMakeRange((NSUInteger)(first + d), (NSUInteger)(last - first + 1))]
            byExtendingSelection:NO];
        [_table scrollRowToVisible:up ? first + d : last + d];
    }
}

- (void)moveUp:(id)sender { (void)sender; [self move:YES]; }
- (void)moveDown:(id)sender { (void)sender; [self move:NO]; }
- (void)selectAllBlocks:(id)sender { (void)sender; [_table selectAll:nil]; }

- (void)load:(id)sender { (void)sender; [_app openTapeAppend:NO from:_window]; }
- (void)append:(id)sender { (void)sender; [_app openTapeAppend:YES from:_window]; }
- (void)save:(id)sender { (void)sender; [_app saveTapeFrom:_window]; }
- (void)eject:(id)sender { (void)sender; [_app ejectTapeFrom:_window]; }

- (void)autostop:(NSMenuItem *)sender
{
    pmd85session_set_tape_autostop(_session, (int)sender.tag);
}

- (void)hex:(id)sender
{
    (void)sender;
    _hex = !_hex;
    pmd85session_set_int(_session, "tape_hex", _hex);
    [_table reloadData];
    [self refresh];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = item.action;
    const pmd85session_tape_info *t = &_info;
    const BOOL have = t->blocks > 0, idle = have && !t->playing;
    const int row = [self targetRow];
    NSIndexSet *set = [self selection];
    const int first = set.count ? (int)set.firstIndex : row;
    const int last = set.count ? (int)set.lastIndex : row;
    const BOOL run = set.count == 0 || (NSUInteger)(last - first + 1) == set.count;

    if (a == @selector(autostop:)) {
        item.state = item.tag == pmd85session_tape_autostop(_session) ? NSControlStateValueOn
                                                                       : NSControlStateValueOff;
        return YES;
    }
    if (a == @selector(hex:)) {
        item.state = _hex ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (a == @selector(seek:)) return idle && row >= 0 && row != t->current;
    if (a == @selector(mark:)) return idle && row > t->current && row != t->marker;
    if (a == @selector(unmark:)) return idle && t->marker >= 0;
    if (a == @selector(deleteBlocks:)) return idle && (set.count > 0 || row >= 0);
    if (a == @selector(moveUp:)) return idle && run && first > 0;
    if (a == @selector(moveDown:)) return idle && run && last >= 0 && last < t->blocks - 1;
    if (a == @selector(selectAllBlocks:) || a == @selector(save:) || a == @selector(eject:))
        return have;
    return YES;
}

@end
