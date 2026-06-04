# Direct-Object Smoke Example

This example uses the native object backend.

It stays inside the currently supported direct-object subset:

- scalar integer parameters
- internal direct calls
- conditional control flow
- integer returns
- no locals, globals, inline asm, or debug info

From the repository root:

```powershell
examples\direct_object_smoke\build.bat
```

That script will:

1. Emit `direct_object_smoke.obj` with `--emit-obj`
2. Dump symbols and relocations if `objdump` is available
3. Build `direct_object_smoke.exe` with `--build --emit-obj`
4. Run the executable and check that it exits with `17`
