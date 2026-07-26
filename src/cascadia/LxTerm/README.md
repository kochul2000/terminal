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

P0 spike. `lxterm_create` / `lxterm_write` / `lxterm_take_frame` / `lxterm_destroy`
work; frames are read straight off the `TextBuffer` and every viewport row is
reported every time. Dirty tracking, input, resize and the event channel are
tracked as separate issues.

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

The stock Windows Terminal build targets a machine with the full IDE and a
specific SDK. LxTerm only needs `TerminalCore` as a desktop static library, so
`build.ps1` overrides:

| Override | Reason |
| --- | --- |
| `OpenConsoleUniversalApp=false` | `TerminalCore` builds as a Windows Store lib by default, which requires the UWP C++ workload |
| `SpectreMitigation=false` | Spectre-mitigated runtime libs are a separate VS component |
| `WindowsTargetPlatformVersion` / `TargetPlatformVersion` | the repo pins 10.0.22621, which may not be installed |
| `_CL_=/WX-` (env) | `TreatWarningAsError` is hardcoded in `src\common.build.pre.props` and cannot be overridden with `/p:`; newer MSVC toolsets warn on code the pinned toolset accepted |

## ABI notes

- `LxFrame` and everything it points at are owned by the `LxTerm` and are valid
  only until the next `lxterm_take_frame` on that instance.
- Rows carry **runs**, not cells. `RowAttributes` is already run-length encoded
  upstream, so runs fall out naturally and cut the payload by one to two orders
  of magnitude — which matters because the same frames go over the wire to
  laymux's remote sessions.
- Colors are resolved to `0x00RRGGBB` on this side of the ABI, via
  `IRenderData::GetAttributeColors`.
- `lxterm_write` holds incomplete UTF-8 sequences internally, so callers can pass
  arbitrary PTY chunks.
