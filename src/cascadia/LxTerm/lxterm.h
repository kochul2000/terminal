// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// laymux_wt.dll public C ABI.
//
// Hosts a headless Microsoft::Terminal::Core::Terminal: bytes go in via
// lxterm_write, and the resulting screen state comes back out as run-encoded
// frames via lxterm_take_frame. No window, no font, no renderer backend.
//
// This header is plain C so it can be consumed directly by bindgen.

#ifndef LXTERM_H
#define LXTERM_H

#include <stdint.h>
#include <stddef.h>

#ifdef LXTERM_EXPORTS
#define LXTERM_API __declspec(dllexport)
#else
#define LXTERM_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Text attribute bits on LxRun::flags.
#define LXTERM_FLAG_BOLD 0x0001u
#define LXTERM_FLAG_ITALIC 0x0002u
#define LXTERM_FLAG_BLINK 0x0004u
#define LXTERM_FLAG_INVISIBLE 0x0008u
#define LXTERM_FLAG_CROSSED_OUT 0x0010u
#define LXTERM_FLAG_REVERSE 0x0020u
#define LXTERM_FLAG_OVERLINE 0x0040u
#define LXTERM_FLAG_FAINT 0x0080u
#define LXTERM_FLAG_UNDERLINE_MASK 0x0700u // 3 bits of UnderlineStyle
#define LXTERM_FLAG_UNDERLINE_SHIFT 8u

typedef struct LxTerm LxTerm;

// A slice into LxFrame::utf8_pool.
typedef struct LxSpan
{
    uint32_t off;
    uint32_t len;
} LxSpan;

// A horizontal run of cells sharing one TextAttribute.
//
// Runs within a row do not necessarily start at column 0 or cover the row
// contiguously: a frame reports only what changed, so place each run at its own
// column. Runs may also overlap when several dirty regions touch one row; later
// runs win.
typedef struct LxRun
{
    LxSpan text; // glyphs, UTF-8
    uint16_t col; // starting column
    uint16_t cols; // columns covered (wide glyphs count 2)
    uint16_t flags; // LXTERM_FLAG_*
    uint16_t hyperlink_id; // 0 = none
    uint32_t fg; // 0x00RRGGBB, already resolved through the color scheme
    uint32_t bg; // 0x00RRGGBB
} LxRun;

// One buffer row's worth of runs, indexing into LxFrame::runs_ptr.
typedef struct LxRow
{
    int32_t row; // absolute buffer row
    uint32_t run_off;
    uint32_t run_count;
} LxRow;

// A snapshot of everything the caller needs to draw. Owned by the LxTerm and
// valid only until the next lxterm_take_frame call on the same instance.
typedef struct LxFrame
{
    uint64_t seq;
    int32_t cols;
    int32_t rows;
    int32_t view_top; // absolute buffer row at the top of the viewport
    int32_t buffer_rows; // total rows, for scrollbar extent
    int32_t cursor_x; // absolute buffer coordinates
    int32_t cursor_y;
    uint8_t cursor_visible;
    uint8_t cursor_style;
    uint8_t alt_buffer_active;
    // 1 when the rows below are the whole viewport rather than a delta, so a
    // consumer holding a mirror of the screen should drop what it has first.
    uint8_t full_repaint;
    const LxRow* rows_ptr;
    uint32_t rows_len;
    const LxRun* runs_ptr;
    uint32_t runs_len;
    const uint8_t* utf8_pool;
    uint32_t utf8_len;
} LxFrame;

// Returns NULL on failure. scrollback must be at least 1: with no scrollback the
// main and alternate buffers are the same size, and alt_buffer_active is derived
// from that difference.
LXTERM_API LxTerm* lxterm_create(int32_t cols, int32_t rows, int32_t scrollback);
LXTERM_API void lxterm_destroy(LxTerm* term);

// Feeds PTY output. Incomplete UTF-8 sequences are held internally until the
// remaining bytes arrive.
LXTERM_API void lxterm_write(LxTerm* term, const uint8_t* utf8, size_t len);

// Resizes the viewport. The main buffer reflows; the next frame is a full repaint.
LXTERM_API void lxterm_resize(LxTerm* term, int32_t cols, int32_t rows);

// Scrolls the viewport so that view_top is the topmost visible buffer row. It is
// clamped to the buffer, and has no effect while the alternate buffer is active.
LXTERM_API void lxterm_user_scroll(LxTerm* term, int32_t view_top);

// Renders whatever changed since the last call. Never returns NULL for a valid
// handle; when nothing changed, rows_len is 0 and the rest of the frame still
// describes the current state.
LXTERM_API const LxFrame* lxterm_take_frame(LxTerm* term);

#ifdef __cplusplus
}
#endif

#endif // LXTERM_H
