// Preview-parity sample (issue #7): a native handler whose TSX body is its preview body, plus an
// inline handler. The developer's C++ in samples/parity-demo/Counter.cpp does the same as the
// preview body, and the parity test checks that they agree step by step. It ships its own sheet
// (counter.css, issue #607) so the Studio shows real author styling rather than the fallback sheet.

import "./counter.css";

export function Counter() {
  const [count, setCount] = useState<int>(0);
  const [label, setLabel] = useState<string>("idle");

  // Note: bind::add
  const add = (): void => {
    setCount(count + 2);
    setLabel("added");
  };

  return (
    <View className="counter">
      <Text className="readout">Count: {count} {label}</Text>
      <Button className="primary" onClick={add}>Add</Button>
      <Button className="ghost" onClick={() => setCount(0)}>Reset</Button>
    </View>
  );
}
