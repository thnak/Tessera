// SPDX-License-Identifier: MIT

// A deliberately busy settings form, used to compare what the engine paints with what a browser
// paints for the same markup and the same stylesheet (`form.module.css`). It is layout and paint stress,
// not an API sample: a card with a shadow, a header with a badge, a two-column grid, text inputs
// (one in an error state), a faux select, a segmented control, switches and a checkbox drawn from
// plain views, a truncated line, a divider and a footer of buttons in every variant and state.
// The browser twin is `tools/parity/form.html`; keep the two structures identical.

import { useState } from "tessera";
import { Button } from "./Button";
import styles from "./form.module.css";

export function FormPage() {
  const [submits, setSubmits] = useState<int>(0);
  const [plan, setPlan] = useState<int>(0);
  return (
    <View className={styles.page}>
      <View className={styles.card}>
        <View className={styles.head}>
          <View className={styles.logo}>
            <Text className={styles.logoMark}>T</Text>
          </View>
          <View className={styles.headText}>
            <Text className={styles.title}>Create workspace</Text>
            <Text className={styles.sub}>Invite your team and pick a plan. You can change this later.</Text>
          </View>
          <View className={styles.badge}>
            <Text className={styles.badgeText}>Pro trial</Text>
          </View>
        </View>

        <View className={styles.grid}>
          <View className={styles.field}>
            <Text className={styles.label}>First name</Text>
            <TextInput className={styles.input} aria-label="First name" defaultValue="Ada" />
          </View>
          <View className={styles.field}>
            <Text className={styles.label}>Last name</Text>
            <TextInput className={styles.input} aria-label="Last name" defaultValue="Lovelace" />
          </View>
          <View className={`${styles.field} ${styles.span}`}>
            <Text className={styles.label}>Work email</Text>
            <TextInput className={`${styles.input} ${styles.inputError}`} aria-label="Work email" defaultValue="ada@analytical" />
            <Text className={styles.error}>Enter a full address, for example ada@analytical.engine</Text>
          </View>
          <View className={`${styles.field} ${styles.span}`}>
            <Text className={styles.label}>Region</Text>
            <View className={styles.select}>
              <Text className={styles.selectValue}>Europe / Berlin (UTC+01:00)</Text>
              <Text className={styles.selectCaret}>v</Text>
            </View>
            <Text className={`${styles.help} ${styles.ellipsis}`}>Data stays in this region. Backups are kept in the same country unless you opt in to a second site later.</Text>
          </View>
        </View>

        <View className={styles.segment}>
          <View className={`${styles.seg} ${plan === 0 ? styles.segOn : ""}`} onClick={() => setPlan(0)}>
            <Text className={`${styles.segText} ${plan === 0 ? styles.segTextOn : ""}`}>Monthly</Text>
          </View>
          <View className={`${styles.seg} ${plan === 1 ? styles.segOn : ""}`} onClick={() => setPlan(1)}>
            <Text className={`${styles.segText} ${plan === 1 ? styles.segTextOn : ""}`}>Yearly</Text>
          </View>
          <View className={`${styles.seg} ${plan === 2 ? styles.segOn : ""}`} onClick={() => setPlan(2)}>
            <Text className={`${styles.segText} ${plan === 2 ? styles.segTextOn : ""}`}>Invoice</Text>
          </View>
        </View>

        <View className={styles.rows}>
          <View className={styles.row}>
            <View className={styles.rowText}>
              <Text className={styles.rowTitle}>Weekly digest</Text>
              <Text className={styles.help}>A summary every Monday morning.</Text>
            </View>
            <View className={`${styles.toggle} ${styles.toggleOn}`}><View className={`${styles.knob} ${styles.knobOn}`} /></View>
          </View>
          <View className={styles.row}>
            <View className={styles.rowText}>
              <Text className={styles.rowTitle}>Two-step sign in</Text>
              <Text className={styles.help}>Recommended for every admin.</Text>
            </View>
            <View className={styles.toggle}><View className={styles.knob} /></View>
          </View>
          <View className={styles.checkRow}>
            <View className={`${styles.check} ${styles.checkOn}`}><Text className={styles.checkMark}>x</Text></View>
            <Text className={styles.checkLabel}>I agree to the terms and the data processing addendum</Text>
          </View>
        </View>

        <View className={styles.rule} />

        <View className={styles.foot}>
          <Button label="Delete draft" variant="ghost" disabled={true} onPress={() => setSubmits(submits + 1)}>
            <Text>Delete draft</Text>
          </Button>
          <View className={styles.spacer} />
          <Button label="Cancel" variant="secondary" onPress={() => setSubmits(submits + 1)}>
            <Text>Cancel</Text>
          </Button>
          <Button label="Create workspace" variant="primary" onPress={() => setSubmits(submits + 1)}>
            <Text>Create workspace</Text>
          </Button>
        </View>
      </View>
    </View>
  );
}
