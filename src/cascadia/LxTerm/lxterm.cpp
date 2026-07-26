// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "lxterm.h"

#include "LxEngine.h"

#include "../TerminalCore/Terminal.hpp"
#include "../../buffer/out/textBuffer.hpp"
#include "../../renderer/base/renderer.hpp"

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;

struct LxTerm
{
    Terminal terminal;
    RenderSettings renderSettings;
    Renderer renderer{ renderSettings, &terminal };
    laymux::LxEngine engine;

    til::u8state u8state;
    std::wstring wide;
    uint64_t seq{ 0 };

    // Handed back by lxterm_take_frame; the arrays it points at live in the engine.
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

    // Deliberately no EnablePainting(): that spawns a render thread that would
    // call PaintFrame on its own schedule. Frames here are pulled, not pushed, so
    // the caller can pace them and we can paint under the terminal lock.
    term->renderer.AddRenderEngine(&term->engine);
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

    LOG_IF_FAILED(term->renderer.PaintFrame());

    const auto& engine = term->engine;
    const auto viewportSize = engine.ViewportSize();
    const auto viewportTop = engine.ViewportTop();
    const auto& cursor = engine.Cursor();

    auto& frame = term->frame;
    frame.seq = ++term->seq;
    frame.cols = viewportSize.width;
    frame.rows = viewportSize.height;
    frame.view_top = viewportTop;
    frame.buffer_rows = term->terminal.GetTextBuffer().TotalRowCount();
    // CursorOptions is viewport-relative; the ABI reports absolute buffer rows.
    frame.cursor_x = cursor.coordCursor.x;
    frame.cursor_y = cursor.coordCursor.y + viewportTop;
    frame.cursor_visible = (cursor.isVisible && cursor.inViewport) ? 1u : 0u;
    frame.cursor_style = gsl::narrow_cast<uint8_t>(cursor.cursorType);
    frame.alt_buffer_active = 0; // TODO(#10): plumb alt-buffer state out of Terminal
    frame.full_repaint = engine.FullRepaint() ? 1u : 0u;

    if (engine.Painted())
    {
        frame.rows_ptr = engine.Rows().data();
        frame.rows_len = gsl::narrow_cast<uint32_t>(engine.Rows().size());
        frame.runs_ptr = engine.Runs().data();
        frame.runs_len = gsl::narrow_cast<uint32_t>(engine.Runs().size());
        frame.utf8_pool = reinterpret_cast<const uint8_t*>(engine.Pool().data());
        frame.utf8_len = gsl::narrow_cast<uint32_t>(engine.Pool().size());
    }
    else
    {
        // Nothing was invalidated, so the engine's arrays still hold the previous
        // frame. Report an empty delta rather than replaying it.
        frame.rows_ptr = nullptr;
        frame.rows_len = 0;
        frame.runs_ptr = nullptr;
        frame.runs_len = 0;
        frame.utf8_pool = nullptr;
        frame.utf8_len = 0;
    }

    return &frame;
}
catch (...)
{
    return nullptr;
}
