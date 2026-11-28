// SPDX-License-Identifier: MIT

// The display components gallery page (issue #240, ADR-047 item 10): Badge, Divider, Card, Avatar,
// Skeleton and Spinner in every tone, size and shape. Nothing here keeps state: these components
// are presentational. The host switches theme, density and direction; the page holds no appearance
// logic.

import { Badge } from "./Badge";
import { Divider } from "./Divider";
import { Card } from "./Card";
import { Avatar } from "./Avatar";
import { Skeleton } from "./Skeleton";
import { Spinner } from "./Spinner";

export function DisplayPage() {
  return (
    <View className="g-page">
      <Text className="g-title">Badge, Divider, Card, Avatar, Skeleton, Spinner</Text>
      <View className="g-row">
        <Badge label="Neutral" />
        <Badge label="Accent" tone="accent" />
        <Badge label="Passed" tone="success" />
        <Badge label="Pending" tone="warning" />
        <Badge label="Failed" tone="danger" />
      </View>
      <Divider />
      <View className="g-row">
        <Avatar label="Ada Lovelace" initials="AL" size="sm" />
        <Avatar label="Grace Hopper" initials="GH" />
        <Avatar label="Alan Turing" initials="AT" size="lg" />
        <Divider orientation="vertical" />
        <Spinner label="Syncing" size="sm" />
        <Spinner label="Loading" />
      </View>
      <View className="g-row">
        <Card title="Account">
          <Text>Signed in as Ada</Text>
          <Badge label="Admin" tone="accent" />
        </Card>
        <Card title="Plan" variant="flat">
          <Text>Team, 5 seats</Text>
        </Card>
      </View>
      <Card variant="flat">
        <View className="g-row">
          <Skeleton shape="circle" />
          <View className="g-skeleton-lines">
            <Skeleton />
            <Skeleton />
          </View>
        </View>
        <Skeleton shape="rect" />
      </Card>
    </View>
  );
}
