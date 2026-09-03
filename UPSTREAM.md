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
| 2026-09-03 | [`2ffb9aef51`](https://github.com/haiku/haiku/commit/2ffb9aef511dde272ab57d545a5d8631705a5ec8) | developer documentation: document device file hooks | Documentation improvement, no code impact. Also the first exercise of the D1 import path. |

## Declined

Nothing yet. When an upstream commit is looked at and rejected, record it here with the reason
— that record is what makes "we did not just run away" a checkable claim rather than a
sentiment.
