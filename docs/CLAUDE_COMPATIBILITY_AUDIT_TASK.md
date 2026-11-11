# Claude task — compatibility / safety audit

Base: `experiment/fex-metal-compat-performance` (PR #289). Work only on this branch; do not merge.

1. Audit PS4 game loading, PKG read-only inventory, Orbis/SCE/PRX handling, memory mapping and guest-to-host translations. Distinguish PKG recognition from game execution.
2. Identify concrete blockers with source paths, reproduction steps and severity. Prioritize crashes, malformed input, leaks and unsupported syscalls over speculative game compatibility.
3. Add focused regression tests or isolated fixes with explicit failure cases; avoid changing FEXCore/Metal files owned by the Codex branch unless coordinated.
4. Run CI and open a draft PR into `experiment/fex-metal-compat-performance` with evidence and remaining risks.
5. Preserve `MaxPS4-Beta`; no release or merge without device smoke tests for Sonic Mania and Mortal Kombat.

Do not claim additional games playable without authorized on-device validation.
