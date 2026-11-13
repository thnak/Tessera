// Preview-parity sample (issue #7): a native handler whose TSX body is its preview body, plus an
// inline handler. The developer's C++ in samples/parity-demo/Counter.cpp does the same as the
// preview body, and the parity test checks that they agree step by step.

export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  const [label, setLabel] = useState<string>("idle");

  // Note: bind::add
  const add = (): void => {
    setCount(count + 2);
    setLabel("added");
  };

  return (
    <View>
      <Text>Count: {count} {label}</Text>
      <Button onClick={add}>Add</Button>
      <Button onClick={() => setCount(0)}>Reset</Button>
    </View>
  );
}
