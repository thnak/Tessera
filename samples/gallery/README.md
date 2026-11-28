# gallery

The component gallery and the native parity page (`--page form` paints `FormPage.tsx` with the engine).
It has several screens (`Gallery`, `DashPage`, `FormPage`, `SafeAreaPage`, `SwatchPage`: every colour token of a theme, painted with the bundled Plex font pack; `ListsPage`: the `ul`/`ol` markers and inline `sub`/`sup`/`small`; `TablesPage`: the `table` tags with spans, collapsed borders and fixed layout), so it deliberately has no
`tessera.json`: a project names one entry, and without one the Studio offers the screens as a choice
and copies `packages/tessera-ui/Button.tsx` next to them the way `CMakeLists.txt` does for the native build.
