# Reproducing the spike

> [<- Layering, scope and the Go decision](04-layering-scope-and-decision.md) | [Texture cache and upload path ->](06-texture-cache.md)

## 9. Reproducing

```powershell
cmake -S . -B build/tvg -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=clang++ -DTESSERA_BUILD_VULKAN=OFF `
  -DTESSERA_ENABLE_THORVG=ON
cmake --build build/tvg --target thorvg_spike
.\build\tvg\tools\poc\thorvg_spike\thorvg_spike.exe --out icons.png
.\build\tvg\tools\poc\thorvg_spike\thorvg_spike.exe --compare tests\golden\thorvg\icons.png
ctest --test-dir build/tvg -R thorvg_spike_golden --output-on-failure
```

`--backend null` exercises the seam without painting (useful to compare binary sizes and to check
plumbing). `--raw path` writes the raw RGBA8 buffer, and `--tolerance`/`--max-mismatch` control the
golden comparison for future platform portability checks.
