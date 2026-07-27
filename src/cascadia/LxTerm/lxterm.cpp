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
    til::CoordType scrollback{ 0 };

    // Handed back by lxterm_take_frame; the arrays it points at live in the engine.
    LxFrame frame{};
};

LxTerm* lxterm_create(int32_t cols, int32_t rows, int32_t scrollback)
try
{
    if (cols <= 0 || rows <= 0 || scrollback < 1)
    {
        return nullptr;
    }

    auto term = std::make_unique<LxTerm>();
    term->scrollback = scrollback;

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

void lxterm_resize(LxTerm* term, int32_t cols, int32_t rows)
try
{
    if (!term || cols <= 0 || rows <= 0)
    {
        return;
    }

    const auto lock = term->terminal.LockForWriting();
    LOG_IF_FAILED(term->terminal.UserResize({ cols, rows }));
}
catch (...)
{
}

void lxterm_user_scroll(LxTerm* term, int32_t view_top)
try
{
    if (!term)
    {
        return;
    }

    const auto lock = term->terminal.LockForWriting();
    term->terminal.UserScrollViewport(view_top);
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

    auto& buffer = term->terminal.GetTextBuffer();
    const auto bufferRows = buffer.TotalRowCount();

    // Straight from the buffer rather than from the engine: the Renderer does not
    // call PaintCursor while the cursor is scrolled out of view, so the engine's
    // copy would be whatever it was when the cursor was last visible.
    const auto& cursor = buffer.GetCursor();
    const auto cursorPos = cursor.GetPosition();
    const auto cursorInView = cursorPos.y >= viewportTop && cursorPos.y < viewportTop + viewportSize.height;

    // Terminal keeps _inAltBuffer() private, and the alternate buffer is the one
    // observable thing that has no scrollback: it is allocated at exactly the
    // viewport size. That is why lxterm_create insists on a scrollback of at least
    // one -- without it the two buffers are indistinguishable from out here.
    const auto inAltBuffer = bufferRows == viewportSize.height;

    auto& frame = term->frame;
    frame.seq = ++term->seq;
    frame.cols = viewportSize.width;
    frame.rows = viewportSize.height;
    frame.view_top = viewportTop;
    frame.buffer_rows = bufferRows;
    frame.cursor_x = cursorPos.x;
    frame.cursor_y = cursorPos.y;
    frame.cursor_visible = (cursor.IsVisible() && cursorInView) ? 1u : 0u;
    frame.cursor_style = gsl::narrow_cast<uint8_t>(cursor.GetType());
    frame.alt_buffer_active = inAltBuffer ? 1u : 0u;
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
