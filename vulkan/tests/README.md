# vkshim probes

Standalone on-device probes for `../vkshim.c`. They are **not** build-system modules and must never be installed into the image — they are built with an NDK and pushed to `/data/local/tmp` by their own script, so they can be run against a device without rebuilding the ROM.

## `vkext` — extension coverage

`./run.sh` builds and runs it. Set `ANDROID_NDK` if the script cannot find an NDK on its own.

It checks three things, in order:

1. What `vkEnumerateDeviceExtensionProperties` reports — the blob's own set plus whatever the shim appends.
2. That `vkCreateDevice` **succeeds with every reported extension enabled.** This is the regression test that matters most: the vendor blob rejects the whole call with `VK_ERROR_EXTENSION_NOT_PRESENT` on the first extension name it does not recognise, and every extension the shim advertises is by definition one of those. Without the filter in `shim_CreateDevice`, an app that enables what it was just told exists gets **no Vulkan device at all** — it never reaches a shim entry point to be helped by. A shim built without that filter fails this step with `VkResult -7`.
3. That each shim-implemented entry point resolves and answers sanely — `VK_KHR_get_memory_requirements2` against the 1.0 answer for the same resource, `VK_KHR_dedicated_allocation` into deliberately poisoned inputs, `VK_KHR_bind_memory2`, and both halves of `VK_KHR_maintenance3`.

⚠️ **The expected results track what the shim currently advertises.** Adding an extension to the shim means adding its check here, or a pass means only that the older claims still hold.

⚠️ **The probe's own `debug.xdplus.*` gates are not reset by it.** With `debug.xdplus.vkmemext=0` the extension count drops and the maintenance3 check reports a failure — that is the gate working, not a regression. The shim caches each property on first use, so a changed gate needs a fresh process.
