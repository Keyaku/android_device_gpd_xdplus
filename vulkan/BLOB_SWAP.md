# Swapping the vendor Vulkan blob — checklist

`vkshim.c` wraps `/vendor/lib{,64}/hw/vulkan.mt8173.so` and hardcodes several facts about **that** binary. Replacing the blob means re-checking every item here; each was measured against the DDK 1.9 (`4893595`) build and none of them re-derives itself at run time.

This file exists because the reasoning behind these lives in working notes that are not part of any repo, and because two of the items are invisible until something breaks strangely months later.

## Must change

**`SHIM_DRIVER_VERSION` (`vkshim.c`).** Currently `VK_MAKE_VERSION(1, 9, 0)`. The blob's raw `driverVersion` is the changelist `4893595`, which bit-decodes to a nonsense "1.170.2971" in every app that guesses IMG's packing, so the shim re-encodes it and keeps the real tag in `driverInfo`. **Apps key their shader caches on `driverVersion`** — leaving it unchanged across a blob swap makes them reuse pipelines built by the old compiler.

**Both `shim_driver_uuid` tables (`vkshim.c`, `#ifdef __LP64__`).** These are the blob's own `.note.gnu.build-id`, one per ABI, reported as `driverUUID`. Read the new ones with:

```
readelf -n /vendor/lib64/hw/vulkan.mt8173.so
readelf -n /vendor/lib/hw/vulkan.mt8173.so
```

⚠️ **The two ABIs have different build-ids** — copying one into both places is a silent error, because each build only compiles its own branch.

## Must re-check

**The `real_*` proc-addr cache (`resolve_identity_fns`).** These pointers are resolved once and never refreshed, so they outlive the instance they were queried from — which Vulkan forbids. It is inert **only** because this driver is a single closed blob that never unmaps its code and whose dispatch ignores the instance handle. A different blob need not behave that way, and the failure would be a use-after-free far from here.

**The NULL-framebuffer guard.** `shim_CmdBeginRenderPass` drops a Begin whose framebuffer is `VK_NULL_HANDLE`, because this blob dereferences it without testing (`IMG_vkCmdBeginRenderPass`, fault at offset `0x80` off a null base). The guard is keyed on the handle, not on any offset, so it needs no edit — but **confirm the fault still exists** before assuming the guard is still load-bearing, and confirm it is still harmless if it is not:

```
./tests/run.sh rp            # 6/6 expected
./tests/run.sh rp control    # expected to SIGSEGV; run.sh restores the property
```

**`maxPushDescriptors = 32`.** Not a value this driver supplied — it reports nothing, since the limit is only queryable through `vkGetPhysicalDeviceProperties2`, which a 1.0.49 driver lacks. 32 is a demonstrated floor plus the conventional value for this vendor. A newer blob may report it honestly, in which case the shim should stop inventing it. See `tests/README.md`.

**`vkCmdBlitImage` cannot resize.** The shim drops scaling blits and clamps samplers to level 0 because this blob gets both filters wrong. If a new blob blits correctly, that emulation becomes pure cost.

**The extension set.** Everything the shim advertises is something this blob lacks. A newer blob may export some natively, and advertising a name twice — or filtering out one the blob now implements, which `shim_CreateDevice` does by table — is how an app ends up on the shim's weaker path instead of the driver's real one.

## Re-run afterwards

```
./tests/run.sh              # extensions, vkCreateDevice with all of them, entry points
./tests/run.sh procaddr
./tests/run.sh twodev
./tests/run.sh rp
./tests/pushimg.sh          # push descriptors through a fragment shader
TYPE=ssbo ./tests/pushfunc.sh
```

⚠️ **The probes are not build-system modules and must never be installed into the image** — they are pushed to `/data/local/tmp` by their own scripts.
