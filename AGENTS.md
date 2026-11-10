# RenkuOS Agent Instructions

## Project

RenkuOS is an independent fork of Haiku. It is a systems-level operating-system project with a strong emphasis on correctness, reproducibility, hardware support, maintainability, and automated verification.

The source repository is:

- `RenkuOS/Source`
- Primary branch: `main`

Before making significant changes, understand the surrounding code and existing design. **Do not assume that upstream Haiku behavior is automatically the desired RenkuOS behavior.**

For project decisions and development policy, see the `RenkuOS/PlanAndGovernance` repository:

- `DECISIONS.md` — authoritative decision record
- `CONTRIBUTING.md` — contribution workflow
- `REVIEW.md` — code-review and merge rules
- `engineering-charter.md` — engineering principles and rationale

If these documents conflict with assumptions made by an agent, follow the current repository policy.

## Core Principles

1. **Correctness before features.**
2. **Automated verification is essential.** CI is a first-class part of the project, not an afterthought.
3. **Prefer small, reviewable changes.**
4. **Preserve upstream history and attribution.**
5. **Review the code, not how it was produced.**
6. **The human contributor remains responsible for every submitted line.**
7. **Do not introduce unnecessary formatting churn or unrelated changes.**

Machine-assisted development is explicitly allowed. AI-generated code is not exempt from testing, review, licensing, or accountability requirements.

## Coding Style

RenkuOS inherits **Haiku's existing coding style**. Match the surrounding code rather than introducing a new style.

- Follow existing naming, formatting, architecture, and API conventions.
- Use `clang-format` on changed lines where applicable.
- Do not reformat unrelated code.
- Avoid large mechanical formatting changes.
- Prefer existing Haiku/RenkuOS idioms over inventing new abstractions.
- Keep changes focused and consistent with the subsystem being modified.

If a future `CODING_STYLE.md` or `.clang-format` exists, follow it as the authoritative style specification.

## Changes and Scope

Prefer **one logical change per pull request**.

Avoid combining unrelated fixes, refactors, formatting changes, and feature work.

As a general rule:

- ~400 changed lines is routine.
- 1,000 changed lines requires an explicit splitting plan or the appropriate approval.

There is no limit on how many subsystems one change may touch. If a change touches several, say in the PR why they belong together. A reviewer may still ask for a split.

## Testing and Verification

Never claim that a change works merely because it compiles.

Before proposing a change, determine what could be affected and test accordingly.

Examples:

- Documentation/translation → verify CI.
- Application/preflet → run the relevant smoke tests.
- Driver → test on actual hardware where possible and record the device model and PCI/USB ID.
- Kernel, filesystem, VM, scheduler, bootloader, package management, or cryptographic code → use substantially stronger testing, including relevant stress or hardware testing where practical.
- Bug fixes should include a test that would have caught the original bug whenever feasible.

Every PR should contain a concise **"How this was tested"** section.

Treat CI failures as engineering problems to investigate, not obstacles to work around.

Do not weaken, bypass, or disable tests merely to make a PR pass.

## Licensing and Provenance

RenkuOS policy is **MIT for new RenkuOS code**, while inherited Haiku code retains its existing license.

Before importing or porting code from another project:

1. Determine its source and license.
2. Confirm that the source is permitted under current RenkuOS policy.
3. Add the required provenance information.
4. Do not copy code of uncertain origin into the tree.

New files require an SPDX license identifier.

Code imported from another project must use:

```text
Ported-from: <project/version/source>
Ported-from-license: <license>
```

These trailers must appear together.

Do not assume that code found in another open-source project is automatically suitable for RenkuOS.

## Commits

Every commit must contain a `Signed-off-by:` trailer.

Use:

```text
git commit -s
```

Preferred commit format:

```text
subsystem: short imperative summary

Explain what changed and why. Wrap the body at approximately
72 columns.

Signed-off-by: Your Name <you@example.org>
```

Keep the subject below 72 characters and preferably use the `subsystem: ` prefix.

When machine assistance substantially contributed to the change, disclose it with:

```text
Assisted-by: <tool-or-model>
```

"Substantial" means roughly more than a small amount of autocomplete, such as a significant portion of the implementation or the design itself.

## Pull Requests

All changes to `main` go through a pull request. Do not push directly to `main`.

Before opening a PR:

- Build the affected targets.
- Run the relevant tests/smoke tests.
- Run `clang-format` on changed lines.
- Ensure every commit has `Signed-off-by:`.
- Check SPDX and provenance requirements.
- Keep the change within the applicable size/scope limits.
- Write the PR summary yourself.
- Include **How this was tested**.
- Identify relevant hardware when submitting driver changes.

The current merge gate requires:

- One approval from an account with write access.
- The latest commit must itself be approved.
- Required CI must be green.
- Review threads must be resolved.
- The PR author cannot approve their own change.

An approval is dismissed when new commits are pushed, so do not assume an earlier approval remains valid after modifying the PR.

Do not merge around failed checks or unresolved review.

## Code Review

Reviewers should identify **concrete technical problems**, not speculate about how code was produced.

Good review:

> This early return leaves `fLock` held.

Bad review:

> This looks AI-generated.

Do not reject or request additional review merely because a change was machine-assisted.

Review for:

- correctness
- regressions
- API/ABI compatibility
- concurrency and lifetime issues
- error handling
- resource ownership
- hardware/device removal
- filesystem/data integrity
- security
- performance where relevant
- test coverage
- licensing/provenance
- unnecessary complexity

A review objection should identify a specific problem or required change.

The contributor is responsible for understanding every line they submit. "The model wrote it" is never an acceptable explanation for code the contributor cannot explain.

## Upstream Haiku

RenkuOS is not simply a downstream branch of Haiku.

Where upstream code is imported, preserve history and attribution. The planned upstream workflow uses:

```text
git cherry-pick -x
```

Do not squash-import upstream changes when doing so would lose useful provenance.

Do not blindly synchronize RenkuOS with Haiku. Determine whether an upstream change applies to the RenkuOS architecture and current divergence.

When importing upstream code, record the decision and provenance according to the project's upstream-import process.

## Agent Behavior

When working on RenkuOS:

- Read relevant existing code before proposing architecture.
- Search for existing implementations before creating new ones.
- Prefer the smallest correct change.
- Do not invent APIs, build commands, test results, hardware support, or project policy.
- Clearly distinguish facts verified in the tree from assumptions.
- If a required build/test command is unknown, say so rather than pretending it was run.
- Do not silently change project policy.
- Do not make unrelated cleanup changes while implementing a feature or fix.
- Preserve compatibility and existing behavior unless the change explicitly requires otherwise.
- Ask for clarification when an important architectural or policy decision is genuinely unresolved.

For ambiguous policy questions, consult `PlanAndGovernance/DECISIONS.md` first.

## Current Project State

Some governance documents describe the intended future state rather than mechanisms currently enforced by GitHub.

As of September 2026:

- `main` is protected.
- One write-access approval is required.
- Required CI includes policy checks, x86_64 build, and x86_64 boot.
- 32-bit x86 is a planned Tier 1 target but is currently suspended from the CI matrix.
- `MAINTAINERS` and `CODEOWNERS` are not yet established.
- Reviewer assignment is therefore not yet automatic.
- Some proposed review rules are policy expectations rather than GitHub-enforced gates.
- Merge-method restrictions described in the future review policy are not yet fully enforced.

**Always prefer the repository's current CI configuration and the "Current state" sections of the governance documents over assumptions about planned infrastructure.**
