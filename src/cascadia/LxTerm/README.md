# LxTerm — headless Terminal.Core for laymux

`laymux_wt.dll` hosts a `Microsoft::Terminal::Core::Terminal` with no window, no
font and no renderer backend. PTY bytes go in; run-encoded screen snapshots come
out. laymux consumes it over a plain C ABI (`lxterm.h`) from Rust.

```
app -> ConPTY -> [laymux_wt.dll] Terminal (VT parse + text buffer) -> LxFrame -> Rust
```

This is the only directory added to the upstream tree. Nothing outside it is
modified, so tracking new upstream release tags stays a rebase with no conflicts.

## Status

`lxterm_create` / `lxterm_write` / `lxterm_take_frame` / `lxterm_destroy` work,
and frames are deltas: `LxEngine` implements `IRenderEngine`, so the Renderer
hands it the same dirty regions, resolved attributes and glyph clusters an
on-screen backend gets. Writing one glyph to an 80x25 terminal produces a frame
of one row, one run, one byte.

Input, resize, the event channel and alt-buffer state are tracked as separate
issues.

## Building

```powershell
$env:VCPKG_ROOT = 'D:\vcpkg'
powershell -File src\cascadia\LxTerm\build.ps1
```

Output: `bin\x64\Release\LxTerm\laymux_wt.dll`.

## Testing

```powershell
powershell -File src\cascadia\LxTerm\test\run.ps1
```

Compiles `test\smoke.c` against the built DLL and asserts that `\x1b[31mhi`
produces a single run of `hi` with a red foreground.

### Requirements

- Visual Studio 2022 (Build Tools are enough) with the C++ workload
- A Windows 10/11 SDK — `build.ps1` picks the newest installed one
- vcpkg, **fully cloned** (a `--depth 1` clone cannot check out port trees) and
  bootstrapped, with `VCPKG_ROOT` pointing at it

`build.ps1` restores `dep\nuget\packages.config` on first run.

### Why the build needs overrides

The stock Windows Terminal build assumes a machine provisioned for the full app.
LxTerm only needs `TerminalCore` as a desktop static library, so `build.ps1`
probes for each missing component and overrides only what it has to — a fully
provisioned machine builds exactly like upstream. It reports which overrides it
applied.

| Override | Applied when | Component that removes it |
| --- | --- | --- |
| `WindowsTargetPlatformVersion` / `TargetPlatformVersion` | SDK 10.0.22621 is absent (the repo pins it) | Windows SDK 10.0.22621 |
| `SpectreMitigation=false` | no `lib\spectre\<platform>` under any MSVC toolset | Spectre-mitigated runtime libraries |
| `OpenConsoleUniversalApp=false` | no `Application Type\Windows Store\10.0` under the VC targets | UWP C++ workload (v143) |

One override is unconditional: `_CL_=/wd4706`. `TreatWarningAsError` is hardcoded
in `src\common.build.pre.props` and cannot be overridden with `/p:`, and cl.exe's
`_CL_` environment variable is the only remaining lever. Upstream CI runs
`Set-LatestVCToolsVersion.ps1`, so it builds against whatever toolset the hosted
image ships rather than a pinned one; newer toolsets warn on code older ones
accepted. As of MSVC 14.44 the only such warning in this dependency graph is
C4706 in `TerminalSelection.cpp`. Suppressing exactly that — rather than `/WX-` —
keeps a rebase onto a newer upstream tag failing loudly on anything new.

## ABI notes

- `LxFrame` and everything it points at are owned by the `LxTerm` and are valid
  only until the next `lxterm_take_frame` on that instance.
- Rows carry **runs**, not cells. `RowAttributes` is already run-length encoded
  upstream, so runs fall out naturally and cut the payload by one to two orders
  of magnitude — which matters because the same frames go over the wire to
  laymux's remote sessions.
- A frame is a **delta**. Only rows the Renderer marked dirty appear, runs within
  a row start at `LxRun::col` rather than covering it contiguously, and runs may
  overlap when several dirty regions touch one row — later runs win. When
  `full_repaint` is set the rows are the whole viewport, so a consumer holding a
  mirror should drop it first. Scrolling sets `full_repaint`.
- Rows are numbered in **absolute buffer coordinates**, not viewport rows, so a
  consumer can follow scrolling from `view_top` without reindexing what it holds.
- The cursor is a coordinate on the frame, not ink in a cell. `LxEngine` therefore
  ignores cursor invalidation for content purposes and only treats an actual move
  as a reason to produce a frame; an idle `lxterm_take_frame` returns
  `rows_len == 0`.
- Colors are resolved to `0x00RRGGBB` on this side of the ABI, via
  `IRenderData::GetAttributeColors`.
- `lxterm_write` holds incomplete UTF-8 sequences internally, so callers can pass
  arbitrary PTY chunks.
