# Cast NV12 overlay shaders

The checked-in headers are release bytecode generated from
`nv12_overlay.hlsl`. `show_host` does not compile shaders or load shader files
at runtime.

Regenerate them from a Visual Studio developer shell with the Windows SDK
`fxc.exe`:

```powershell
fxc /nologo /T cs_5_0 /E compose_y /O3 /Qstrip_debug /Qstrip_reflect /Fh nv12_overlay_y_bytecode.h /Vn kNv12OverlayYShader nv12_overlay.hlsl
fxc /nologo /T cs_5_0 /E compose_uv /O3 /Qstrip_debug /Qstrip_reflect /Fh nv12_overlay_uv_bytecode.h /Vn kNv12OverlayUvShader nv12_overlay.hlsl
```
