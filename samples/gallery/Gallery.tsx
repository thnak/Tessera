// SPDX-License-Identifier: MIT

// Gallery skeleton of the Instrument library (issue #239, ADR-047 item 10): every Button variant in
// both sizes and disabled, on one page. The host (main.cpp) switches theme, density, direction and
// text scale around this page; the page itself holds no appearance logic.

import { useState } from "tessera";
import { Button } from "./Button";

export function Gallery() {
  const [presses, setPresses] = useState<int>(0);
  return (
    <View className="g-page">
      <Text className="g-title">Button</Text>
      <View className="g-row">
        <Button label="Primary" variant="primary" onPress={() => setPresses(presses + 1)}>
          <Text>Primary</Text>
        </Button>
        <Button label="Primary small" variant="primary" size="sm" onPress={() => setPresses(presses + 1)}>
          <Text>Small</Text>
        </Button>
        <Button label="Primary disabled" variant="primary" disabled={true} onPress={() => setPresses(presses + 1)}>
          <Text>Disabled</Text>
        </Button>
      </View>
      <View className="g-row">
        <Button label="Secondary" variant="secondary" onPress={() => setPresses(presses + 1)}>
          <Text>Secondary</Text>
        </Button>
        <Button label="Secondary small" variant="secondary" size="sm" onPress={() => setPresses(presses + 1)}>
          <Text>Small</Text>
        </Button>
        <Button label="Secondary disabled" variant="secondary" disabled={true} onPress={() => setPresses(presses + 1)}>
          <Text>Disabled</Text>
        </Button>
      </View>
      <View className="g-row">
        <Button label="Ghost" variant="ghost" onPress={() => setPresses(presses + 1)}>
          <Text>Ghost</Text>
        </Button>
        <Button label="Ghost small" variant="ghost" size="sm" onPress={() => setPresses(presses + 1)}>
          <Text>Small</Text>
        </Button>
        <Button label="Ghost disabled" variant="ghost" disabled={true} onPress={() => setPresses(presses + 1)}>
          <Text>Disabled</Text>
        </Button>
      </View>
      <View className="g-row">
        <Button label="Danger" variant="danger" onPress={() => setPresses(presses + 1)}>
          <Text>Danger</Text>
        </Button>
        <Button label="Danger small" variant="danger" size="sm" onPress={() => setPresses(presses + 1)}>
          <Text>Small</Text>
        </Button>
        <Button label="Danger disabled" variant="danger" disabled={true} onPress={() => setPresses(presses + 1)}>
          <Text>Disabled</Text>
        </Button>
      </View>
      <Text className="g-note">Presses: {presses}</Text>
    </View>
  );
}
