#include <stdio.h>
#include <string.h>
#include "lxterm.h"

static void dump_row(const LxFrame* f, int rowIndex)
{
    const LxRow* r = &f->rows_ptr[rowIndex];
    printf("row %d (buffer row %d): %u runs\n", rowIndex, r->row, r->run_count);
    for (uint32_t i = 0; i < r->run_count; ++i)
    {
        const LxRun* run = &f->runs_ptr[r->run_off + i];
        printf("  run %u: cols=%u fg=%06X bg=%06X flags=%04X text=\"%.*s\"\n",
               i,
               run->cols,
               run->fg,
               run->bg,
               run->flags,
               (int)run->text.len,
               (const char*)(f->utf8_pool + run->text.off));
    }
}

int main(void)
{
    LxTerm* t = lxterm_create(80, 25, 1000);
    if (!t)
    {
        printf("FAIL: lxterm_create returned NULL\n");
        return 1;
    }

    const char* s = "\x1b[31mhi";
    lxterm_write(t, (const uint8_t*)s, strlen(s));

    const LxFrame* f = lxterm_take_frame(t);
    if (!f)
    {
        printf("FAIL: lxterm_take_frame returned NULL\n");
        return 1;
    }

    printf("frame seq=%llu %dx%d view_top=%d buffer_rows=%d cursor=(%d,%d) vis=%u rows_len=%u runs_len=%u pool=%u\n",
           (unsigned long long)f->seq,
           f->cols,
           f->rows,
           f->view_top,
           f->buffer_rows,
           f->cursor_x,
           f->cursor_y,
           f->cursor_visible,
           f->rows_len,
           f->runs_len,
           f->utf8_len);

    dump_row(f, 0);

    /* Expect: first run of row 0 is "hi" with a red foreground. */
    int ok = 0;
    if (f->rows_len > 0 && f->rows_ptr[0].run_count > 0)
    {
        const LxRun* run = &f->runs_ptr[f->rows_ptr[0].run_off];
        const char* text = (const char*)(f->utf8_pool + run->text.off);
        const int isHi = run->text.len == 2 && text[0] == 'h' && text[1] == 'i';
        const unsigned r = (run->fg >> 16) & 0xFF;
        const unsigned g = (run->fg >> 8) & 0xFF;
        const unsigned b = run->fg & 0xFF;
        const int isRed = r > 0x60 && g < 0x40 && b < 0x40;
        printf("check: text==\"hi\" %s, fg red (r=%u g=%u b=%u) %s\n",
               isHi ? "OK" : "NO",
               r,
               g,
               b,
               isRed ? "OK" : "NO");
        ok = isHi && isRed;
    }

    /* Also exercise a second write + frame to be sure state persists. */
    const char* s2 = "\x1b[mX";
    lxterm_write(t, (const uint8_t*)s2, strlen(s2));
    f = lxterm_take_frame(t);
    printf("--- after second write (seq=%llu) ---\n", (unsigned long long)f->seq);
    dump_row(f, 0);

    lxterm_destroy(t);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
