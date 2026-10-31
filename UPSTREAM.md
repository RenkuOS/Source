<img src="Planning/DocsForRelease/assets/logo.png" alt="RenkuOS" width="260">

# Upstream import ledger

Required by D1. Every commit taken from Haiku is recorded here, with why. Code moves one way,
per commit, on merit — Haiku is not upstream in the GNU sense, and we do not merge wholesale.

Imports are made with `git cherry-pick -x`, which writes the source commit into the message,
so provenance is also permanent and greppable in the history itself. The original author is
preserved; only the committer changes.

**On sign-off.** An imported commit carries no `Signed-off-by` of ours, and `lint.yml` requires
one. Add it with `git commit --amend -s`. This is not rubber-stamping someone else's work: DCO
clauses (b) and (c) cover exactly this case — certifying that the contribution was received
under an appropriate licence and that you have the right to submit it.

**Imports go in verbatim.** If an imported commit contains a mistake, fix it in a separate,
clearly-labelled commit rather than quietly editing the import. That keeps the diff against
upstream honest and the fix attributable.

| Date | Upstream commit | Subject | Why |
|---|---|---|---|
| 2026-09-16 | `fcdee3a787` | deskbar: don't loop forever if replicant is larger than the row itself | Fixes a Deskbar hang (#19394): a replicant wider than the tray made the placement loop spin at 100% CPU, so Deskbar never appeared |
| 2026-09-19 | `e9ade57f61` | ntfs: fix vnode leak on mkdir | Creating one directory on an NTFS volume leaked a vnode reference, so the volume could never be unmounted again ("inode is still referenced") |
| 2026-09-21 | `056cd81185` | deskbar: fix replicant loop guard regression on first empty row | Follow-up to `fcdee3a787`: with the clock shown, a replicant too wide for the first row but narrow enough for a full row was forced onto the first row and overlapped the clock, instead of moving to the second row |
| 2026-10-06 | `d1111656df` | usb_manager: check descriptors length integrity | A device reporting a malformed configuration could make the USB stack overread its descriptor buffer: a total length shorter than the configuration header, descriptors of length 1, or interface and endpoint descriptors shorter than their structures. Also refuses a device with no configurations |
| 2026-10-07 | `7f32a78c62` | radeon_hd: fix crtc control register state tracing on evergreen+ | The mode-set register dump read the pre-Evergreen D1/D2 addresses on every card, so it showed nothing useful on DCE4 and later. Debug output only (`TRACE_MODE`). Taken ahead of the RadeonHD port, which edits the same files |
| 2026-10-20 | `1d73eca40b` | freebsd_network: enable on arm64 | Prerequisite of `ae25f101ad`, which rewrites the same lines |
| 2026-10-20 | `ae25f101ad` | freebsd_network: use generic bus.h and cpufunc.h for all non-x86 architectures | The FreeBSD compatibility layer did not build for 32-bit ARM, so no FreeBSD network driver could be built there (the USB Ethernet adapters used with the Toshiba AC100) |
| 2026-10-20 | `5f79cf3d8e` | freebsd_network: add missing bpf stub | Companion of `ae25f101ad`: a stub the drivers need once the layer builds on the other architectures |
| 2026-10-30 | `ad3afc3a2f` | tcp: _Spawn() resets local address | Fixes a kernel panic on shutdown (#167, "bound endpoint not in hash!"). A spawned connection that failed to bind kept the listening socket's local address, so destroying it tried to remove it from a hash it was never added to |

## Declined

Nothing yet. When an upstream commit is looked at and rejected, record it here with the reason
— that record is what makes "we did not just run away" a checkable claim rather than a
sentiment.
