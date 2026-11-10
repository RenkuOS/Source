<!-- SPDX-License-Identifier: MIT -->
# Experimental Gen9 render node

This is a full import of the native Haiku `intel_render_9x` prototype, including
its tested engine-map rejection and ring/completion fixes. It extends
`intel_extreme`; it does not replace its display driver. This import is for draft
review. RenkuOS integration, size approval, license review and CI remain required.

## Scope

For Generation() == 9, publish `graphics/intel_render_9x_<bus><device><function>`
alongside the display device. Initialization is lazy, on render-node open.
Implement area-backed GEM objects, CPU mappings, private GGTT allocation,
per-context 48-bit PPGTT, logical ring contexts, RCS execlists, and seqno-backed
sync objects. The render interrupt hooks into the existing Gen8 display handler.

The ioctl ABI is private and experimental. `render_abi.h` defines the native test
interface. `drm_uapi.h` provides the DRM/i915 subset used by a Haiku-built Mesa
iris stack. Its ioctl direction bits follow Haiku, so it is not a Linux binary ABI.
No public `headers/os` ABI is changed.

All GPU submissions complete under the node lock before EXEC returns. This is a
correctness-first prototype, not an asynchronous request scheduler. Ring rollover
pads the unused tail with NOOPs; it does not reset HEAD. CREATE_EXT rejects flags
and extension chains it cannot implement. In particular, it must not accept and
ignore an engine map: render and compute slots on physical RCS need separate
hardware-context state. Rejecting the map makes stock iris use distinct legacy
contexts. The prior implementation aliased them and corrupted later draws.

## Build and hardware regression

In a configured native Haiku/RenkuOS source tree, build the existing target:

```
jam -q -j2 intel_extreme
```

On the hardware, from the source root:

```
g++ -o /tmp/intel-render-stress \
  src/add-ons/kernel/drivers/graphics/intel_extreme/tests/drmstress.cpp \
  -Iheaders/private/graphics
/tmp/intel-render-stress
```

The default device path is for PCI bus 0/device 2/function 0. The test first
requires rejection of an unsupported two-RCS engine map, then verifies 4096
changing-value GPU stores, GEM_WAIT and syncobj completion. It crosses the 16 KiB
ring boundary repeatedly. It returns nonzero on the first failed submission/data
check. These are real hardware tests; they cannot run meaningfully on an ordinary
QEMU VGA adapter.

The imported implementation was built and run on Haiku hrev60232, x86_64, on a
Xiaomi Mi Air 12.5 with Intel HD Graphics 515, PCI 8086:191e. The final Haiku driver
passed 4096 stores in 0.141 seconds after cold boot. A separately patched Mesa
25.3.6/libglvnd stack passed window/FBO readback and 300 full-image frames including
resize. GLTeapot produced a complete image; one matched 20-second comparison
counted 18857 iris frames and 14121 llvmpipe frames (942.85 vs 706.05 FPS).
These are Haiku prototype results, not RenkuOS build, boot or hardware validation.
This imported snapshot preserves RenkuOS's existing ValleyView enablement and
NORTH display-register relocation changes; those changes were not present in the
prototype's older source base.

## Userspace is separate

The kernel import alone does not turn on hardware GL for BGLView. The tested
userspace used a libdrm shim, Haiku EGL vendor selection, linear color resources,
current-frame fence waits, and CPU copies into BBitmaps. Its hardware present path
omits `flush_resource`, which prepares external sharing and can replace iris's BO.
Software display-target preparation occurs after queued GL vertices are drained.
Those Mesa/libglvnd changes and packages need a separate contribution. The earlier
CCS failure still requires the tested `noccs` workaround in that stack.

## Known limits and merge blockers

- Only 8086:191e on x86_64 has hardware evidence. Other Gen9 IDs published by the
  predicate and 32-bit configurations are untested. This is not a family-wide
  support claim.
- No GPU command parser or isolation guarantee is provided for untrusted clients.
  Context descriptors allow privileged submission. Device access must be reviewed
  before enabling this for untrusted users.
- No complete hang/reset recovery, suspend/resume, power management, engine-map
  scheduling, Vulkan or dma-buf implementation. A one-second submit timeout marks
  the context dead; failure teardown needs further audit before production use.
- GGTT reservation uses a watermark, not integration with a general allocator.
  Later GART/display allocations and modesetting need regression testing.
- Some accepted compatibility ioctls are no-ops. Audit every advertised capability
  before expanding userspace use; the engine-map bug demonstrates why this matters.
- Kernel source/build evidence is from Haiku. RenkuOS image build, boot, hardware,
  display regression, and failure-path validation remain necessary.

On the tested laptop, a driver change requires full power-off/on. Keep the packaged
working driver intact and stage experiments as a user non-packaged override. To
revert, remove the experimental override (or restore its known-good backup) and
cold boot. Before merge, also agree a private-ABI/versioning and trusted-access
policy. Do not deploy this draft as a production graphics package.

## Provenance and license review

The source prototype declares MIT for native Haiku code. It explicitly used
Genode's GPU driver as its hardware model and describes its context register image
as transcribed from Genode. That record must not be erased or turned into an
unqualified MIT-only claim.

Reference pinned during review: Genode commit
`0f275e7afaab44e9a45cecd0eac0b88691278477`, particularly
`repos/os/src/driver/gpu/intel/{context,context_descriptor,ppgtt,mmio,reset,commands}.h`
and `main.cc`. Genode's headers name Genode Labs GmbH and AGPL version 3. The exact
root license, including its linking exception and AGPL text, is retained verbatim
in `COPYING.Genode`.

For review, the Genode-modeled driver files conservatively retain both the native
MIT notice and `LicenseRef-Genode-AGPL-3.0-with-linking-exception`. This identifier
refers to COPYING.Genode. It records the reference's license; it does not establish
that every hardware offset or algorithm is protected expression, or resolve which
portions are derivative. No Genode template classes or source files were copied
wholesale into the native implementation. The licensing of the modeled portions
still needs maintainer/council review under D3. A decision to accept this license,
obtain other permission, or replace those portions from an acceptable source is
required before merge. The kernel add-on packaging constraint in D3 also needs an
explicit decision; this draft does not silently change that policy.

The DRM UAPI subset records transcription from libdrm 2.4.125/Linux DRM definitions.
The Precision Insight, VA Linux Systems and Tungsten Graphics permissive notices
are preserved in drm_uapi.h. The native ABI and regression program are MIT.
All inherited RenkuOS/Haiku file notices are preserved.

## Review size

This full import exceeds the 1000-line cap. It adds a new memory/submission stack;
removing context, PPGTT or completion code would make the tested iris path unusable.
The draft requires a maintainer-approved size exception or an agreed staged series.
A possible series is: private ABI/register definitions and device publication;
GEM/GGTT management; contexts/PPGTT and submission; synchronization/regressions.
That split needs buildable boundaries/refactoring, rather than artificial commits
that leave unresolved symbols. No size-exemption label or CI bypass is applied by
this submission.
