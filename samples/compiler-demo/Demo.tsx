// A small Tessera screen used by the codegen CLI sample and the Big Step C acceptance:
// useState, useEffect (deps + cleanup), useRef, useAnimatedValue, a declared handler bound through
// a protected region, inline setter handlers, and a conditional child (the keyed-list demo needs a
// list-typed state cell, which the front end cannot author yet). It ships its own sheet (demo.css,
// issue #607): the Studio's first sample shows real author styling, not the fallback sheet.

import "./demo.css";

export function Demo() {
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
      <Text className="title">Count: {count}</Text>
      <Text className="muted">Name: {name}</Text>
      <Button className="primary" onClick={() => setCount(count + 1)}>Add</Button>
      <TextInput className="field" value={name} onChange={(e) => setName(e.value)} />
      {count > 0 && <Text className="badge">Positive</Text>}
    </View>
  );
}
