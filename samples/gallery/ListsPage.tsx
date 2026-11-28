// SPDX-License-Identifier: MIT

// The list look page (issue #1006): the user-agent list markers of `ul`/`ol`/`li` as a browser draws
// them. Nested bullets (disc, circle, square by depth), decimal numbering, `start`, `reversed`,
// an item `value`, and the alphabetic and roman types through the author sheet. The host paints it
// with lavapipe only; the page holds no appearance logic (docs/design/list-style.md).

export function ListsPage() {
  return (
    <View className="g-page">
      <Text className="g-title">Lists</Text>
      <View className="l-columns">
        <ul className="l-list">
          <li><span>Alpha</span></li>
          <li>
            <span>Beta</span>
            <ul>
              <li><span>Nested one</span></li>
              <li>
                <span>Nested two</span>
                <ul><li><span>Third level</span></li></ul>
              </li>
            </ul>
          </li>
          <li><span>Gamma</span></li>
        </ul>
        <ol className="l-list">
          <li><span>First</span></li>
          <li><span>Second</span></li>
          <li><span>Third</span></li>
        </ol>
        <ol className="l-list" start={5}>
          <li><span>Starts at five</span></li>
          <li><span>Six</span></li>
          <li value={20}><span>Value twenty</span></li>
          <li><span>Twenty-one</span></li>
        </ol>
        <ol className="l-list" reversed>
          <li><span>Counts down</span></li>
          <li><span>Two</span></li>
          <li><span>One</span></li>
        </ol>
        <ol className="l-list l-roman">
          <li><span>Roman</span></li>
          <li><span>Numerals</span></li>
          <li><span>Third</span></li>
          <li><span>Fourth</span></li>
        </ol>
        <ol className="l-list l-alpha">
          <li><span>Alpha</span></li>
          <li><span>Letters</span></li>
          <li><span>Third</span></li>
        </ol>
      </View>
      <Text className="l-sizes">
        Mixed sizes: H
        <sub>2</sub>
        O, x
        <sup>2</sup> and <small>small print</small> on one line
      </Text>
    </View>
  );
}
