export function applied(overrides = {}) {
  return { status: 'applied', verified: true, verification: 'owner_and_commit',
    desired_revision: 'a'.repeat(64), applied_revision: 'a'.repeat(64),
    mode: 2, scope: 0, block_quic: false, ...overrides };
}
