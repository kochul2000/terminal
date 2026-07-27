// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "LxEngine.h"

#include "../../buffer/out/textBuffer.hpp"
#include "../../types/inc/GlyphWidth.hpp"

using namespace Microsoft::Console::Render;
using namespace laymux;

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

til::size LxEngine::ViewportSize() const noexcept
{
    return { _viewport.right - _viewport.left + 1, _viewport.bottom - _viewport.top + 1 };
}

#pragma region Frame lifecycle

[[nodiscard]] HRESULT LxEngine::StartPaint() noexcept
try
{
    _framePainted = false;
    _framePaintedAll = false;

    if (!_invalidAll && _invalid.empty() && !_cursorDirty)
    {
        // Nothing changed. S_FALSE tells the Renderer to skip the rest of the frame.
        return S_FALSE;
    }

    _rows.clear();
    _runs.clear();
    _pool.clear();

    _frameDirty.clear();
    if (_invalidAll)
    {
        const auto size = ViewportSize();
        _frameDirty.push_back(til::rect{ 0, 0, size.width, size.height });
    }
    else
    {
        _frameDirty = _invalid;
    }

    _framePainted = true;
    _framePaintedAll = _invalidAll;
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::EndPaint() noexcept
try
{
    _invalid.clear();
    _invalidAll = false;
    _cursorDirty = false;
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::Present() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::ScrollFrame() noexcept
{
    return S_OK;
}

#pragma endregion

#pragma region Invalidation

[[nodiscard]] HRESULT LxEngine::Invalidate(const til::rect* psrRegion) noexcept
try
{
    if (!_invalidAll && psrRegion && *psrRegion)
    {
        _invalid.push_back(*psrRegion);
    }
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::InvalidateCursor(const til::rect* psrRegion) noexcept
{
    // Not forwarded to Invalidate: the cursor is a coordinate in LxFrame, not ink
    // in a cell, so the cell underneath it never needs repainting on its account.
    // The Renderer calls this with the old position and then the new one every
    // frame, so only a rect we have not already seen means the cursor moved.
    if (psrRegion && *psrRegion != _lastCursorInvalidation)
    {
        _lastCursorInvalidation = *psrRegion;
        _cursorDirty = true;
    }
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::InvalidateSystem(const til::rect* prcDirtyClient) noexcept
{
    // There is no client area to lose; a system repaint carries no information
    // an in-memory consumer does not already have.
    UNREFERENCED_PARAMETER(prcDirtyClient);
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::InvalidateScroll(const til::point* pcoordDelta) noexcept
{
    // Rows are reported in absolute buffer coordinates, so a consumer can follow a
    // scroll from view_top alone -- but the rows newly brought into view still have
    // to be painted, and working out exactly which ones is not worth it next to a
    // viewport-sized repaint.
    if (pcoordDelta && (pcoordDelta->x != 0 || pcoordDelta->y != 0))
    {
        return InvalidateAll();
    }
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::InvalidateAll() noexcept
try
{
    _invalidAll = true;
    _invalid.clear();
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::GetDirtyArea(std::span<const til::rect>& area) noexcept
{
    area = _frameDirty;
    return S_OK;
}

#pragma endregion

#pragma region Painting

[[nodiscard]] HRESULT LxEngine::PaintBackground() noexcept
{
    return S_OK;
}

LxRow& LxEngine::_rowFor(til::CoordType absoluteRow)
{
    // The Renderer walks rows in order within a dirty rect, so the row being
    // painted is almost always the one just appended. Overlapping dirty rects can
    // revisit an earlier row, hence the search.
    for (auto it = _rows.rbegin(); it != _rows.rend(); ++it)
    {
        if (it->row == absoluteRow)
        {
            return *it;
        }
    }

    _rows.push_back(LxRow{
        .row = absoluteRow,
        .run_off = gsl::narrow_cast<uint32_t>(_runs.size()),
        .run_count = 0,
    });
    return _rows.back();
}

[[nodiscard]] HRESULT LxEngine::PaintBufferLine(std::span<const Cluster> clusters, til::point coord, bool fTrimLeft, bool lineWrapped) noexcept
try
{
    // TODO(#5): fTrimLeft means this run begins on the right half of a wide glyph
    // that starts before the dirty region. Reporting the whole glyph is harmless
    // for a consumer that redraws by cell, but it does overstate the run's width.
    UNREFERENCED_PARAMETER(fTrimLeft);
    UNREFERENCED_PARAMETER(lineWrapped);

    if (clusters.empty())
    {
        return S_OK;
    }

    auto& row = _rowFor(coord.y + _viewport.top);

    // A row's runs have to be contiguous in _runs for run_off/run_count to address
    // them. Overlapping dirty rects could interleave rows; when that happens, start
    // the row over at the end of the array rather than emitting a torn range.
    if (row.run_off + row.run_count != _runs.size())
    {
        row.run_off = gsl::narrow_cast<uint32_t>(_runs.size());
        row.run_count = 0;
    }

    LxRun out{};
    out.text.off = gsl::narrow_cast<uint32_t>(_pool.size());
    til::CoordType columns = 0;
    for (const auto& cluster : clusters)
    {
        if (SUCCEEDED(til::u16u8(cluster.GetText(), _narrow)))
        {
            _pool.append(_narrow);
        }
        columns += cluster.GetColumns();
    }
    out.text.len = gsl::narrow_cast<uint32_t>(_pool.size()) - out.text.off;
    out.col = gsl::narrow_cast<uint16_t>(std::max(0, coord.x));
    out.cols = gsl::narrow_cast<uint16_t>(columns);
    out.flags = _flags;
    out.fg = _fg;
    out.bg = _bg;
    out.hyperlink_id = _hyperlinkId;
    _runs.push_back(out);
    row.run_count++;

    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::PaintBufferGridLines(GridLineSet lines, COLORREF gridlineColor, COLORREF underlineColor, size_t cchLine, til::point coordTarget) noexcept
{
    // Underline and strikethrough already reach the consumer through LxRun::flags,
    // which it can render however it likes. The box-drawing grid lines are a
    // conhost feature with no equivalent on our side.
    UNREFERENCED_PARAMETER(lines);
    UNREFERENCED_PARAMETER(gridlineColor);
    UNREFERENCED_PARAMETER(underlineColor);
    UNREFERENCED_PARAMETER(cchLine);
    UNREFERENCED_PARAMETER(coordTarget);
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::PaintImageSlice(const ImageSlice& imageSlice, til::CoordType targetRow, til::CoordType viewportLeft) noexcept
{
    // TODO: sixel / iTerm images have no representation in LxFrame yet.
    UNREFERENCED_PARAMETER(imageSlice);
    UNREFERENCED_PARAMETER(targetRow);
    UNREFERENCED_PARAMETER(viewportLeft);
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::PaintSelection(const til::rect& rect) noexcept
{
    // TODO: selection is driven by laymux's own UI today, not by Terminal.
    UNREFERENCED_PARAMETER(rect);
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::PaintCursor(const CursorOptions& options) noexcept
{
    // Recorded for the cursor's rendering options (color, height, double width),
    // none of which the ABI carries yet. The position does NOT come from here: the
    // Renderer skips this call entirely when the cursor is outside the viewport,
    // which would leave a stale position behind after a scroll.
    _cursor = options;
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::UpdateDrawingBrushes(const TextAttribute& textAttributes, const RenderSettings& renderSettings, gsl::not_null<IRenderData*> pData, bool usingSoftFont, bool isSettingDefaultBrushes) noexcept
try
{
    UNREFERENCED_PARAMETER(renderSettings);
    UNREFERENCED_PARAMETER(usingSoftFont);
    UNREFERENCED_PARAMETER(isSettingDefaultBrushes);

    // Resolve through IRenderData rather than the RenderSettings we were handed:
    // Terminal owns the color scheme, and it is what applies intense/faint and the
    // reverse-video swap.
    const auto [fg, bg] = pData->GetAttributeColors(textAttributes);
    _fg = _swizzle(fg);
    _bg = _swizzle(bg);
    _flags = _flagsOf(textAttributes);
    _hyperlinkId = textAttributes.GetHyperlinkId();
    return S_OK;
}
CATCH_RETURN()

#pragma endregion

#pragma region Font and geometry

// Everything here is measured in cells, so a 1x1 cell makes the Renderer's pixel
// arithmetic a no-op.
static constexpr til::size CellSize{ 1, 1 };

[[nodiscard]] HRESULT LxEngine::UpdateFont(const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo) noexcept
try
{
    fontInfo.SetFromEngine(fontInfoDesired.GetFaceName(),
                           fontInfoDesired.GetFamily(),
                           fontInfoDesired.GetWeight(),
                           false,
                           CellSize,
                           CellSize);
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT LxEngine::GetProposedFont(const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo, int iDpi) noexcept
{
    UNREFERENCED_PARAMETER(iDpi);
    return UpdateFont(fontInfoDesired, fontInfo);
}

[[nodiscard]] HRESULT LxEngine::UpdateDpi(int iDpi) noexcept
{
    UNREFERENCED_PARAMETER(iDpi);
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::UpdateViewport(const til::inclusive_rect& srNewViewport) noexcept
{
    // A resize keeps the viewport's origin, so the Renderer's InvalidateScroll
    // gets a zero delta and nothing would be marked dirty. Catch it here: any
    // change in the visible dimensions invalidates everything we last reported.
    const auto resized = (srNewViewport.right - srNewViewport.left) != (_viewport.right - _viewport.left) ||
                         (srNewViewport.bottom - srNewViewport.top) != (_viewport.bottom - _viewport.top);
    _viewport = srNewViewport;
    if (resized)
    {
        return InvalidateAll();
    }
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::GetFontSize(_Out_ til::size* pFontSize) noexcept
{
    *pFontSize = CellSize;
    return S_OK;
}

[[nodiscard]] HRESULT LxEngine::IsGlyphWideByFont(std::wstring_view glyph, _Out_ bool* pResult) noexcept
{
    // No font to measure against, so fall back to the same Unicode width tables
    // the text buffer used when it decided how many columns the glyph occupies.
    *pResult = IsGlyphFullWidth(glyph);
    return S_OK;
}

#pragma endregion

[[nodiscard]] HRESULT LxEngine::_DoUpdateTitle(std::wstring_view newTitle) noexcept
try
{
    _title = newTitle;
    return S_OK;
}
CATCH_RETURN()
