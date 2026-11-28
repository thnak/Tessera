// SPDX-License-Identifier: MIT

// The table look page (issue #543): the `table` tags as a browser draws them. A data table with a
// caption, header, body and footer and collapsed borders; spans (`colSpan`, `rowSpan`, a row
// written without a `tbody`); the separate model with `border-spacing`; a fixed layout with `col`
// widths; and vertical alignment inside a tall row. The host paints it with lavapipe only; the page
// holds no layout logic (docs/design/table-layout.md).

export function TablesPage() {
  return (
    <View className="g-page">
      <Text className="g-title">Tables</Text>
      <View className="t-columns">
        <table className="t-data">
          <caption><span>Quarterly totals</span></caption>
          <thead>
            <tr>
              <th><span>Region</span></th>
              <th><span>Q1</span></th>
              <th><span>Q2</span></th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td><span>North</span></td>
              <td className="t-num"><span>120</span></td>
              <td className="t-num"><span>135</span></td>
            </tr>
            <tr>
              <td><span>South and east</span></td>
              <td className="t-num"><span>98</span></td>
              <td className="t-num"><span>1,204</span></td>
            </tr>
          </tbody>
          <tfoot>
            <tr>
              <th><span>Total</span></th>
              <td className="t-num"><span>218</span></td>
              <td className="t-num"><span>1,339</span></td>
            </tr>
          </tfoot>
        </table>
        <table className="t-data t-spans">
          <tr>
            <th colSpan={2}><span>Spans two columns</span></th>
            <th rowSpan={2}><span>Two rows</span></th>
          </tr>
          <tr>
            <td><span>a</span></td>
            <td><span>b</span></td>
          </tr>
          <tr><td colSpan={3}><span>Spans all three columns</span></td></tr>
        </table>
        <table className="t-spaced">
          <tr>
            <td><span>Separate</span></td>
            <td><span>borders</span></td>
          </tr>
          <tr>
            <td><span>with 6px</span></td>
            <td><span>spacing</span></td>
          </tr>
        </table>
        <table className="t-data t-fixed">
          <colgroup>
            <col className="t-narrow" />
            <col />
          </colgroup>
          <tr>
            <td><span>Fixed</span></td>
            <td>
              <span>layout, 300px wide: the first column is 80px, the second takes the rest</span>
            </td>
          </tr>
        </table>
        <table className="t-data">
          <tr>
            <td className="t-tall"><span>Middle</span></td>
            <td className="t-tall t-top"><span>Top</span></td>
            <td className="t-tall t-bottom"><span>Bottom</span></td>
          </tr>
        </table>
      </View>
    </View>
  );
}
