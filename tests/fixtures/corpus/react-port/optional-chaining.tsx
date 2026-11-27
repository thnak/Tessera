// SPDX-License-Identifier: MIT

// Corpus (spike #660): optional chaining and nullish fallback

interface Profile {
  address?: { city?: string };
}

export function City({ profile }: { profile: Profile }) {
  return <span>{profile.address?.city ?? "Unknown"}</span>;
}
