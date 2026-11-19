/*
 * Debugger window (AppKit) over the session's 8080 debugger engine
 * (pmd85debug.h, pmd85memtools.h): the GNOME app's window
 * (frontends/gnome/debugger/dbg_window.c), tab for tab, in the AppKit shape
 * of fujinet-go-coco-desktop's macOS debugger.
 *
 * Tabs (PMD85_DEBUGGER_TAB takes the number, or the start of the name):
 *   0 Console      the engine's command set, Tab completion, history
 *   1 Disassembly  breakpoint gutter, follow-PC, jump box, a selected row;
 *                  Intel 8080 or Z80 mnemonics, hex as C000h/$C000/0xC000/#C000
 *   2 Memory       the CPU's 64K as mapped now, go-to (a range is
 *                  highlighted), region labels, search (hex or text), poke
 *   3 Breakpoints  why it stopped; execute, read, write, access, IN, OUT,
 *                  IN/OUT on ports, conditions, hit counts
 *   4 Watches      addresses, symbols and ranges with their live values
 *   5 I/O          the memory map, the 8255s, the 8253, the 8251, the
 *                  modules and the Becker link; the I/O log beside it
 *   6 Trace        the instruction ring, live while the machine runs
 *   7 History      the same ring, read when the machine stops
 *   8 Symbols      every known symbol (the monitor's, and loaded ones)
 *   9 FujiNet      every DriveWire packet, with the Becker port's state
 * The registers (PC SP A F BC DE HL, the flags S Z AC P CY) sit under the
 * tabs and are editable while the machine is stopped. Any tab can be popped
 * out into a window of its own (Pop Out, then Dock; closing that window
 * docks it).
 *
 * Opening the window stops the machine (engage + pause); closing it releases
 * the engine so the machine goes on at full speed. A 100 ms timer polls the
 * engine's stop snapshot and generation and refreshes the tabs on show.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, F9 breakpoint on the selected line, F12 or Option+F12 close.
 * Environment hooks read at creation: PMD85_DEBUGGER_TAB, PMD85_WATCH,
 * PMD85_WATCH_BREAK, PMD85_MEM_GOTO, PMD85_HISTORY, PMD85_POPOUT.
 *
 * Built only on CI (no Apple toolchain on the development machine).
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "pmd85debug.h"
#include "pmd85memtools.h"

#define DISASM_WINDOW 48
#define DISASM_LINES (DISASM_WINDOW * 2)   /* an instruction, and maybe its label */
#define MEM_ROWS 32
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_REGIONS 64
#define POKE_MAX 64
#define FUJI_PACKETS_MAX 512
#define NUM_REGS 7              /* PC SP A F BC DE HL */
#define NUM_FLAGS 5             /* S Z AC P CY */
#define HIST_RING 65536         /* the engine's trace ring */
#define HIST_SHOWN 2000         /* the newest matching lines a tab shows */
#define IOLOG_READ 4096
#define SYMBOLS_SHOWN 2000
#define TRACE_TICKS 4           /* 4 x 100 ms: the Trace tab's live refresh */
#define LIVE_TICKS 5            /* 5 x 100 ms: the live refresh while running */

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_IO, TAB_TRACE,
       TAB_HISTORY, TAB_SYMBOLS, TAB_FUJINET, NUM_TABS };

/* The blue of the PMD 85's K0-K11 keys (as in the app's icon), dimmed so the
 * white reads on it: the PC's line, the Run button while stopped, Record. */
#define ACCENT_RGB 0x557992u
#define ACCENT_INK_RGB 0xFFFFFFu

static PmdDebuggerWindow *g_debugger;

/* ---- colours ---------------------------------------------------------------- */

static NSColor *rgbColor(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static NSColor *accentFill(void) { return rgbColor(ACCENT_RGB); }
static NSColor *accentInk(void) { return rgbColor(ACCENT_INK_RGB); }

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* Toggled-on look for a push button: accent bezel and the white title. Off
 * restores the plain bezel and title. */
static void setButtonAccent(NSButton *button, BOOL active)
{
    if (!button)
        return;
    NSString *title = [button.title copy];
    if (active) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accentFill();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName: button.font ?: [NSFont systemFontOfSize:[NSFont systemFontSize]],
                    NSForegroundColorAttributeName: accentInk(),
                    NSParagraphStyleAttributeName: para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;       /* drops the attributed title */
    }
}

/* ---- text views that report clicks ------------------------------------------ */

/* A monospace text view for rows: a click reports its line, column and click
 * count; the wheel browses by row (when onScroll is set). The disassembly and
 * the memory dump. */
@interface PmdRowTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation PmdRowTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length)
        index = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger pos = 0; pos < index; pos++)
        if ([text characterAtIndex:pos] == '\n') {
            line++;
            lineStart = pos + 1;
        }
    if (self.onClick)
        self.onClick(line, (int)(index - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (!self.onScroll) {
        [super scrollWheel:event];
        return;
    }
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0)
        return;
    _wheel -= rows;
    self.onScroll(rows);
}
@end

/* ---- the window --------------------------------------------------------------- */

@interface PmdDebuggerWindow () <NSWindowDelegate, NSTabViewDelegate, NSTextFieldDelegate>
- (instancetype)initWithSession:(pmd85session *)session;
- (void)show;
- (void)hide;
- (void)refreshNow;
@end

@implementation PmdDebuggerWindow {
    pmd85session *_session;
    pmd85debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    int _wasPaused;             /* -1 until the first look: forces a refresh */
    uint64_t _seenStop;         /* the stop serial last acted on */
    int _liveTicks;
    int _traceTicks;
    NSTabView *_tabs;
    NSTabViewItem *_items[NUM_TABS];    /* each tab's item, in order */
    NSWindow *_popped[NUM_TABS];        /* its own window while popped out */

    NSButton *_runBtn;
    NSTextField *_status;
    NSTextField *_breakAt;
    NSPopUpButton *_syntax, *_hexStyle;

    /* Console */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* the strip under the tabs */
    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_machine;

    /* Disassembly */
    NSButton *_followPc;
    NSTextField *_jump;
    PmdRowTextView *_disasm;
    uint16_t _disasmTop;            /* the first instruction shown */
    int _lineAddr[DISASM_LINES];    /* per text line; -1 for a label line */
    int _lineCount;
    int _selected;                  /* the selected row's address, -1 none */

    /* Memory */
    PmdRowTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    uint32_t _memBase;
    BOOL _memHl;                    /* a range is highlighted: Go to, a hit, a watch */
    uint32_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    /* Breakpoints */
    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    /* Watches */
    NSStackView *_wtList;
    NSString *_wtSig;
    NSTextField *_wtEntry;

    /* I/O */
    NSTextView *_ioText, *_ioLog;
    NSButton *_ioOn;
    NSTextField *_ioFilter, *_ioNote;
    uint64_t _ioSig;

    /* Trace (0) and History (1) share one ring */
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];
    uint64_t _hSig[2];

    /* Symbols */
    NSTextField *_symFilter, *_symCount, *_symInfo, *_symName, *_symAddr;
    PmdRowTextView *_symList;
    NSMutableArray<NSNumber *> *_symAddrs;      /* the address of each line shown */
    int _symSelected;                           /* the selected line, -1 for none */
    NSString *_symSig;

    /* FujiNet */
    NSTextField *_fujiNote;
    NSButton *_fujiHideReady;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

static NSString *const kTabTitles[NUM_TABS] = {
    @"Console", @"Disassembly", @"Memory", @"Breakpoints", @"Watches", @"I/O", @"Trace",
    @"History", @"Symbols", @"FujiNet"
};

/* The register fields, in strip order: label, hex digits, the engine's register. */
static NSString *const kRegNames[NUM_REGS] = { @"PC", @"SP", @"A", @"F", @"BC", @"DE", @"HL" };
static const int kRegDigits[NUM_REGS] = { 4, 4, 2, 2, 4, 4, 4 };
static const int kRegIds[NUM_REGS] = {
    PMD85DEBUG_REG_PC, PMD85DEBUG_REG_SP, PMD85DEBUG_REG_A, PMD85DEBUG_REG_F,
    PMD85DEBUG_REG_BC, PMD85DEBUG_REG_DE, PMD85DEBUG_REG_HL
};
/* The F bits as the strip shows them. */
static NSString *const kFlagNames[NUM_FLAGS] = { @"S", @"Z", @"AC", @"P", @"CY" };
static const unsigned kFlagMasks[NUM_FLAGS] = {
    PMD85DEBUG_F_S, PMD85DEBUG_F_Z, PMD85DEBUG_F_AC, PMD85DEBUG_F_P, PMD85DEBUG_F_CY
};

/* A tab by number, by an alias, or by the start of its title; -1 for none. */
static int tabFromText(const char *tab)
{
    static const struct { const char *name; int tab; } aliases[] = {
        { "cpu", TAB_DISASM }, { "disasm", TAB_DISASM }, { "io", TAB_IO }, { "iolog", TAB_IO },
        { "breaks", TAB_BREAKS }, { "mem", TAB_MEMORY }, { "regs", TAB_CONSOLE },
    };
    if (!tab || !*tab)
        return -1;
    char *end;
    const long number = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return number >= 0 && number < NUM_TABS ? (int)number : -1;
    const size_t len = strlen(tab);
    for (size_t index = 0; index < sizeof aliases / sizeof aliases[0]; index++)
        if (!strcasecmp(tab, aliases[index].name))
            return aliases[index].tab;
    for (int index = 0; index < NUM_TABS; index++)
        if (!strncasecmp(tab, kTabTitles[index].UTF8String, len))
            return index;
    for (size_t index = 0; index < sizeof aliases / sizeof aliases[0]; index++)
        if (!strncasecmp(tab, aliases[index].name, len))
            return aliases[index].tab;
    return -1;
}

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped, UTF-8 if it is,
 * otherwise Latin-1. */
static NSString *cstr(const char *text)
{
    if (!text)
        return @"";
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy)
        return @"";
    size_t used = 0;
    for (size_t pos = 0; pos < length; pos++) {
        const unsigned char code = (unsigned char)text[pos];
        if (code >= 0x20 || code == '\n' || code == '\t')
            copy[used++] = (char)code;
    }
    copy[used] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result)
        result = [[NSString alloc] initWithBytes:copy length:used encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

static NSString *trimmed(NSString *text)
{
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
}

/* $hex, 0xhex, #hex, hexh, or bare hex (the PMD's own listings write #C000). */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *word = trimmed(text);
    if ([word hasPrefix:@"$"] || [word hasPrefix:@"#"])
        word = [word substringFromIndex:1];
    else if ([word.lowercaseString hasPrefix:@"0x"])
        word = [word substringFromIndex:2];
    else if (word.length > 1 && [word.lowercaseString hasSuffix:@"h"])
        word = [word substringToIndex:word.length - 1];
    if (word.length == 0)
        return NO;
    char *end;
    const long value = strtol(word.UTF8String, &end, 16);
    if (*end)
        return NO;
    *out = value;
    return YES;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with the white ink on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: accentFill(), NSForegroundColorAttributeName: accentInk() };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A monospace row view in a scroll view with no vertical bar: the wheel and
 * the clicks are the view's own (the disassembly, the memory dump). */
static PmdRowTextView *rowView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    PmdRowTextView *view = [[PmdRowTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    view.editable = NO;
    view.richText = NO;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A click-reporting list that scrolls as any text view does (Symbols). */
static PmdRowTextView *listView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    PmdRowTextView *view = [[PmdRowTextView alloc] initWithFrame:NSMakeRect(0, 0, 700, 500)];
    view.editable = NO;
    view.richText = YES;
    view.selectable = NO;
    view.font = monoFont();
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll)
        return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint where = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:where];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow)
        [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint where = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:where];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    return stack;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    return stack;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* One table cell: a fixed-width monospace label, or (width 0) the flexible
 * one that takes what is left. */
static NSTextField *tableCell(NSString *text, CGFloat width)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = monoFont();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [label.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [label setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return label;
}

/* A stack of rows that scrolls: the document view is pinned to the top and to
 * the clip view's width so rows keep the tab's width. */
static NSScrollView *rowsScroll(NSStackView *rows)
{
    rows.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = rows;
    NSClipView *clip = scroll.contentView;
    [rows.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [rows.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    return scroll;
}

/* "Stopped", "Stopped at C000h", "Stopped: breakpoint ...", or "Running". */
static NSString *stopLine(const pmd85debug_stop *stop, NSString *running)
{
    if (!stop->paused)
        return running;
    if (!stop->reason[0] || !strcmp(stop->reason, "stopped"))
        return @"Stopped";
    if (!strncmp(stop->reason, "stopped ", 8))
        return [@"S" stringByAppendingString:cstr(stop->reason + 1)];
    return [@"Stopped: " stringByAppendingString:cstr(stop->reason)];
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *button = [NSButton buttonWithTitle:title target:self action:sel];
    [button setRefusesFirstResponder:YES];
    return button;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return label;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.font = monoFont();
    field.target = self;
    field.action = sel;
    if (width > 0)
        [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

- (NSPopUpButton *)popup:(NSArray<NSString *> *)titles action:(SEL)sel
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [popup addItemsWithTitles:titles];
    popup.target = self;
    popup.action = sel;
    [popup setRefusesFirstResponder:YES];
    return popup;
}

/* A number in the hex style chosen (C000h, $C000, 0xC000, #C000). */
- (NSString *)hex:(unsigned)value digits:(int)digits
{
    char text[24];
    text[0] = '\0';
    pmd85debug_format_hex(_dbg, value, digits, text, (int)sizeof text);
    return cstr(text);
}

/* A value as the fields take it: a symbol, a number (C000h, $C000, 0xC000,
 * #C000, bare hex) or an expression (registers, [addr], symbols: the
 * console's). On failure the reason is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *word = trimmed(text);
    if (word.length == 0) {
        if (why)
            *why = @"Enter an address, a symbol or an expression";
        return NO;
    }
    uint16_t symbolAddr;
    if (pmd85debug_symbol_find(_dbg, word.UTF8String, &symbolAddr)) {
        *out = symbolAddr;
        return YES;
    }
    if (parseNum(word, out))
        return YES;
    char err[160];
    err[0] = '\0';
    long value;
    if (pmd85debug_eval(_dbg, word.UTF8String, &value, err, (int)sizeof err) == 0) {
        *out = value;
        return YES;
    }
    if (why)
        *why = err[0] ? cstr(err) : @"Not an address, a symbol or an expression";
    return NO;
}

/* An address: the above, within 0000-FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long value;
    if (![self evaluate:text into:&value why:why])
        return -1;
    if (value < 0 || value > 0xFFFF) {
        if (why)
            *why = [NSString stringWithFormat:@"Out of range (0 to %@)", [self hex:0xFFFF digits:4]];
        return -1;
    }
    return (int)value;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(pmd85session *)session
{
    if (!g_debugger) {
        if (!pmd85session_debugger(session)) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Debugger Unavailable";
            alert.informativeText = @"The debugger could not be created for this session.";
            [alert runModal];
            return;
        }
        g_debugger = [[PmdDebuggerWindow alloc] initWithSession:session];
    } else if (!g_debugger->_window.isVisible) {
        [g_debugger attach];
    }
    [g_debugger show];
}

+ (void)toggleForSession:(pmd85session *)session
{
    if (g_debugger && g_debugger->_window.isVisible)
        [g_debugger hide];
    else
        [self showForSession:session];
}

+ (BOOL)isShown
{
    return g_debugger && g_debugger->_window.isVisible;
}

+ (void)shutdown
{
    if (!g_debugger)
        return;
    [g_debugger->_tick invalidate];
    g_debugger->_tick = nil;
    if (g_debugger->_keyMonitor) {
        [NSEvent removeMonitor:g_debugger->_keyMonitor];
        g_debugger->_keyMonitor = nil;
    }
    /* let the emulator thread out of the hook before the session stops it */
    pmd85debug_set_engaged(g_debugger->_dbg, 0);
    for (int tab = 0; tab < NUM_TABS; tab++) {
        g_debugger->_popped[tab].delegate = nil;
        [g_debugger->_popped[tab] orderOut:nil];
    }
    g_debugger->_window.delegate = nil;
    [g_debugger->_window orderOut:nil];
}

/* Engages the engine and stops the machine: what a user opening a debugger
 * expects -- otherwise the window opens showing nothing at all. */
- (void)attach
{
    const unsigned gen = pmd85debug_generation(_dbg);
    pmd85debug_set_engaged(_dbg, 1);
    pmd85debug_pause(_dbg);
    [self settleFrom:gen];
    _bpSig = nil;
    _wasPaused = -1;
    _liveTicks = 0;
}

- (void)show
{
    if (_window.isMiniaturized)
        [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_popped[tab])
            [_popped[tab] orderFront:nil];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE)
        [_window makeFirstResponder:_conIn];
}

/* Hiding keeps the window and its state (it comes back as it was), and
 * releases the engine right here: left engaged, the machine would run on
 * the slower instruction-hooked path with the window merely hidden. */
- (void)hide
{
    pmd85debug_set_engaged(_dbg, 0);
    _wasPaused = -1;
    for (int tab = 0; tab < NUM_TABS; tab++)
        [_popped[tab] orderOut:nil];     /* they come back with the window */
    [_window orderOut:nil];
}

- (instancetype)initWithSession:(pmd85session *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = pmd85session_debugger(session);
    _fujiCount = -1;
    _wasPaused = -1;
    _selected = -1;
    _symSelected = -1;
    _ioSig = ~(uint64_t)0;
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    _symAddrs = [NSMutableArray array];
    _history = [NSMutableArray array];
    [self buildWindow];
    [self attach];

    /* For captures and tests */
    {
        const char *watch = getenv("PMD85_WATCH");
        const char *watchBreak = getenv("PMD85_WATCH_BREAK");
        const char *gotoText = getenv("PMD85_MEM_GOTO");
        const char *popOut = getenv("PMD85_POPOUT");
        uint32_t low, high;
        if (watch && *watch) {
            for (NSString *item in [cstr(watch) componentsSeparatedByString:@","]) {
                NSString *text = trimmed(item);
                if (text.length)
                    pmd85watch_add(text.UTF8String, PMD85DEBUG_SPACE_CPU);
            }
        }
        if (watchBreak && *watchBreak &&
            pmd85sym_parse_range(_dbg, PMD85DEBUG_SPACE_CPU, watchBreak, &low, &high))
            [self breakOnChangeLow:low high:high];
        if (gotoText && *gotoText && pmd85sym_parse_range(_dbg, PMD85DEBUG_SPACE_CPU, gotoText, &low, &high))
            [self showMemoryLow:low high:high];
        if (getenv("PMD85_HISTORY"))
            pmd85debug_trace_enable(_dbg, 1);
        if (popOut && *popOut) {
            const int tab = tabFromText(popOut);
            if (tab >= 0)
                [self popOutTab:tab];
        }
    }

    __weak PmdDebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* the keys wherever the focus is in this window, the console's field
     * included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        PmdDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || ![strongSelf ownsWindow:event.window] || !strongSelf->_window.isVisible)
            return event;
        return [strongSelf handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor)
        [NSEvent removeMonitor:_keyMonitor];
}

/* The close control (or Window > Close): a popped-out tab's window docks the
 * tab again; the main window hides. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (sender == _popped[tab]) {
            [self dockTab:tab];
            return NO;
        }
    [self hide];
    return NO;
}

/* The debugger's main window or one of its popped-out tabs. */
- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window)
        return NO;
    if (window == _window)
        return YES;
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_popped[tab] == window)
            return YES;
    return NO;
}

/* The tab the main window's tab view shows, by its place in the full order
 * (-1 if none). */
- (NSInteger)currentTab
{
    NSTabViewItem *selected = _tabs.selectedTabViewItem;
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_items[tab] == selected)
            return tab;
    return -1;
}

/* On screen: the tab view's page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || [self currentTab] == tab;
}

/* A new stop (a pause, a breakpoint, a step): the disassembly goes back to
 * the PC even if the reader scrolled away; the Trace and History logs redraw
 * and follow their newest line. */
- (void)recentreViews
{
    _followPc.state = NSControlStateValueOn;
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tabs' live values twice a second while the machine runs. */
- (void)onTick
{
    if (![_window isVisible])
        return;
    /* shown across a session restart: the engine comes back with the machine */
    if (!pmd85debug_is_engaged(_dbg) && pmd85session_is_running(_session)) {
        [self attach];
        [self refreshNow];
        return;
    }
    pmd85debug_stop stop;
    pmd85debug_stop_snapshot(_dbg, &stop);
    const unsigned gen = pmd85debug_generation(_dbg);
    if (gen != _seenGen || stop.paused != _wasPaused || stop.serial != _seenStop) {
        [self refreshNow];
        return;
    }
    if (!stop.paused && [self tabVisible:TAB_IO])
        [self refreshIolog:NO];
    if (!stop.paused && [self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;        /* the Trace tab follows the machine */
        [self refreshHist:0];
    }
    if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stop.paused)
            [self refreshNow];
        else if ([self tabVisible:TAB_FUJINET])
            [self refreshFujinet:NO];   /* its packets arrive on their own */
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as paused. */
- (void)settleFrom:(unsigned)gen
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (pmd85debug_generation(_dbg) != gen && pmd85debug_is_paused(_dbg))
            break;
        usleep(2000);
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE)
        [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1)
        return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl))
        return NO;
    const unichar key = [chars characterAtIndex:0];
    if (key == NSF12FunctionKey) {      /* with Option, as the main window opens it, or without */
        [self hide];
        return YES;
    }
    if (mods & NSEventModifierFlagOption)
        return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch (key) {
    case NSF5FunctionKey:
        [self toggleRun:nil];
        return YES;
    case NSF7FunctionKey:
        [self step:nil];
        return YES;
    case NSF8FunctionKey:
        if (shift)
            [self stepOut:nil];
        else
            [self stepOver:nil];
        return YES;
    case NSF9FunctionKey:
        [self toggleSelected];
        return YES;
    default:
        return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break C000h, bpw 0400h, bpout 3F, regs, mem 8000h, io, ...)"
                   width:0
                  action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *page = vstack(@[outScroll, _conIn]);
    fill(page, outScroll);
    fill(page, _conIn);
    return page;
}

/* The registers and flags, under the tabs on every one of them, and the
 * machine with the CPU's interrupt and halt state. */
- (NSView *)buildRegStrip
{
    NSMutableArray *row = [NSMutableArray array];
    for (int field = 0; field < NUM_REGS; field++) {
        [row addObject:[self label:kRegNames[field]]];
        _reg[field] = [self field:@"" width:(kRegDigits[field] == 4 ? 52 : 36) action:@selector(applyRegister:)];
        _reg[field].tag = field;
        _reg[field].toolTip = @"Edit while the machine is stopped (hex, a symbol or an expression), then Return";
        [row addObject:_reg[field]];
    }
    [row addObject:[self label:@"  "]];
    for (int flag = 0; flag < NUM_FLAGS; flag++) {
        _flag[flag] = [NSButton checkboxWithTitle:kFlagNames[flag] target:self action:@selector(flagClicked:)];
        _flag[flag].tag = flag;
        [row addObject:_flag[flag]];
    }
    _machine = [self hint:@""];
    [row addObject:_machine];
    return hstack(row);
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address, symbol or expression" width:200 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it; click its gutter, double-click it or press "
                                           @"F9 to toggle its breakpoint. Scroll to browse."]]);

    NSScrollView *scroll;
    _disasm = rowView(&scroll);
    __weak PmdDebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        PmdDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= strongSelf->_lineCount || strongSelf->_lineAddr[line] < 0)
            return;
        const BOOL gutter = column < 2;
        strongSelf->_selected = strongSelf->_lineAddr[line];
        if ((clicks == 1 && gutter) || (clicks == 2 && !gutter)) {
            [strongSelf toggleExecBreakpoint:(uint16_t)strongSelf->_selected];
            [strongSelf refreshNow];
        } else {
            [strongSelf refreshDisasm];
        }
    };
    _disasm.onScroll = ^(int rows) {
        PmdDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0)
            return;
        strongSelf->_followPc.state = NSControlStateValueOff;
        strongSelf->_disasmTop = (uint16_t)pmd85debug_row_address(strongSelf->_dbg, strongSelf->_disasmTop, rows);
        [strongSelf refreshDisasm];
    };

    NSStackView *page = vstack(@[row, scroll]);
    fill(page, row);
    fill(page, scroll);
    return page;
}

- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = rowView(&memScroll);
    __weak PmdDebuggerWindow *weakSelf = self;
    _mem.onScroll = ^(int rows) {
        PmdDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0)
            return;
        [strongSelf setMemBase:(long)strongSelf->_memBase + rows * 16];
    };
    _memGoto = [self field:@"address, symbol, expression or lo-hi" width:240 action:@selector(gotoMem:)];
    NSStackView *memHead = hstack(@[[self label:@"Go to"], _memGoto, [self button:@"Go To" action:@selector(gotoMem:)],
                                    [self button:@"◀" action:@selector(memPrev:)],
                                    [self button:@"▶" action:@selector(memNext:)]]);

    _srchKind = [self popup:@[@"Hex", @"Text"] action:nil];
    _srchKind.toolTip = @"Hex bytes (86 41) or text, one byte per character, not case-sensitive";
    _srchEntry = [self field:@"hex bytes or text" width:220 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);

    _memWrite = [self field:@"address value [value...]" width:200 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"RAM and VRAM only: a ROM byte is skipped. Several values go to "
                                            @"consecutive addresses. Reading never touches a device."]]);

    NSStackView *page = vstack(@[memHead, searchRow, memScroll, edit]);
    fill(page, memScroll);
    return page;
}

- (NSView *)buildBreaks
{
    _bpStop = [self hint:@""];
    _bpList = vstack(@[]);
    NSScrollView *bpScroll = rowsScroll(_bpList);

    _bpType = [self popup:@[@"Execute", @"Read", @"Write", @"Access", @"IN port", @"OUT port", @"IN/OUT port"]
                   action:nil];
    _bpStart = [self field:@"C000h / symbol / port" width:200 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"(end)" width:90 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition, e.g. a == FFh && value != old — optional" width:0
                   action:@selector(addBreakpoint:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"If"], _bpCond, [self button:@"Add" action:@selector(addBreakpoint:)],
                                [self button:@"Clear Hits" action:@selector(clearHits:)],
                                [self button:@"Clear All" action:@selector(clearBreaks:)]]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is a C expression: registers, flags, [addr], "
                                   @"symbols; on a Read/Write/Access break value, addr and old work too, on a port "
                                   @"break value and port. The end defaults to the start."];

    NSStackView *page = vstack(@[_bpStop, bpScroll, hint, row, _bpErr]);
    fill(page, bpScroll);
    fill(page, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return page;
}

/* Watches: an entry, then a row for each with its value and three buttons. */
- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, symbol, expression or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *row = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    row.distribution = NSStackViewDistributionFill;

    _wtList = vstack(@[]);
    NSScrollView *scroll = rowsScroll(_wtList);
    NSTextField *hint = [self hint:@"Values show while the machine is stopped. Show in Memory shows the watch in the "
                                   @"Memory tab; Break on Change stops when the machine writes a different value there."];
    NSStackView *page = vstack(@[row, scroll, hint]);
    fill(page, row);
    fill(page, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return page;
}

/* The machine, its memory map and chips left; the I/O log right. */
- (NSView *)buildIo
{
    NSScrollView *chipsScroll, *logScroll;
    _ioText = monoView(&chipsScroll, NO);
    _ioLog = monoView(&logScroll, NO);
    _ioOn = [NSButton checkboxWithTitle:@"Log" target:self action:@selector(ioOn:)];
    _ioOn.toolTip = @"Record every IN and OUT while the debugger is open";
    _ioFilter = [self field:@"port or lo-hi, e.g. 3E-3F" width:150 action:@selector(ioFilterChanged:)];
    _ioFilter.delegate = self;
    _ioNote = [self hint:@""];
    NSStackView *row = hstack(@[_ioOn, [self label:@"Ports"], _ioFilter,
                                [self button:@"Clear" action:@selector(ioClear:)], _ioNote]);
    row.distribution = NSStackViewDistributionFill;

    NSStackView *left = vstack(@[[self heading:@"Machine, memory and chips"], chipsScroll]);
    left.distribution = NSStackViewDistributionFill;
    fill(left, chipsScroll);
    NSStackView *right = vstack(@[[self heading:@"I/O log (T-state, PC, access, port name)"], row, logScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, row);
    fill(right, logScroll);

    NSStackView *page = hstack(@[left, right]);
    page.alignment = NSLayoutAttributeTop;
    page.spacing = 12;
    page.distribution = NSStackViewDistributionFill;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:460].active = YES;
    [right.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [left.widthAnchor constraintEqualToAnchor:right.widthAnchor multiplier:1.15].active = YES;
    [left.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    return page;
}

/* Trace (idx 0) and History (idx 1): two views of the engine's one ring. */
- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs slower)";
    _hFilter[idx] = [self popup:@[@"All instructions", @"Branches only", @"In range", @"IN/OUT only"]
                         action:@selector(histChanged:)];
    _hRange[idx] = [self field:@"range: 8000-8FFF or a symbol" width:200 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx],
                                [self button:@"Clear" action:@selector(histClear:)], _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *page = vstack(@[bar, scroll]);
    fill(page, bar);
    fill(page, scroll);
    return page;
}

/* Symbols: a filter over the table (the monitor's and loaded ones). A click
 * selects a row, a double-click (or Show in Disassembly) goes to its address. */
- (NSView *)buildSymbols
{
    _symFilter = [self field:@"filter by name" width:240 action:@selector(symFilterChanged:)];
    _symFilter.delegate = self;
    _symCount = [self hint:@""];
    NSStackView *bar = hstack(@[[self label:@"Filter"], _symFilter, _symCount,
                                [self button:@"Show in Disassembly" action:@selector(symShow:)],
                                [self button:@"Break at" action:@selector(symBreak:)],
                                [self button:@"Load…" action:@selector(loadSymbols:)],
                                [self button:@"Clear" action:@selector(clearSymbols:)]]);
    _symInfo = [self hint:@""];
    _symName = [self field:@"label name" width:180 action:@selector(defineSymbol:)];
    _symAddr = [self field:@"address" width:100 action:@selector(defineSymbol:)];
    NSStackView *define = hstack(@[[self label:@"Define"], _symName, [self label:@"="], _symAddr,
                                   [self button:@"Set Label" action:@selector(defineSymbol:)]]);

    NSScrollView *scroll;
    _symList = listView(&scroll);
    __weak PmdDebuggerWindow *weakSelf = self;
    _symList.onClick = ^(int line, int column, NSInteger clicks) {
        (void)column;
        PmdDebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= (int)strongSelf->_symAddrs.count)
            return;
        strongSelf->_symSelected = line;
        if (clicks >= 2)
            [strongSelf showDisassemblyAt:(uint16_t)strongSelf->_symAddrs[(NSUInteger)line].intValue];
        else
            [strongSelf refreshSymbols:YES];
    };

    NSTextField *hint = [self hint:@"Load reads NAME EQU addr lines, a z88dk .map or NAME ADDR lines; Clear drops "
                                   @"what was loaded and keeps the monitor's names."];
    NSStackView *page = vstack(@[bar, _symInfo, scroll, define, hint]);
    fill(page, bar);
    fill(page, _symInfo);
    fill(page, scroll);
    return page;
}

/* The DriveWire packets left (a note, Hide READY probes and Clear over them),
 * the Becker port's state right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self hint:@""];
    [_fujiNote setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    _fujiHideReady = [NSButton checkboxWithTitle:@"Hide READY probes" target:self action:@selector(fujiHideReady:)];
    _fujiHideReady.state = NSControlStateValueOn;
    _fujiHideReady.toolTip = @"fujinet-lib asks E2 00 before every call";
    NSStackView *row = hstack(@[_fujiNote, _fujiHideReady, [self button:@"Clear" action:@selector(fujiClear:)]]);
    row.distribution = NSStackViewDistributionFill;

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"DriveWire packets"], row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSStackView *right = vstack(@[[self heading:@"Becker port"], stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *page = hstack(@[left, right]);
    page.alignment = NSLayoutAttributeTop;
    page.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:560].active = YES;
    [right.widthAnchor constraintEqualToConstant:340].active = YES;
    [left.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    return page;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1150, 820)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar rows. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Pause (F5)" action:@selector(toggleRun:)];
    NSButton *stepBtn = [self button:@"Step (F7)" action:@selector(step:)];
    stepBtn.toolTip = @"One instruction";
    NSButton *overBtn = [self button:@"Over (F8)" action:@selector(stepOver:)];
    overBtn.toolTip = @"Over a subroutine call";
    NSButton *outBtn = [self button:@"Out (⇧F8)" action:@selector(stepOut:)];
    outBtn.toolTip = @"Until this routine returns";
    NSButton *cursorBtn = [self button:@"To Cursor" action:@selector(runToCursor:)];
    cursorBtn.toolTip = @"Run to the selected disassembly line";
    NSButton *clearBps = [self button:@"Clear BPs" action:@selector(clearBreaks:)];
    clearBps.toolTip = @"Remove every breakpoint";
    NSButton *popBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSStackView *toolbar = hstack(@[_runBtn, stepBtn, overBtn, outBtn, cursorBtn, clearBps, popBtn]);

    _breakAt = [self field:@"symbol or hex" width:150 action:@selector(breakAt:)];
    _breakAt.toolTip = @"Set an execute breakpoint at a symbol, a hex address or an expression";
    NSButton *breakBtn = [self button:@"Break" action:@selector(breakAt:)];
    NSButton *loadBtn = [self button:@"Symbols…" action:@selector(loadSymbols:)];
    loadBtn.toolTip = @"Load a symbol file (NAME EQU addr, a z88dk .map, or NAME ADDR lines)";
    NSButton *clearSymBtn = [self button:@"Clear Symbols" action:@selector(clearSymbols:)];
    clearSymBtn.toolTip = @"Forget every loaded symbol; the monitor's stay";
    _syntax = [self popup:@[@"Intel 8080", @"Z80"] action:@selector(syntaxChanged:)];
    _syntax.toolTip = @"The mnemonics: Intel's own, or the Z80 spelling";
    [_syntax selectItemAtIndex:(NSInteger)pmd85debug_syntax(_dbg)];
    _hexStyle = [self popup:@[@"C000h", @"$C000", @"0xC000", @"#C000"] action:@selector(hexStyleChanged:)];
    _hexStyle.toolTip = @"How hex numbers are shown (any of them is accepted when typed)";
    [_hexStyle selectItemAtIndex:(NSInteger)pmd85debug_hex_style(_dbg)];
    NSStackView *toolbar2 = hstack(@[[self label:@"Break at"], _breakAt, breakBtn, loadBtn, clearSymBtn,
                                     [self label:@"Mnemonics"], _syntax, [self label:@"Hex"], _hexStyle]);

    /* the stop reason gets a row of its own, so it is never cut short */
    _status = [self hint:@"Running"];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks],
                        [self buildWatches], [self buildIo], [self buildHist:0], [self buildHist:1],
                        [self buildSymbols], [self buildFujinet] ];
    for (int tab = 0; tab < NUM_TABS; tab++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:kTabTitles[tab]];
        item.label = kTabTitles[tab];
        NSView *content = pages[(NSUInteger)tab];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[tab] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("PMD85_DEBUGGER_TAB"));
    if (first >= 0 && first < (int)_tabs.numberOfTabViewItems)
        [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];
    NSStackView *root = vstack(@[toolbar, toolbar2, _status, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [toolbar2.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_status.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    if (![_window setFrameUsingName:@"PMD85Debugger"])
        [_window center];
    [_window setFrameAutosaveName:@"PMD85Debugger"];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console)
        [self appendConsole:[text stringByAppendingString:@"\n"]];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = pmd85debug_generation(_dbg);
    if (pmd85debug_is_paused(_dbg))
        pmd85debug_resume(_dbg);
    else
        pmd85debug_pause(_dbg);
    [self moved:gen];
}

- (void)step:(id)sender
{
    (void)sender;
    const unsigned gen = pmd85debug_generation(_dbg);
    pmd85debug_step(_dbg);
    [self moved:gen];
}

- (void)stepOver:(id)sender
{
    (void)sender;
    const unsigned gen = pmd85debug_generation(_dbg);
    pmd85debug_step_over(_dbg);
    [self moved:gen];
}

- (void)stepOut:(id)sender
{
    (void)sender;
    const unsigned gen = pmd85debug_generation(_dbg);
    pmd85debug_step_out(_dbg);
    [self moved:gen];
}

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a disassembly line to run to" console:NO];
        return;
    }
    const unsigned gen = pmd85debug_generation(_dbg);
    pmd85debug_run_to(_dbg, (uint16_t)_selected);
    [self moved:gen];
}

/* The gutter and F9: a click on a breakpoint that is switched off (the hollow
 * dot) switches it back on; otherwise the breakpoint is set or removed. */
- (void)toggleExecBreakpoint:(uint16_t)address
{
    static pmd85debug_breakpoint list[PMD85DEBUG_MAX_BREAKPOINTS];
    const int count = pmd85debug_breakpoints_list(_dbg, list, PMD85DEBUG_MAX_BREAKPOINTS);
    for (int index = 0; index < count; index++)
        if (list[index].type == PMD85DEBUG_BP_EXEC && !list[index].enabled && address >= list[index].start &&
            address <= list[index].end) {
            pmd85debug_breakpoint_enable(_dbg, list[index].id, 1);
            return;
        }
    if (pmd85debug_breakpoint_toggle(_dbg, address) < 0)
        [self message:@"The breakpoint table is full" console:NO];
}

/* F9: an execute breakpoint on the selected line. */
- (void)toggleSelected
{
    if (_selected < 0) {
        [self message:@"Select a disassembly line first" console:NO];
        return;
    }
    [self toggleExecBreakpoint:(uint16_t)_selected];
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    pmd85debug_breakpoint_clear_all(_dbg);
    [self refreshNow];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow)
        [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *command = trimmed(_conIn.stringValue);
    if (command.length == 0)
        return;
    if (![_history.lastObject isEqualToString:command])
        [_history addObject:command];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", command]];
    out[0] = '\0';
    const unsigned gen = pmd85debug_generation(_dbg);
    const BOOL wasPaused = pmd85debug_is_paused(_dbg) != 0;
    pmd85debug_command(_dbg, command.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length)
        [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    if (wasPaused)
        [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus (the engine
 * completes the end of the whole line: a command, then its arguments), and
 * Up/Down walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn)
        return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger count = (NSInteger)_history.count;
        if (count == 0)
            return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0)
            _historyPos = 0;
        if (_historyPos > count)
            _historyPos = count;
        _conIn.stringValue = _historyPos < count ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:))
        return NO;
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char completions[4096];
    completions[0] = '\0';
    const int count = pmd85debug_completions(_dbg, text.UTF8String, completions, (int)sizeof completions);
    NSString *list = cstr(completions);
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    for (NSString *line in [list componentsSeparatedByString:@"\n"])
        if (line.length)
            [candidates addObject:line];
    if (count == 1 && candidates.count) {
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, candidates[0]];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (count > 1 && candidates.count) {
        /* the common start is filled in; the candidates are listed */
        NSString *common = candidates[0];
        for (NSString *candidate in candidates)
            common = [common commonPrefixWithString:candidate options:NSLiteralSearch];
        if (common.length > word.length) {
            _conIn.stringValue = [head stringByAppendingString:common];
            [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        }
        [self appendConsole:[[candidates componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"]];
    }
    return YES;
}

/* The I/O port filter and the symbol filter apply as they are typed. */
- (void)controlTextDidChange:(NSNotification *)note
{
    if (note.object == _ioFilter)
        [self refreshIolog:YES];
    else if (note.object == _symFilter)
        [self symFilterChanged:nil];
}

/* An open panel with no content-type filter: symbol files have no registered
 * type, and leaving allowedContentTypes unset shows every file. */
- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols";
    panel.message = @"NAME EQU addr lines, a z88dk .map, or NAME ADDR lines";
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    const int loaded = pmd85debug_symbols_load(_dbg, panel.URL.path.UTF8String);
    if (loaded < 0)
        [self message:[NSString stringWithFormat:@"Could not open the symbol file %@", panel.URL.path] console:YES];
    else
        [self message:[NSString stringWithFormat:@"Loaded %d symbol%s from %@", loaded, loaded == 1 ? "" : "s",
                                                 panel.URL.lastPathComponent]
              console:YES];
    _symSig = nil;
    [self refreshNow];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    pmd85debug_symbols_clear(_dbg);
    [self message:@"Symbols cleared (the monitor's stay)" console:YES];
    _symSig = nil;
    [self refreshNow];
}

/* A Symbols-tab edit: name = address. */
- (void)defineSymbol:(id)sender
{
    (void)sender;
    NSString *name = trimmed(_symName.stringValue);
    NSString *why = nil;
    const int address = [self resolveAddr:_symAddr.stringValue why:&why];
    if (name.length == 0 || address < 0) {
        [self message:@"Define: a label name and an address" console:NO];
        return;
    }
    pmd85debug_symbol_add(_dbg, (uint16_t)address, name.UTF8String);
    _symName.stringValue = @"";
    _symAddr.stringValue = @"";
    [self message:[NSString stringWithFormat:@"%@ = %@", name, [self hex:(unsigned)address digits:4]] console:NO];
    _symSig = nil;
    [self refreshNow];
}

- (void)applyRegister:(NSTextField *)sender
{
    long value;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!pmd85debug_is_paused(_dbg)) {
        [self message:@"Stop the machine (F5) to edit the registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&value why:&why]) {
        if (!pmd85debug_regs_set(_dbg, kRegIds[sender.tag], (unsigned)value))
            [self message:[NSString stringWithFormat:@"%@ was not set", kRegNames[sender.tag]] console:NO];
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshNow];
}

- (void)flagClicked:(NSButton *)sender
{
    if (sender.tag >= 0 && sender.tag < NUM_FLAGS && pmd85debug_is_paused(_dbg))
        pmd85debug_flag_set(_dbg, kFlagMasks[sender.tag], sender.state == NSControlStateValueOn);
    else
        [self message:@"Stop the machine (F5) to edit the flags" console:NO];
    [self refreshNow];
}

- (void)syntaxChanged:(id)sender
{
    (void)sender;
    pmd85debug_set_syntax(_dbg, (i8080_syntax)_syntax.indexOfSelectedItem);
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    _ioSig = ~(uint64_t)0;
    _bpSig = nil;
    [self refreshNow];
}

- (void)hexStyleChanged:(id)sender
{
    (void)sender;
    pmd85debug_set_hex_style(_dbg, (i8080_hex)_hexStyle.indexOfSelectedItem);
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    _ioSig = ~(uint64_t)0;
    _bpSig = nil;
    _wtSig = nil;
    _symSig = nil;
    [self refreshNow];
}

/* ---- Memory ----------------------------------------------------------------- */

- (void)setMemBase:(long)base
{
    if (base > 0x10000 - MEM_BYTES)
        base = 0x10000 - MEM_BYTES;
    if (base < 0)
        base = 0;
    _memBase = (uint32_t)base & ~0xFu;
    [self refreshMem];
}

/* The page holding a range, the range highlighted (a little context above it). */
- (void)showMemoryLow:(uint32_t)low high:(uint32_t)high
{
    if (high < low)
        high = low;
    _memHl = YES;
    _memHlLo = low;
    _memHlHi = high;
    [self setMemBase:((long)low & ~0xF) - 4 * 16];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint32_t low, high;
    if (!pmd85sym_parse_range(_dbg, PMD85DEBUG_SPACE_CPU, _memGoto.stringValue.UTF8String, &low, &high)) {
        [self message:[NSString stringWithFormat:@"Go to %@: not an address, a symbol or a range",
                                                 _memGoto.stringValue]
              console:NO];
        return;
    }
    [self showMemoryLow:low high:high];
}

- (void)memPrev:(id)sender
{
    (void)sender;
    [self setMemBase:(long)_memBase - MEM_BYTES];
}

- (void)memNext:(id)sender
{
    (void)sender;
    [self setMemBase:(long)_memBase + MEM_BYTES];
}

/* "address value [value...]": the values land at consecutive addresses; ROM
 * bytes are skipped by the engine. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *word in [_memWrite.stringValue componentsSeparatedByCharactersInSet:
                                [NSCharacterSet characterSetWithCharactersInString:@" \t:=,"]])
        if (word.length)
            [tokens addObject:word];
    NSString *why = nil;
    const int first = tokens.count >= 2 ? [self resolveAddr:tokens[0] why:&why] : -1;
    if (first < 0) {
        [self message:why ?: @"Poke takes an address and hex bytes, e.g. 0100 41 42" console:NO];
        return;
    }
    uint8_t bytes[POKE_MAX];
    int count = 0;
    for (NSUInteger index = 1; index < tokens.count && count < POKE_MAX; index++) {
        long value;
        if (!parseNum(tokens[index], &value) || value < 0 || value > 0xFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tokens[index]] console:NO];
            return;
        }
        bytes[count++] = (uint8_t)value;
    }
    if (!pmd85debug_is_paused(_dbg)) {
        [self message:@"Stop the machine (F5) to poke memory" console:NO];
        return;
    }
    if (pmd85debug_mem_write(_dbg, PMD85DEBUG_SPACE_CPU, (uint32_t)first, bytes, count) < count)
        [self message:@"Only RAM takes a poke: ROM was skipped" console:NO];
    _memWrite.stringValue = @"";
    [self showMemoryLow:(uint32_t)first high:(uint32_t)first + (uint32_t)(count - 1)];
}

/* Find Next / Find Previous: from the highlight (else the top of the page). */
- (void)searchBackward:(BOOL)backward
{
    uint8_t pattern[PMD85SEARCH_MAX], mask = 0xFF;
    int length = 0, flags = 0;
    if (!pmd85search_parse((int)_srchKind.indexOfSelectedItem == 1 ? PMD85SEARCH_TEXT : PMD85SEARCH_HEX,
                           _srchEntry.stringValue.UTF8String, pattern, &length, &mask, &flags) ||
        length <= 0) {
        _srchNote.stringValue = @"Type hex bytes (86 41) or some text";
        return;
    }
    if (!pmd85debug_is_paused(_dbg)) {
        _srchNote.stringValue = @"Stop the machine first";
        return;
    }
    uint32_t from;
    if (_memHl)
        from = _memHlLo;
    else if (backward)
        from = _memBase;
    else
        from = _memBase ? _memBase - 1 : 0xFFFF;
    uint32_t found = 0;
    const int result = pmd85debug_search_next(_dbg, PMD85DEBUG_SPACE_CPU, pattern, length, mask, flags, from,
                                              backward ? 1 : 0, &found);
    if (result > 0) {
        [self showMemoryLow:found high:found + (uint32_t)length - 1];
        _srchNote.stringValue = [@"Found at " stringByAppendingString:[self hex:found digits:4]];
    } else if (result == 0) {
        _srchNote.stringValue = @"Not found";
    } else {
        _srchNote.stringValue = @"Stop the machine first";
    }
}

- (void)searchNext:(id)sender
{
    (void)sender;
    [self searchBackward:NO];
}

- (void)searchPrev:(id)sender
{
    (void)sender;
    [self searchBackward:YES];
}

/* ---- Disassembly -------------------------------------------------------------- */

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)addr];
}

/* The Disassembly from an address on (off the PC, until the next stop). */
- (void)showDisassemblyAt:(uint16_t)addr
{
    _disasmTop = addr;
    _followPc.state = NSControlStateValueOff;
    [self showTab:TAB_DISASM];
    [self refreshDisasm];
}

/* The toolbar's "Break at": an execute breakpoint at a symbol or an address
 * (one already there is left as it is). */
- (void)breakAt:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_breakAt.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Break at %@: %@", _breakAt.stringValue, why] console:NO];
        return;
    }
    [self breakExecuteAt:(uint16_t)addr];
    _breakAt.stringValue = @"";
}

- (void)breakExecuteAt:(uint16_t)addr
{
    if (pmd85debug_breakpoint_is_set(_dbg, addr)) {
        [self message:[@"A breakpoint is already set at " stringByAppendingString:[self hex:addr digits:4]]
              console:NO];
        return;
    }
    if (pmd85debug_breakpoint_toggle(_dbg, addr) < 0)
        [self message:@"The breakpoint table is full" console:NO];
    else
        [self message:[@"Breakpoint at " stringByAppendingString:[self hex:addr digits:4]] console:NO];
    [self refreshNow];
}

/* ---- Symbols ------------------------------------------------------------------ */

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    _symSelected = -1;
    [self refreshSymbols:NO];
}

- (void)symShow:(id)sender
{
    (void)sender;
    if (_symSelected < 0 || _symSelected >= (int)_symAddrs.count) {
        [self message:@"Click a symbol first" console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)_symAddrs[(NSUInteger)_symSelected].intValue];
}

- (void)symBreak:(id)sender
{
    (void)sender;
    if (_symSelected < 0 || _symSelected >= (int)_symAddrs.count) {
        [self message:@"Click a symbol first" console:NO];
        return;
    }
    [self breakExecuteAt:(uint16_t)_symAddrs[(NSUInteger)_symSelected].intValue];
}

/* ---- Breakpoints --------------------------------------------------------------- */

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[7] = {
        PMD85DEBUG_BP_EXEC, PMD85DEBUG_BP_READ, PMD85DEBUG_BP_WRITE, PMD85DEBUG_BP_ACCESS,
        PMD85DEBUG_BP_IN, PMD85DEBUG_BP_OUT, PMD85DEBUG_BP_IO
    };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 7 ? sel : 0];
    NSString *why = nil;
    long start, end;
    if (![self evaluate:_bpStart.stringValue into:&start why:&why]) {
        _bpErr.stringValue = why;
        return;
    }
    end = start;
    if (trimmed(_bpEnd.stringValue).length && ![self evaluate:_bpEnd.stringValue into:&end why:&why]) {
        _bpErr.stringValue = why;
        return;
    }
    const long limit = (type & PMD85DEBUG_BP_IO) ? 0xFF : 0xFFFF;
    if (start < 0 || end > limit) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Out of range (0 to %@)",
                                                        [self hex:(unsigned)limit digits:limit > 0xFF ? 4 : 2]];
        return;
    }
    if (end < start) {
        _bpErr.stringValue = @"The end comes before the start";
        return;
    }
    if (pmd85debug_breakpoint_add(_dbg, type, (uint32_t)start, (uint32_t)end,
                                  trimmed(_bpCond.stringValue).UTF8String) < 0) {
        _bpErr.stringValue = (type & PMD85DEBUG_BP_IO)
                                 ? @"The breakpoint was refused (a port is 0-FF; does the condition parse?)"
                                 : @"The breakpoint was refused (does the condition parse?)";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    pmd85debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    pmd85debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearHits:(id)sender
{
    (void)sender;
    pmd85debug_breakpoint_clear_hits(_dbg);
    [self refreshNow];
}

/* ---- Watches, I/O, trace, FujiNet ---------------------------------------------- */

- (void)addWatch:(id)sender
{
    (void)sender;
    NSString *text = trimmed(_wtEntry.stringValue);
    if (text.length == 0 || pmd85watch_add(text.UTF8String, PMD85DEBUG_SPACE_CPU) < 0) {
        [self message:@"Type an address, a symbol, an expression or a lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
    if ([self tabVisible:TAB_MEMORY])
        [self refreshMem];
}

/* A Write breakpoint over low..high that fires when the value changes. */
- (void)breakOnChangeLow:(uint32_t)low high:(uint32_t)high
{
    if (pmd85debug_breakpoint_add(_dbg, PMD85DEBUG_BP_WRITE, low, high, "value != old") < 0) {
        [self message:@"The breakpoint was refused" console:NO];
        return;
    }
    [self message:@"Breaking when it changes" console:NO];
    [self refreshNow];
}

/* Selects a tab, or raises the window it was popped out into. */
- (void)showTab:(int)tab
{
    if (_popped[tab]) {
        [_popped[tab] makeKeyAndOrderFront:nil];
        return;
    }
    [_tabs selectTabViewItem:_items[tab]];
}

- (void)wtMemory:(NSButton *)sender
{
    uint32_t low, high;
    if (!pmd85watch_range(_dbg, (int)sender.tag, &low, &high)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self showTab:TAB_MEMORY];
    [self showMemoryLow:low high:high];
}

- (void)wtBreak:(NSButton *)sender
{
    uint32_t low, high;
    if (!pmd85watch_range(_dbg, (int)sender.tag, &low, &high)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self breakOnChangeLow:low high:high];
}

- (void)wtRemove:(NSButton *)sender
{
    pmd85watch_remove((int)sender.tag);
    [self refreshWatches];
}

- (void)ioOn:(id)sender
{
    (void)sender;
    pmd85debug_io_log_enable(_dbg, _ioOn.state == NSControlStateValueOn);
    [self refreshIolog:YES];
}

- (void)ioClear:(id)sender
{
    (void)sender;
    pmd85debug_io_log_clear(_dbg);
    [self refreshIolog:YES];
}

- (void)ioFilterChanged:(id)sender
{
    (void)sender;
    [self refreshIolog:YES];
}

/* Both Record buttons follow the engine's one recorder. */
- (void)syncRecord:(BOOL)active
{
    for (int which = 0; which < 2; which++) {
        const NSInteger want = active ? NSControlStateValueOn : NSControlStateValueOff;
        if (_hRec[which].state != want) {
            _hRec[which].state = want;
            setButtonAccent(_hRec[which], active);
        }
    }
}

- (void)histRec:(NSButton *)sender
{
    pmd85debug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self syncRecord:pmd85debug_trace_enabled(_dbg) != 0];
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    [self refreshTab];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    [self refreshTab];
}

- (void)histClear:(id)sender
{
    (void)sender;
    pmd85debug_trace_clear(_dbg);
    _hSig[0] = _hSig[1] = ~(uint64_t)0;
    [self refreshTab];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    pmd85debug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)fujiHideReady:(id)sender
{
    (void)sender;
    [self refreshFujinet:YES];
}

/* ---- pop out / dock ------------------------------------------------------------ */

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    const NSInteger tab = [self currentTab];
    if (tab >= 0)
        [self popOutTab:(int)tab];
}

/* Moves a tab into a window of its own. One tab always stays. */
- (void)popOutTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS || _popped[tab] || _tabs.numberOfTabViewItems <= 1)
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [@"Debugger - " stringByAppendingString:kTabTitles[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSView *root = [[NSView alloc] init];
    dockBtn.translatesAutoresizingMaskIntoConstraints = NO;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:dockBtn];
    [root addSubview:view];
    [dockBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [dockBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:8].active = YES;
    [view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8].active = YES;
    [view.topAnchor constraintEqualToAnchor:dockBtn.bottomAnchor constant:6].active = YES;
    [view.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8].active = YES;
    win.contentView = root;
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self refreshTab];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

/* Puts a popped-out tab back where it was in the order. */
- (void)dockTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS)
        return;
    NSWindow *win = _popped[tab];
    if (!win)
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int position = 0;
    _popped[tab] = nil;
    win.delegate = nil;
    [view removeFromSuperview];
    view.translatesAutoresizingMaskIntoConstraints = YES;     /* the tab view sizes it again */
    item.view = view;
    for (int before = 0; before < tab; before++)
        if (!_popped[before])
            position++;
    [_tabs insertTabViewItem:item atIndex:position];
    [_tabs selectTabViewItem:item];
    [win orderOut:nil];
    [self refreshTab];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status, the registers and
 * the visible tabs. The poll compares against what was seen here. */
- (void)refreshNow
{
    pmd85debug_stop stop;
    pmd85debug_stop_snapshot(_dbg, &stop);
    _seenGen = pmd85debug_generation(_dbg);
    _wasPaused = stop.paused;
    if (stop.serial != _seenStop) {
        /* a new stop: every view goes back to where the machine is */
        _seenStop = stop.serial;
        if (stop.paused)
            [self recentreViews];
    }
    _liveTicks = 0;
    [self refreshStatus:&stop];
    [self refreshTab];
}

/* The registers, and each tab on show (the tab view's page and any popped-out
 * window): the others refresh when they are selected. */
- (void)refreshTab
{
    [self refreshCpu];
    for (int tab = 0; tab < NUM_TABS; tab++)
        if ([self tabVisible:tab])
            [self refreshTabAt:tab];
}

- (void)refreshTabAt:(int)tab
{
    switch (tab) {
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_MEMORY: [self refreshMem]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_WATCHES: [self refreshWatches]; break;
    case TAB_IO:
        [self refreshIo];
        [self refreshIolog:NO];
        break;
    case TAB_TRACE: [self refreshHist:0]; break;
    case TAB_HISTORY: [self refreshHist:1]; break;
    case TAB_SYMBOLS: [self refreshSymbols:NO]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus:(const pmd85debug_stop *)stop
{
    _status.stringValue = stopLine(stop, @"Running");
    _runBtn.title = stop->paused ? @"Run (F5)" : @"Pause (F5)";
    setButtonAccent(_runBtn, stop->paused);    /* the accent while stopped */
}

/* The register strip. Blank while running: the machine can only be read
 * while it is parked. */
- (void)refreshCpu
{
    pmd85debug_regs regs;
    memset(&regs, 0, sizeof regs);
    const BOOL paused = pmd85debug_regs_get(_dbg, &regs) != 0;
    const unsigned values[NUM_REGS] = {
        regs.pc, regs.sp, regs.a, regs.f, (unsigned)(regs.b << 8 | regs.c), (unsigned)(regs.d << 8 | regs.e),
        (unsigned)(regs.h << 8 | regs.l)
    };
    for (int field = 0; field < NUM_REGS; field++) {
        if (_reg[field].currentEditor)
            continue;                       /* never over a field being edited */
        _reg[field].stringValue = paused ? [NSString stringWithFormat:@"%0*X", kRegDigits[field], values[field]] : @"";
        _reg[field].enabled = paused;
    }
    for (int flag = 0; flag < NUM_FLAGS; flag++) {
        _flag[flag].state = (paused && (regs.f & kFlagMasks[flag])) ? NSControlStateValueOn : NSControlStateValueOff;
        _flag[flag].enabled = paused;
    }
    _machine.stringValue = paused ? [NSString stringWithFormat:@"   %s, MHB8080A   %s%s",
                                               pmd85session_model_name(pmd85session_model(_session)),
                                               regs.iff ? "EI" : "DI", regs.halt ? "   HALT" : ""]
                                  : @"";
}

/* The dump a page at a time: the address, 16 bytes, the text, and the region
 * the row lies in (one pmd85debug_memory_map per refresh). The highlighted
 * range is the accent fill, the watched ranges the selection colour. */
- (void)refreshMem
{
    static pmd85debug_region regions[MAX_REGIONS];
    uint8_t data[MEM_BYTES];
    if (!pmd85debug_is_paused(_dbg)) {
        setStyledText(_mem, [[NSAttributedString alloc]
                                initWithString:@"The machine is running: stop it (F5) to read memory.\n"
                                    attributes:monoAttrs()]);
        return;
    }
    if (_memBase > 0x10000 - MEM_BYTES)
        _memBase = 0x10000 - MEM_BYTES;
    const uint32_t base = _memBase;
    memset(data, 0, sizeof data);
    const int count = pmd85debug_mem_read(_dbg, PMD85DEBUG_SPACE_CPU, base, data, MEM_BYTES);
    int regionCount = pmd85debug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0)
        regionCount = 0;
    if (regionCount > MAX_REGIONS)
        regionCount = MAX_REGIONS;

    uint32_t watchLo[PMD85WATCH_MAX], watchHi[PMD85WATCH_MAX];
    int watchCount = 0;
    for (int watch = 0; watch < pmd85watch_count() && watchCount < PMD85WATCH_MAX; watch++) {
        uint32_t low, high;
        if (pmd85watch_range(_dbg, watch, &low, &high)) {
            watchLo[watchCount] = low;
            watchHi[watchCount] = high;
            watchCount++;
        }
    }

    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];          /* the highlighted bytes' cells */
    NSMutableArray<NSValue *> *watchMarks = [NSMutableArray array];     /* the watched ones */
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint32_t rowBase = base + (uint32_t)row * 16;
        [text appendFormat:@"%04X  ", rowBase];
        for (int pass = 0; pass < 2; pass++) {
            for (int col = 0; col < 16; col++) {
                const uint32_t addr = rowBase + (uint32_t)col;
                const int index = row * 16 + col;
                NSMutableArray<NSValue *> *target = nil;
                if (_memHl && addr >= _memHlLo && addr <= _memHlHi) {
                    target = marks;
                } else {
                    for (int watch = 0; watch < watchCount; watch++)
                        if (addr >= watchLo[watch] && addr <= watchHi[watch]) {
                            target = watchMarks;
                            break;
                        }
                }
                /* a highlighted run reads as one bar: the gap after a byte is
                 * marked too when the next one is */
                if (target) {
                    const BOOL joined = pass == 0 && col < 15 && target == marks && addr < _memHlHi;
                    [target addObject:[NSValue valueWithRange:NSMakeRange(text.length, pass == 0 ? (joined ? 3 : 2) : 1)]];
                }
                const uint8_t byte = data[index];
                if (pass == 0)
                    [text appendString:index < count ? [NSString stringWithFormat:@"%02X ", byte] : @"-- "];
                else
                    [text appendFormat:@"%c", (index < count && byte >= 0x20 && byte < 0x7F) ? (char)byte : '.'];
            }
            if (pass == 0)
                [text appendString:@" "];
        }
        char region[64];
        region[0] = '\0';
        pmd85mem_region_name(regions, regionCount, (uint16_t)rowBase, region, (int)sizeof region);
        if (region[0])
            [text appendFormat:@"  %s", region];
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *value in watchMarks)
        [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                       range:value.rangeValue];
    for (NSValue *value in marks)
        [styled addAttributes:accentAttrs() range:value.rangeValue];
    setStyledText(_mem, styled);
}

/* A label gets a line of its own above its instruction; the PC's line is the
 * accent, the selected one the selection colour, labels and comments grey,
 * an enabled breakpoint's dot red and a disabled one's hollow grey. */
- (void)refreshDisasm
{
    static pmd85debug_line lines[DISASM_WINDOW];
    pmd85debug_regs regs;
    if (!pmd85debug_regs_get(_dbg, &regs)) {
        _lineCount = 0;
        setStyledText(_disasm, [[NSAttributedString alloc]
                                   initWithString:@"The machine is running: stop it (F5) to read the program.\n"
                                       attributes:monoAttrs()]);
        return;
    }
    /* following, the PC sits a third of the way down */
    if (_followPc.state == NSControlStateValueOn) {
        int top = pmd85debug_row_address(_dbg, regs.pc, -(DISASM_WINDOW / 3));
        if (top > regs.pc)
            top = 0;            /* near 0000 it wraps: start at the bottom */
        _disasmTop = (uint16_t)top;
    }
    int pcLine = -1;
    int count = pmd85debug_disassemble_ex(_dbg, _disasmTop, lines, DISASM_WINDOW, &pcLine);
    if (count < 0)
        count = 0;
    if (count > DISASM_WINDOW)
        count = DISASM_WINDOW;

    NSMutableString *text = [NSMutableString string];
    NSRange lineRange[DISASM_LINES], dimRange[DISASM_LINES];
    int gutterLine[DISASM_WINDOW];
    int line = 0, pcText = -1, selText = -1;
    for (int index = 0; index < count; index++) {
        if (lines[index].label[0]) {
            NSString *label = [NSString stringWithFormat:@"      %@:", cstr(lines[index].label)];
            lineRange[line] = NSMakeRange(text.length, label.length);
            dimRange[line] = lineRange[line];
            _lineAddr[line++] = -1;
            [text appendString:label];
            [text appendString:@"\n"];
        }
        NSString *gutter = lines[index].has_breakpoint ? @"●" : (lines[index].bp_disabled ? @"○" : @" ");
        NSMutableString *row = [NSMutableString stringWithFormat:@"%@%c %04X  %-11s %-18s", gutter,
                                lines[index].is_pc ? '>' : ' ', lines[index].address, lines[index].bytes,
                                lines[index].disasm];
        dimRange[line] = NSMakeRange(NSNotFound, 0);
        if (lines[index].comment[0]) {
            NSString *comment = [@"; " stringByAppendingString:cstr(lines[index].comment)];
            dimRange[line] = NSMakeRange(text.length + row.length, comment.length);
            [row appendString:comment];
        }
        lineRange[line] = NSMakeRange(text.length, row.length);
        gutterLine[index] = line;
        if (lines[index].address == _selected)
            selText = line;
        if (index == pcLine)
            pcText = line;
        _lineAddr[line++] = lines[index].address;
        [text appendString:row];
        [text appendString:@"\n"];
    }
    _lineCount = line;
    if (count == 0)
        [text appendString:@"(no disassembly)\n"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int index = 0; index < line; index++)
        if (dimRange[index].location != NSNotFound)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.secondaryLabelColor
                           range:dimRange[index]];
    if (selText >= 0)
        [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                       range:lineRange[selText]];
    /* the PC's line in the accent colour (over the selection) */
    if (pcText >= 0)
        [styled addAttributes:accentAttrs() range:lineRange[pcText]];
    /* the gutter: red while the breakpoint is enabled, grey off (it keeps its
     * colour on the PC's line) */
    for (int index = 0; index < count; index++)
        if (lines[index].has_breakpoint || lines[index].bp_disabled)
            [styled addAttribute:NSForegroundColorAttributeName
                           value:lines[index].has_breakpoint ? breakpointRed() : NSColor.systemGrayColor
                           range:NSMakeRange(lineRange[gutterLine[index]].location, 1)];
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshBps
{
    static pmd85debug_breakpoint bps[PMD85DEBUG_MAX_BREAKPOINTS];
    int count = pmd85debug_breakpoints_list(_dbg, bps, PMD85DEBUG_MAX_BREAKPOINTS);
    if (count < 0)
        count = 0;

    pmd85debug_stop stop;
    pmd85debug_stop_snapshot(_dbg, &stop);
    _bpStop.stringValue = stopLine(&stop, @"Running.");

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *signature = [NSMutableString string];
    NSMutableArray<NSString *> *descriptions = [NSMutableArray array];
    for (int index = 0; index < count; index++) {
        char desc[256];
        desc[0] = '\0';
        pmd85bp_describe(_dbg, &bps[index], desc, (int)sizeof desc);
        [descriptions addObject:cstr(desc)];
        [signature appendFormat:@"%d %d %u %@\n", bps[index].id, bps[index].enabled, bps[index].hits,
                                descriptions[(NSUInteger)index]];
    }
    if ([signature isEqualToString:_bpSig])
        return;
    _bpSig = signature;

    for (NSView *view in [_bpList.arrangedSubviews copy])
        [_bpList removeView:view];
    if (count == 0)
        [_bpList addArrangedSubview:[self hint:@"No breakpoints. Select a disassembly line and press F9, or add "
                                               @"one below."]];
    for (int index = 0; index < count; index++) {
        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[index].id]
                                             target:self
                                             action:@selector(bpEnabled:)];
        box.state = bps[index].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[index].id;
        box.font = monoFont();
        box.toolTip = @"Enabled";
        [box.widthAnchor constraintEqualToConstant:64].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[index].id;
        NSStackView *row = hstack(@[box, tableCell(descriptions[(NSUInteger)index], 0),
                                    tableCell([NSString stringWithFormat:@"hits %u", bps[index].hits], 90), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int count = pmd85watch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableString *signature = [NSMutableString string];
    for (int index = 0; index < count; index++) {
        char buffer[256];
        buffer[0] = '\0';
        pmd85watch_format(_dbg, index, buffer, (int)sizeof buffer);
        [lines addObject:cstr(buffer)];
        [signature appendFormat:@"%@\n", lines[(NSUInteger)index]];
    }
    [signature appendFormat:@"%u", pmd85watch_generation()];
    if ([signature isEqualToString:_wtSig])
        return;      /* a refresh must not eat a click */
    _wtSig = signature;

    for (NSView *view in [_wtList.arrangedSubviews copy])
        [_wtList removeView:view];
    if (count == 0)
        [_wtList addArrangedSubview:[self hint:@"No watches"]];
    for (int index = 0; index < count; index++) {
        NSButton *mem = [self button:@"Show in Memory" action:@selector(wtMemory:)];
        mem.tag = index;
        NSButton *brk = [self button:@"Break on Change" action:@selector(wtBreak:)];
        brk.tag = index;
        NSButton *remove = [self button:@"Remove" action:@selector(wtRemove:)];
        remove.tag = index;
        NSStackView *row = hstack(@[tableCell(lines[(NSUInteger)index], 0), mem, brk, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_wtList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_wtList.widthAnchor].active = YES;
    }
}

static void appendPio(NSMutableString *text, const char *name, const pmd85debug_pio *pio)
{
    if (!pio->valid)
        return;
    [text appendFormat:@"  %-8s CWR %02X   out PA %02X  PB %02X  PC %02X\n"
                       @"  %-8s INTE %X    in  PA %02X  PB %02X  PC %02X\n",
                       name, pio->cwr, pio->a, pio->b, pio->c, "", pio->inte, pio->in_a, pio->in_b, pio->in_c];
}

/* The model and the memory map, then the chips. */
- (void)refreshIo
{
    static pmd85debug_region regions[MAX_REGIONS];
    if (!pmd85debug_is_paused(_dbg)) {
        setText(_ioText, @"The machine is running: stop it (F5) to read the chips.", NO);
        return;
    }
    pmd85debug_io chips;
    memset(&chips, 0, sizeof chips);
    if (!pmd85debug_io_get(_dbg, &chips)) {
        setText(_ioText, @"(no machine)", NO);
        return;
    }
    NSMutableString *text = [NSMutableString stringWithFormat:@"%s   LEDs %02X%s%s%s\n\nMemory map (as mapped now)\n",
                                             pmd85session_model_name(chips.model), chips.led,
                                             chips.allram ? "   ALLRAM" : "", chips.remapped ? "   remapped" : "",
                                             chips.in_reset ? "   (reset map)" : ""];
    int regionCount = pmd85debug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0)
        regionCount = 0;
    if (regionCount > MAX_REGIONS)
        regionCount = MAX_REGIONS;
    for (int index = 0; index < regionCount; index++)
        [text appendFormat:@"  %04X-%04X  %s  %@\n", regions[index].lo, regions[index].hi,
                           regions[index].writable ? "RW" : "R-", cstr(regions[index].name)];
    [text appendString:@"\n8255 PIOs\n"];
    appendPio(text, "SYS PIO", &chips.sys);
    appendPio(text, "GPIO", &chips.gpio);
    appendPio(text, "ROM PIO", &chips.rom);
    if (chips.pit[0].valid) {
        [text appendString:@"\n8253 timer\n"];
        for (int index = 0; index < 3; index++)
            [text appendFormat:@"  CT%d  CWR %02X  init %04X  count %04X  G%d O%d%s\n", index, chips.pit[index].cwr,
                               chips.pit[index].init, chips.pit[index].value, chips.pit[index].gate,
                               chips.pit[index].out, chips.pit[index].counting ? "  run" : ""];
    }
    if (chips.usart_valid)
        [text appendFormat:@"\n8251 USART (tape)\n  mode %02X  command %02X  status %02X\n", chips.usart_mode,
                           chips.usart_command, chips.usart_status];
    [text appendString:@"\nModules and expansions\n"];
    if (chips.mega_page >= 0)
        [text appendFormat:@"  MegaModule        page %d\n", chips.mega_page];
    else
        [text appendString:@"  MegaModule        -\n"];
    if (chips.mm256 >= 0)
        [text appendFormat:@"  256K mapper       page %02X\n", chips.mm256];
    else
        [text appendString:@"  256K mapper       -\n"];
    [text appendFormat:@"  MIF 85            %s\n  PMD 32            %s\n", chips.mif85 ? "fitted" : "-",
                       chips.pmd32 ? "fitted" : "-"];
    if (chips.becker)
        [text appendFormat:@"\nBecker (DriveWire) link   %s   %d byte%s waiting for the CPU\n",
                           chips.becker_link ? "up" : "down", chips.becker_rx, chips.becker_rx == 1 ? "" : "s"];
    else
        [text appendString:@"\nBecker port: not fitted (FujiNet is off)\n"];
    setText(_ioText, text, NO);
}

/* The newest accesses, oldest first; a port filter (a port or lo-hi) narrows
 * it. Rebuilt only when the log moved on. */
- (void)refreshIolog:(BOOL)force
{
    static pmd85debug_io_access log[IOLOG_READ];
    const NSInteger want = pmd85debug_io_log_enabled(_dbg) ? NSControlStateValueOn : NSControlStateValueOff;
    if (_ioOn.state != want)
        _ioOn.state = want;
    long low = 0, high = 255;
    NSString *filter = trimmed(_ioFilter.stringValue);
    if (filter.length) {
        const NSRange dash = [filter rangeOfString:@"-" options:0 range:NSMakeRange(1, filter.length - 1)];
        NSString *lowText = dash.location == NSNotFound ? filter : [filter substringToIndex:dash.location];
        if (!parseNum(lowText, &low) ||
            (dash.location != NSNotFound && !parseNum([filter substringFromIndex:dash.location + 1], &high)) ||
            low < 0 || low > 255 || high < 0 || high > 255) {
            _ioNote.stringValue = @"Filter: a port (3E) or a range (84-87)";
            return;
        }
        if (dash.location == NSNotFound)
            high = low;
    }
    int count = pmd85debug_io_log(_dbg, log, IOLOG_READ);
    if (count < 0)
        count = 0;
    const uint64_t sig = ((uint64_t)count << 48) ^ (count ? log[0].tstates : 0) ^ (uint64_t)(low << 8 | high);
    if (!force && sig == _ioSig)
        return;
    _ioSig = sig;
    NSMutableString *text = [NSMutableString string];
    int shown = 0;
    for (int index = count - 1; index >= 0; index--) {
        if (log[index].port < low || log[index].port > high)
            continue;
        const char *name = pmd85debug_port_name(_dbg, log[index].port);
        NSString *port = [[self hex:log[index].port digits:2] stringByPaddingToLength:5 withString:@" "
                                                                      startingAtIndex:0];
        NSString *value = [[self hex:log[index].value digits:2] stringByPaddingToLength:5 withString:@" "
                                                                        startingAtIndex:0];
        [text appendFormat:@"%14llu  %04X  %-3s %@ = %@  %s\n", (unsigned long long)log[index].tstates,
                           log[index].pc, log[index].out ? "OUT" : "IN", port, value, name ? name : ""];
        shown++;
    }
    if (!shown)
        [text appendString:pmd85debug_io_log_enabled(_dbg) ? @"(nothing yet: it records while the debugger is open)"
                                                          : @"The I/O log is off."];
    setText(_ioLog, text, YES);
    _ioNote.stringValue = [NSString stringWithFormat:@"%d access%s (the newest %d kept)", shown,
                                                     shown == 1 ? "" : "es", IOLOG_READ];
}

/* The symbol table, filtered: "C000h  name", ordered as the engine filters
 * it. Its own order is unstable, so each row's address is cached when the
 * list is built and a click reads that. Rebuilt only when it changed. */
- (void)refreshSymbols:(BOOL)force
{
    static int indices[SYMBOLS_SHOWN + 1];
    const int total = pmd85debug_symbols_count(_dbg);
    NSString *filter = trimmed(_symFilter.stringValue);
    int matched = pmd85debug_symbols_filter(_dbg, filter.UTF8String, indices, SYMBOLS_SHOWN + 1);
    if (matched < 0)
        matched = 0;
    const int shown = matched > SYMBOLS_SHOWN ? SYMBOLS_SHOWN : matched;

    NSMutableString *text = [NSMutableString string];
    NSMutableArray<NSNumber *> *addrs = [NSMutableArray array];
    for (int row = 0; row < shown; row++) {
        uint16_t address = 0;
        char name[96];
        name[0] = '\0';
        if (!pmd85debug_symbols_get(_dbg, indices[row], &address, name, (int)sizeof name))
            continue;
        NSString *hex = [self hex:address digits:4];
        [text appendFormat:@"%@%@  %@\n", hex, [@"      " substringFromIndex:MIN(hex.length, (NSUInteger)6)],
                           cstr(name)];
        [addrs addObject:@(address)];
    }
    if (addrs.count == 0)
        [text appendString:@"(no symbols)\n"];
    NSString *signature = [NSString stringWithFormat:@"%@%d/%d/%d", text, matched, total, _symSelected];
    char info[200];
    info[0] = '\0';
    pmd85debug_monitor_symbols_info(_dbg, info, (int)sizeof info);
    NSString *infoText = cstr(info);
    if (infoText.length)
        infoText = [[infoText substringToIndex:1].uppercaseString stringByAppendingString:[infoText substringFromIndex:1]];
    _symInfo.stringValue = infoText;
    _symCount.stringValue = matched > SYMBOLS_SHOWN
                                ? [NSString stringWithFormat:@"%d of %d match; the first %d are shown", matched, total,
                                                             SYMBOLS_SHOWN]
                                : [NSString stringWithFormat:@"%d of %d symbol%s", matched, total, total == 1 ? "" : "s"];
    if (!force && [signature isEqualToString:_symSig])
        return;
    _symSig = signature;
    _symAddrs = addrs;
    if (_symSelected >= (int)_symAddrs.count)
        _symSelected = -1;
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    if (_symSelected >= 0) {
        NSUInteger start = 0;
        for (int row = 0; row < _symSelected; row++)
            start = NSMaxRange([text lineRangeForRange:NSMakeRange(start, 0)]);
        [styled addAttributes:accentAttrs() range:[text lineRangeForRange:NSMakeRange(start, 0)]];
    }
    setStyledText(_symList, styled);
}

/* Trace (idx 0) and History (idx 1): the newest HIST_SHOWN lines of the
 * engine's ring that pass the filter, oldest first. Trace follows the ring
 * while the machine runs; History is read once it has stopped. */
- (void)refreshHist:(int)idx
{
    static pmd85debug_trace_entry ring[HIST_RING];
    static int hit[HIST_SHOWN];
    const BOOL recording = pmd85debug_trace_enabled(_dbg) != 0;
    const BOOL stopped = pmd85debug_is_paused(_dbg) != 0;
    const int total = pmd85debug_trace_count(_dbg);
    [self syncRecord:recording];

    if (idx == 1 && !stopped) {
        _hNote[1].stringValue = @"Stop the machine to read its history.";
        _hSig[1] = ~(uint64_t)0;
        return;
    }
    pmd85debug_trace_entry newest;
    memset(&newest, 0, sizeof newest);
    pmd85debug_trace_read(_dbg, &newest, 1);
    const uint64_t sig = ((uint64_t)total << 40) ^ newest.tstates ^ (recording ? 1 : 0);
    if (sig == _hSig[idx])
        return;

    uint32_t low = 0, high = 0xFFFF;
    int filter = PMD85DEBUG_TRACE_ALL;
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1:
        filter = PMD85DEBUG_TRACE_BRANCHES;
        break;
    case 2:
        if (!pmd85sym_parse_range(_dbg, PMD85DEBUG_SPACE_CPU, _hRange[idx].stringValue.UTF8String, &low, &high)) {
            _hNote[idx].stringValue = @"Type a range: 8000-8FFF, a symbol, symbol..symbol";
            return;
        }
        filter = PMD85DEBUG_TRACE_RANGE;
        break;
    case 3:
        filter = PMD85DEBUG_TRACE_IO;
        break;
    default:
        break;
    }
    _hSig[idx] = sig;
    /* newest first: unfiltered, only the lines shown are needed */
    int count = pmd85debug_trace_read(_dbg, ring, filter == PMD85DEBUG_TRACE_ALL ? HIST_SHOWN : HIST_RING);
    if (count < 0)
        count = 0;
    int shown = 0, matched = 0;
    for (int index = 0; index < count; index++)
        if (pmd85debug_trace_match(&ring[index], filter, (uint16_t)low, (uint16_t)high)) {
            matched++;
            if (shown < HIST_SHOWN)
                hit[shown++] = index;
        }

    NSMutableString *text = [NSMutableString string];
    if (shown == 0)
        [text appendString:recording ? (total > 0 ? @"(no recorded instruction matches this filter)"
                                                  : @"(nothing recorded yet: let the machine run)")
                                     : @"Recording is off. Press Record, then let the machine run."];
    for (int pos = shown - 1; pos >= 0; pos--) {        /* oldest first */
        char line[256];
        line[0] = '\0';
        pmd85debug_trace_format(_dbg, &ring[hit[pos]], line, (int)sizeof line);
        [text appendString:cstr(line)];
        [text appendString:@"\n"];
    }
    _hNote[idx].stringValue = [NSString stringWithFormat:@"%@, %d instruction%s%@",
                                                         recording ? @"recording" : @"recording off", total,
                                                         total == 1 ? "" : "s",
                                                         matched > shown ? @", newest 2000 shown" : @""];
    setText(_hText[idx], text, YES);
}

/* An exchange's bytes as the Becker port's own debug dump has them: a line
 * per 16 bytes in one direction, OUT: (PMD to FujiNet) or IN: (back), then
 * the bytes as text. A direction up to 16 bytes past PMD85_FUJI_DIR (a
 * 256-byte payload and its command) is shown whole; a longer one stops there
 * with a count of the rest. */
static void dumpPacket(NSMutableString *text, const pmd85_fuji_packet *packet)
{
    const int inTotal = packet->length > 0 ? packet->length : 0;
    const int total[2] = { inTotal, packet->nbytes - inTotal };     /* [1]: out */
    int limit[2], shown[2] = { 0, 0 }, at = 0;
    for (int dir = 0; dir < 2; dir++)
        limit[dir] = total[dir] <= PMD85_FUJI_KEEP ? total[dir] : PMD85_FUJI_DIR;
#define OUT_AT(i) ((packet->out_bits[(i) / 8] >> ((i) % 8)) & 1)
    while (at < packet->nkept) {
        const int out = OUT_AT(at);
        int run = 0;
        uint8_t line[16];
        /* one line: up to 16 bytes that went the same way */
        while (at < packet->nkept && run < 16 && OUT_AT(at) == out) {
            if (shown[out] < limit[out]) {
                line[run++] = packet->data[at];
                shown[out]++;
            }
            at++;
            if (shown[out] == limit[out])
                break;
        }
        if (run) {
            [text appendString:out ? @"  OUT: " : @"  IN:  "];
            NSMutableString *ascii = [NSMutableString string];
            for (int col = 0; col < 16; col++) {
                if (col == 8)
                    [text appendString:@" "];
                if (col < run) {
                    [text appendFormat:@"%02X ", line[col]];
                    [ascii appendFormat:@"%c", (line[col] >= 0x20 && line[col] < 0x7F) ? (char)line[col] : '.'];
                } else {
                    [text appendString:@"   "];      /* keeps the text column in line */
                }
            }
            [text appendFormat:@" %@\n", ascii];
        }
        if (shown[out] == limit[out] && total[out] > limit[out]) {
            const int more = total[out] - limit[out];
            [text appendFormat:@"        … %d more %s byte%s\n", more, out ? "OUT" : "IN", more == 1 ? "" : "s"];
            limit[out] = -1;    /* said once; nothing more of it is shown */
        }
    }
#undef OUT_AT
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static pmd85_fuji_packet packets[FUJI_PACKETS_MAX];
    char state[2048];
    int count = pmd85debug_fujinet_trace(_dbg, packets, FUJI_PACKETS_MAX);
    if (count < 0)
        count = 0;
    const BOOL hide = _fujiHideReady.state == NSControlStateValueOn;

    if (!pmd85session_fujinet_running(_session) && !pmd85session_fujinet_external(_session))
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"%@ FujiNet on the Becker port %s, %@.",
                                 pmd85session_fujinet_external(_session) ? @"External" : @"Built-in",
                                 pmd85session_becker_endpoint(_session),
                                 pmd85session_fujinet_link(_session) ? @"connected" : @"not connected"];
    state[0] = '\0';
    pmd85debug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, cstr(state), NO);

    if (!force && count == _fujiCount && (count == 0 || packets[count - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = count;
    _fujiLast = count > 0 ? packets[count - 1].serial : 0;
    NSMutableString *text = [NSMutableString string];
    int shown = 0;
    for (int index = 0; index < count; index++) {
        if (hide && pmd85debug_fujinet_is_ready(&packets[index]))
            continue;
        if (shown++)
            [text appendString:@"\n"];
        [text appendString:cstr(packets[index].text)];
        if (packets[index].repeat > 1)
            [text appendFormat:@" ×%u", packets[index].repeat];
        [text appendString:@"\n"];
        dumpPacket(text, &packets[index]);
    }
    if (shown == 0)
        [text setString:count ? @"(only READY probes since the trace was cleared)"
                              : @"(no DriveWire traffic since the trace was cleared)"];
    setText(_fujiText, text, follow);
}

@end
