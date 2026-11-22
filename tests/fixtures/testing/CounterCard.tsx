// A counter screen written the way an app author writes one: `useState`, a click handler that reads
// and writes it, and a conditional child. `automationId` pins the targets a test and an agent address,
// which is what `docs/specs/automation-ids-v1.md` section 2 recommends for a long-lived target (a
// computed `uiId` moves when the tree does; an explicit one does not).
//
// Used by `tests/unit/testing_acceptance_tests.cpp` through `tessera_testing`, and by
// `docs/guides/testing-your-app.md` as its worked example.

export function CounterCard() {
  const [count, setCount] = useState<int>(0);
  const [label, setLabel] = useState<string>("idle");

  return (
    <View automationId="1001">
      <Text automationId="1002">Count: {count}</Text>
      <Text automationId="1003">{label}</Text>
      <Button automationId="1004" onClick={() => { setCount(count + 1); setLabel("added"); }}>
        Add
      </Button>
      <TextInput automationId="1005" onChange={(e) => setLabel(e.value)} />
      {count > 0 && <Text automationId="1006">Positive</Text>}
    </View>
  );
}
