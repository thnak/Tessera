// A Tessera screen with two `bind::` markers, used by the `#line` map compile test (issue #78).
// The two markers sit on lines 5 and 10 so one failing build produces two diagnostics, which is what
// proves the generated-file numbering is restored correctly after the first mapped region: with a
// single marker an off-by-N in the restore directive would go unnoticed.

export function Two(): Node {
  const [a, setA] = useState<int>(0);
  const [b, setB] = useState<int>(0);

  // Note: bind::first
  useEffect(() => {
    log(a);
  }, [a]);

  // Note: bind::second
  useEffect(() => {
    log(b);
  }, [b]);

  return (
    <View>
      <Text>a {a}</Text>
      <Text>b {b}</Text>
    </View>
  );
}
