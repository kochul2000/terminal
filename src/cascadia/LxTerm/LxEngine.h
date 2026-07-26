// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// A render engine that draws into memory instead of onto a screen.
//
// The Renderer hands every engine the same information an on-screen backend
// gets — dirty regions, resolved attributes, glyph clusters already grouped into
// runs, cursor placement — so implementing IRenderEngine is how you get WT's
// rendering decisions without WT's rendering. LxEngine records that stream into
// the flat arrays that lxterm.h hands across the ABI.
//
// There is no font here. All geometry is in cells, and the engine reports a 1x1
// "font" so the Renderer's pixel math degenerates into cell math.

#pragma once

#include "lxterm.h"

#include "../../renderer/inc/RenderEngineBase.hpp"

namespace laymux
{
    class LxEngine final : public Microsoft::Console::Render::RenderEngineBase
    {
    public:
        LxEngine() noexcept = default;

        // Painted state, valid between EndPaint and the next StartPaint.
        const std::vector<LxRow>& Rows() const noexcept { return _rows; }
        const std::vector<LxRun>& Runs() const noexcept { return _runs; }
        const std::string& Pool() const noexcept { return _pool; }
        bool FullRepaint() const noexcept { return _framePaintedAll; }
        bool Painted() const noexcept { return _framePainted; }

        til::CoordType ViewportTop() const noexcept { return _viewport.top; }
        til::size ViewportSize() const noexcept;

        const Microsoft::Console::Render::CursorOptions& Cursor() const noexcept { return _cursor; }
        std::wstring_view Title() const noexcept { return _title; }

        // IRenderEngine
        [[nodiscard]] HRESULT StartPaint() noexcept override;
        [[nodiscard]] HRESULT EndPaint() noexcept override;
        [[nodiscard]] HRESULT Present() noexcept override;
        [[nodiscard]] HRESULT ScrollFrame() noexcept override;

        [[nodiscard]] HRESULT Invalidate(const til::rect* psrRegion) noexcept override;
        [[nodiscard]] HRESULT InvalidateCursor(const til::rect* psrRegion) noexcept override;
        [[nodiscard]] HRESULT InvalidateSystem(const til::rect* prcDirtyClient) noexcept override;
        [[nodiscard]] HRESULT InvalidateScroll(const til::point* pcoordDelta) noexcept override;
        [[nodiscard]] HRESULT InvalidateAll() noexcept override;

        [[nodiscard]] HRESULT PaintBackground() noexcept override;
        [[nodiscard]] HRESULT PaintBufferLine(std::span<const Microsoft::Console::Render::Cluster> clusters, til::point coord, bool fTrimLeft, bool lineWrapped) noexcept override;
        [[nodiscard]] HRESULT PaintBufferGridLines(Microsoft::Console::Render::GridLineSet lines, COLORREF gridlineColor, COLORREF underlineColor, size_t cchLine, til::point coordTarget) noexcept override;
        [[nodiscard]] HRESULT PaintImageSlice(const ImageSlice& imageSlice, til::CoordType targetRow, til::CoordType viewportLeft) noexcept override;
        [[nodiscard]] HRESULT PaintSelection(const til::rect& rect) noexcept override;
        [[nodiscard]] HRESULT PaintCursor(const Microsoft::Console::Render::CursorOptions& options) noexcept override;

        [[nodiscard]] HRESULT UpdateDrawingBrushes(const TextAttribute& textAttributes, const Microsoft::Console::Render::RenderSettings& renderSettings, gsl::not_null<Microsoft::Console::Render::IRenderData*> pData, bool usingSoftFont, bool isSettingDefaultBrushes) noexcept override;
        [[nodiscard]] HRESULT UpdateFont(const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo) noexcept override;
        [[nodiscard]] HRESULT UpdateDpi(int iDpi) noexcept override;
        [[nodiscard]] HRESULT UpdateViewport(const til::inclusive_rect& srNewViewport) noexcept override;

        [[nodiscard]] HRESULT GetProposedFont(const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo, int iDpi) noexcept override;
        [[nodiscard]] HRESULT GetDirtyArea(std::span<const til::rect>& area) noexcept override;
        [[nodiscard]] HRESULT GetFontSize(_Out_ til::size* pFontSize) noexcept override;
        [[nodiscard]] HRESULT IsGlyphWideByFont(std::wstring_view glyph, _Out_ bool* pResult) noexcept override;

    protected:
        [[nodiscard]] HRESULT _DoUpdateTitle(std::wstring_view newTitle) noexcept override;

    private:
        LxRow& _rowFor(til::CoordType absoluteRow);

        // Invalidation accumulated since the last frame, in viewport-relative
        // exclusive coordinates. _invalidAll short-circuits the list.
        std::vector<til::rect> _invalid;
        bool _invalidAll{ true };

        // The Renderer invalidates the cursor cell twice a frame (old position,
        // then new) whether or not it moved. The cursor is reported as a
        // coordinate rather than painted into cells, so only an actual move is
        // worth a frame.
        til::rect _lastCursorInvalidation{};
        bool _cursorDirty{ true };

        // The dirty rects handed to the Renderer for the frame in flight.
        std::vector<til::rect> _frameDirty;

        std::vector<LxRow> _rows;
        std::vector<LxRun> _runs;
        std::string _pool;
        std::string _narrow; // scratch for UTF-16 -> UTF-8
        bool _framePainted{ false };
        bool _framePaintedAll{ false };

        // Brush state, set by UpdateDrawingBrushes just before each run.
        uint32_t _fg{ 0 };
        uint32_t _bg{ 0 };
        uint16_t _flags{ 0 };
        uint16_t _hyperlinkId{ 0 };

        til::inclusive_rect _viewport{};
        Microsoft::Console::Render::CursorOptions _cursor{};
        std::wstring _title;
    };
}
