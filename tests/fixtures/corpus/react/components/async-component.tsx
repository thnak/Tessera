// SPDX-License-Identifier: MIT

// Corpus fixture: an `async` component that awaits a fetch. ADR-066 item 8 keeps this rejected:
// data comes through services and observables (ADR-050), and async handlers are native.

export async function UserCard({ id }: { id: string }) {
  const response = await fetch(`/api/users/${id}`);
  const user = await response.json();
  return (
    <div className="user-card">
      <span className="user-name">{user.name}</span>
    </div>
  );
}
