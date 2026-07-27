#include <stdio.h>
#include <string.h>
#include "lxterm.h"

static int failures = 0;

static void check(int ok, const char* what)
{
    printf("  %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
    {
        failures++;
    }
}

static void dump_frame(const LxFrame* f, int maxRows)
{
    printf("frame seq=%llu %dx%d view_top=%d buffer_rows=%d cursor=(%d,%d) vis=%u full=%u rows=%u runs=%u pool=%u\n",
           (unsigned long long)f->seq,
           f->cols,
           f->rows,
           f->view_top,
           f->buffer_rows,
           f->cursor_x,
           f->cursor_y,
           f->cursor_visible,
           f->full_repaint,
           f->rows_len,
           f->runs_len,
           f->utf8_len);

    for (uint32_t ri = 0; ri < f->rows_len && (int)ri < maxRows; ++ri)
    {
        const LxRow* r = &f->rows_ptr[ri];
        printf("  row %d: %u runs\n", r->row, r->run_count);
        for (uint32_t i = 0; i < r->run_count; ++i)
        {
            const LxRun* run = &f->runs_ptr[r->run_off + i];
            printf("    col=%u cols=%u fg=%06X bg=%06X flags=%04X text=\"%.*s\"\n",
                   run->col,
                   run->cols,
                   run->fg,
                   run->bg,
                   run->flags,
                   (int)run->text.len,
                   (const char*)(f->utf8_pool + run->text.off));
        }
    }
}

static const LxRun* first_run(const LxFrame* f, int32_t row)
{
    for (uint32_t ri = 0; ri < f->rows_len; ++ri)
    {
        if (f->rows_ptr[ri].row == row && f->rows_ptr[ri].run_count > 0)
        {
            return &f->runs_ptr[f->rows_ptr[ri].run_off];
        }
    }
    return NULL;
}

static int is_red(uint32_t rgb)
{
    return ((rgb >> 16) & 0xFF) > 0x60 && ((rgb >> 8) & 0xFF) < 0x40 && (rgb & 0xFF) < 0x40;
}

static void write_str(LxTerm* t, const char* s)
{
    lxterm_write(t, (const uint8_t*)s, strlen(s));
}

int main(void)
{
    LxTerm* t = lxterm_create(80, 25, 1000);
    if (!t)
    {
        printf("FAIL: lxterm_create returned NULL\n");
        return 1;
    }

    /* --- first frame: red text, and a full repaint of the viewport --- */
    write_str(t, "\x1b[31mhi");
    const LxFrame* f = lxterm_take_frame(t);
    if (!f)
    {
        printf("FAIL: lxterm_take_frame returned NULL\n");
        return 1;
    }
    printf("--- frame 1: after \\e[31mhi ---\n");
    dump_frame(f, 1);

    const LxRun* run = first_run(f, 0);
    check(run != NULL, "row 0 has a run");
    if (run)
    {
        // The Renderer folds the trailing blanks into the preceding run: spaces
        // have no foreground to distinguish, so this is one run of 80 columns.
        const char* text = (const char*)(f->utf8_pool + run->text.off);
        check(run->col == 0, "first run starts at column 0");
        check(run->text.len >= 2 && text[0] == 'h' && text[1] == 'i', "first run begins with \"hi\"");
        check(is_red(run->fg), "first run is red");
    }
    check(f->full_repaint == 1, "first frame is a full repaint");
    check(f->rows_len == 25, "first frame covers the whole viewport");
    check(f->cursor_x == 2 && f->cursor_y == 0, "cursor sits after \"hi\"");
    check(f->buffer_rows == 1025, "buffer is viewport + scrollback");

    /* --- second frame: one more glyph should not repaint the viewport --- */
    write_str(t, "\x1b[mX");
    f = lxterm_take_frame(t);
    printf("--- frame 2: after \\e[mX ---\n");
    dump_frame(f, 4);
    check(f->full_repaint == 0, "second frame is a delta");
    check(f->rows_len == 1 && f->rows_ptr[0].row == 0, "second frame touches only row 0");
    run = first_run(f, 0);
    check(run != NULL && run->col == 2, "the delta starts at the column that changed");
    if (run)
    {
        const char* text = (const char*)(f->utf8_pool + run->text.off);
        check(run->text.len >= 1 && text[0] == 'X', "the delta carries the new glyph");
    }
    check(f->cursor_x == 3 && f->cursor_y == 0, "cursor advanced past X");

    /* --- third frame: nothing was written, so nothing is dirty --- */
    f = lxterm_take_frame(t);
    printf("--- frame 3: no writes ---\n");
    dump_frame(f, 0);
    check(f->rows_len == 0, "idle frame is empty");
    check(f->cols == 80 && f->rows == 25, "idle frame still reports the viewport");

    /* --- scrolling past the bottom moves the viewport --- */
    for (int i = 0; i < 40; ++i)
    {
        write_str(t, "line\r\n");
    }
    f = lxterm_take_frame(t);
    printf("--- frame 4: after 40 lines ---\n");
    dump_frame(f, 0);
    check(f->view_top == 16, "viewport scrolled by 40 - 24");
    check(f->rows_len > 0, "scrolled frame reports rows");
    if (f->rows_len > 0)
    {
        int32_t minRow = f->rows_ptr[0].row;
        for (uint32_t ri = 1; ri < f->rows_len; ++ri)
        {
            if (f->rows_ptr[ri].row < minRow)
            {
                minRow = f->rows_ptr[ri].row;
            }
        }
        check(minRow >= f->view_top, "reported rows are absolute buffer rows inside the viewport");
    }

    /* --- scrolling back up moves the viewport without touching the buffer --- */
    lxterm_user_scroll(t, 0);
    f = lxterm_take_frame(t);
    printf("--- frame 5: scrolled to the top ---\n");
    dump_frame(f, 0);
    check(f->view_top == 0, "user scroll moved the viewport to the top");
    check(f->buffer_rows == 1025, "user scroll did not change the buffer");
    check(f->cursor_y == 40, "cursor stayed on its buffer row while the view scrolled");
    check(f->cursor_visible == 0, "cursor scrolled out of view is not visible");

    /* --- resize reflows and repaints --- */
    lxterm_resize(t, 100, 30);
    f = lxterm_take_frame(t);
    printf("--- frame 6: resized to 100x30 ---\n");
    dump_frame(f, 0);
    check(f->cols == 100 && f->rows == 30, "frame reports the new viewport");
    check(f->full_repaint == 1, "resize forces a full repaint");
    check(f->rows_len == 30, "resize repaints every visible row");
    check(f->buffer_rows == 1030, "buffer height follows the viewport");

    /* --- alternate buffer --- */
    write_str(t, "\x1b[?1049h");
    f = lxterm_take_frame(t);
    printf("--- frame 7: alternate buffer ---\n");
    dump_frame(f, 0);
    check(f->alt_buffer_active == 1, "alternate buffer is reported");
    check(f->buffer_rows == 30, "alternate buffer has no scrollback");
    check(f->view_top == 0, "alternate buffer viewport starts at row 0");

    write_str(t, "\x1b[?1049l");
    f = lxterm_take_frame(t);
    printf("--- frame 8: back to the main buffer ---\n");
    dump_frame(f, 0);
    check(f->alt_buffer_active == 0, "main buffer is reported again");
    check(f->buffer_rows == 1030, "scrollback survived the alternate buffer");

    lxterm_destroy(t);

    /* --- scrollback is required, so that alt_buffer_active can be derived --- */
    check(lxterm_create(80, 25, 0) == NULL, "create rejects a zero scrollback");

    if (failures == 0)
    {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL (%d checks)\n", failures);
    return 1;
}
