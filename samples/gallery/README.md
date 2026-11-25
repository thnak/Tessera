# gallery

The component gallery and the native parity page (`--page form` paints `FormPage.tsx` with the engine).
It has several screens (`Gallery`, `DashPage`, `FormPage`, `SafeAreaPage`), so it deliberately has no
`tessera.json`: a project names one entry, and without one the Studio offers the screens as a choice
and copies `packages/tessera-ui/Button.tsx` next to them the way `CMakeLists.txt` does for the native build.
