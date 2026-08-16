# vkshim probes

Standalone on-device probes for `../vkshim.c`. They are **not** build-system modules and must never be installed into the image — they are built with an NDK and pushed to `/data/local/tmp` by their own script, so they can be run against a device without rebuilding the ROM.

## `vkext` — extension coverage

`./run.sh` builds and runs it. Set `ANDROID_NDK` if the script cannot find an NDK on its own.

It checks three things, in order:

1. What `vkEnumerateDeviceExtensionProperties` reports — the blob's own set plus whatever the shim appends.
2. That `vkCreateDevice` **succeeds with every reported extension enabled.** This is the regression test that matters most: the vendor blob rejects the whole call with `VK_ERROR_EXTENSION_NOT_PRESENT` on the first extension name it does not recognise, and every extension the shim advertises is by definition one of those. Without the filter in `shim_CreateDevice`, an app that enables what it was just told exists gets **no Vulkan device at all** — it never reaches a shim entry point to be helped by. A shim built without that filter fails this step with `VkResult -7`.
3. That each shim-implemented entry point resolves and answers sanely — `VK_KHR_get_memory_requirements2` against the 1.0 answer for the same resource, `VK_KHR_dedicated_allocation` into deliberately poisoned inputs, `VK_KHR_bind_memory2`, and both halves of `VK_KHR_maintenance3`.

⚠️ **The expected results track what the shim currently advertises.** Adding an extension to the shim means adding its check here, or a pass means only that the older claims still hold.

## `vkext push` — the maxPushDescriptors search, and why it fails

`./run.sh push` attempts to measure `VK_KHR_push_descriptor`'s one limit, which the blob supports but cannot report: the limit is only queryable through `vkGetPhysicalDeviceProperties2`, which a 1.0.49 driver does not have, so an app chaining `VkPhysicalDevicePushDescriptorPropertiesKHR` reads zero. The value is in no header in this tree, and guessing is bad in both directions — too high can corrupt or crash an app, too low denies it a feature the hardware has.

The search is a binary search for the largest descriptor count a push-flagged layout is accepted with, guarded by two gates it checks before believing any number: whether the driver validates at all (probed with an absurd count first), and whether the boundary is specific to the push flag (a plain layout at the same count is the control).

⚠️ **The first gate fires on this driver, at both object-creation points, so this route is CLOSED — do not re-derive it.** The blob accepts a push-flagged layout of 65536 descriptors, and a pipeline layout built from it, without complaint; it also accepts 65536 uniform buffers in a *plain* layout, well past its own reported `maxDescriptorSetUniformBuffers` of 256. It simply does not diagnose descriptor-set limits at creation time, so any boundary a search finds is the top of the search range and nothing else.

What is left is a functional test — actually pushing N descriptors, drawing, and checking the result — whose failure mode is undefined behaviour or a GPU hang, so it needs a driven session rather than an unattended run. Until that or a real value from DDK sources, **`maxPushDescriptors` stays 0**, which is the safe direction: an app that reads zero falls back to ordinary descriptor sets, and an app that calls `vkCmdPushDescriptorSetKHR` without querying is unaffected either way. ⚠️ Reporting a guessed value here is *more* dangerous than on a normal driver, precisely because nothing in this one will catch a claim that is too high.

⚠️ **The probe's own `debug.xdplus.*` gates are not reset by it.** With `debug.xdplus.vkmemext=0` the extension count drops and the maintenance3 check reports a failure — that is the gate working, not a regression. The shim caches each property on first use, so a changed gate needs a fresh process.
