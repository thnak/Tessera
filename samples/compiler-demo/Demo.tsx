// A small Tessera screen used by the codegen CLI sample and the Big Step C acceptance:
// useState, useEffect (deps + cleanup), useRef, useAnimatedValue, a declared handler bound through
// a protected region, and inline setter handlers.

export function Demo(): Node {
  const [count, setCount] = useState<int>(0);
  const [name, setName] = useState<string>("");
  const box = useRef<View>(null);
  const fade = useAnimatedValue(1.0);


  // Note: bind::sync
  useEffect(() => {
    log(count);
    return () => {
      stop();
    };
  }, [count]);

  return (
    <View ref={box} className="card" style={{ opacity: fade }}>
      <Text>Count: {count}</Text>
      <Text>Name: {name}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
      <TextInput value={name} onChange={(e) => setName(e.value)} />
    </View>
  );
}
