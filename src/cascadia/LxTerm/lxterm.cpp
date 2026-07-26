// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "lxterm.h"

#include "../TerminalCore/Terminal.hpp"
#include "../../renderer/inc/DummyRenderer.hpp"
#include "../../buffer/out/textBuffer.hpp"

using namespace Microsoft::Terminal::Core;

// COLORREF is 0x00BBGGRR; the ABI hands out 0x00RRGGBB.
static uint32_t _swizzle(COLORREF c) noexcept
{
    return ((c & 0x000000FFu) << 16) | (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16);
}

static uint16_t _flagsOf(const TextAttribute& attr) noexcept
{
    uint16_t f = 0;
    if (attr.IsIntense())
    {
        f |= LXTERM_FLAG_BOLD;
    }
    if (attr.IsFaint())
    {
        f |= LXTERM_FLAG_FAINT;
    }
    if (attr.IsItalic())
    {
        f |= LXTERM_FLAG_ITALIC;
    }
    if (attr.IsBlinking())
    {
        f |= LXTERM_FLAG_BLINK;
    }
    if (attr.IsInvisible())
    {
        f |= LXTERM_FLAG_INVISIBLE;
    }
    if (attr.IsCrossedOut())
    {
        f |= LXTERM_FLAG_CROSSED_OUT;
    }
    if (attr.IsReverseVideo())
    {
        f |= LXTERM_FLAG_REVERSE;
    }
    if (attr.IsOverlined())
    {
        f |= LXTERM_FLAG_OVERLINE;
    }
    const auto underline = static_cast<uint16_t>(attr.GetUnderlineStyle());
    f |= static_cast<uint16_t>((underline << LXTERM_FLAG_UNDERLINE_SHIFT) & LXTERM_FLAG_UNDERLINE_MASK);
    return f;
}

struct LxTerm
{
    Terminal terminal;
    DummyRenderer renderer{ &terminal };
    til::u8state u8state;
    std::wstring wide;
    std::string narrow;
    uint64_t seq{ 0 };

    // Frame scratch, reused across take_frame calls so the returned pointers stay
    // stable for exactly one frame.
    std::vector<LxRow> rows;
    std::vector<LxRun> runs;
    std::string pool;
    LxFrame frame{};
};

LxTerm* lxterm_create(int32_t cols, int32_t rows, int32_t scrollback)
try
{
    if (cols <= 0 || rows <= 0 || scrollback < 0)
    {
        return nullptr;
    }

    auto term = std::make_unique<LxTerm>();
    term->terminal.Create({ cols, rows }, scrollback, term->renderer);
    return term.release();
}
catch (...)
{
    return nullptr;
}

void lxterm_destroy(LxTerm* term)
{
    delete term;
}

void lxterm_write(LxTerm* term, const uint8_t* utf8, size_t len)
try
{
    if (!term || !utf8 || len == 0)
    {
        return;
    }

    const std::string_view in{ reinterpret_cast<const char*>(utf8), len };
    if (FAILED(til::u8u16(in, term->wide, term->u8state)))
    {
        return;
    }
    if (term->wide.empty())
    {
        return; // the whole chunk was an incomplete sequence
    }

    const auto lock = term->terminal.LockForWriting();
    term->terminal.Write(term->wide);
}
catch (...)
{
}

const LxFrame* lxterm_take_frame(LxTerm* term)
try
{
    if (!term)
    {
        return nullptr;
    }

    const auto lock = term->terminal.LockForWriting();

    auto& rowsOut = term->rows;
    auto& runsOut = term->runs;
    auto& pool = term->pool;
    rowsOut.clear();
    runsOut.clear();
    pool.clear();

    auto& buffer = term->terminal.GetTextBuffer();
    const auto viewport = term->terminal.GetViewport();
    const auto top = viewport.Top();
    const auto height = viewport.Height();
    const auto width = viewport.Width();

    for (auto y = top; y < top + height; ++y)
    {
        const auto& row = buffer.GetRowByOffset(y);
        const auto runStart = gsl::narrow_cast<uint32_t>(runsOut.size());

        til::CoordType col = 0;
        for (const auto& run : row.Attributes().runs())
        {
            if (col >= width)
            {
                break;
            }
            const auto end = std::min(width, col + gsl::narrow_cast<til::CoordType>(run.length));
            const auto text = row.GetText(col, end);
            const auto [fg, bg] = term->terminal.GetAttributeColors(run.value);

            LxRun out{};
            out.text.off = gsl::narrow_cast<uint32_t>(pool.size());
            if (SUCCEEDED(til::u16u8(text, term->narrow)))
            {
                pool.append(term->narrow);
            }
            out.text.len = gsl::narrow_cast<uint32_t>(pool.size()) - out.text.off;
            out.cols = gsl::narrow_cast<uint16_t>(end - col);
            out.flags = _flagsOf(run.value);
            out.fg = _swizzle(fg);
            out.bg = _swizzle(bg);
            out.hyperlink_id = run.value.GetHyperlinkId();
            runsOut.push_back(out);

            col = end;
        }

        rowsOut.push_back(LxRow{
            .row = y,
            .run_off = runStart,
            .run_count = gsl::narrow_cast<uint32_t>(runsOut.size()) - runStart,
        });
    }

    const auto& cursor = buffer.GetCursor();
    const auto cursorPos = cursor.GetPosition();

    auto& frame = term->frame;
    frame.seq = ++term->seq;
    frame.cols = width;
    frame.rows = height;
    frame.view_top = top;
    frame.buffer_rows = buffer.TotalRowCount();
    frame.cursor_x = cursorPos.x;
    frame.cursor_y = cursorPos.y;
    frame.cursor_visible = cursor.IsVisible() ? 1u : 0u;
    frame.cursor_style = gsl::narrow_cast<uint8_t>(cursor.GetType());
    frame.alt_buffer_active = 0; // TODO(#10): plumb alt-buffer state out of Terminal
    frame.rows_ptr = rowsOut.data();
    frame.rows_len = gsl::narrow_cast<uint32_t>(rowsOut.size());
    frame.runs_ptr = runsOut.data();
    frame.runs_len = gsl::narrow_cast<uint32_t>(runsOut.size());
    frame.utf8_pool = reinterpret_cast<const uint8_t*>(pool.data());
    frame.utf8_len = gsl::narrow_cast<uint32_t>(pool.size());
    return &frame;
}
catch (...)
{
    return nullptr;
}
