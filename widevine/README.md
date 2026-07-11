# Widevine DRM

Brings up `drm-widevine-hal-1-0` (`android.hardware.drm@1.0::IDrmFactory/ICryptoFactory` `widevine`) on LOS 18.1 over the prebuilt 8.1 vendor.

## Root cause

Vendor's `libwvhidl.so` and `libwvdrmengine.so` (built for API 27) import `google::protobuf::internal::empty_string_` (`_ZN6google8protobuf8internal13empty_string_E`). R's `libprotobuf-cpp-lite.so` renamed that data symbol to `fixed_address_empty_string`. The device's `[legacy]` single-namespace `/linkerconfig/ld.config.txt` searches `/system/lib` before `/vendor/lib`, so R's protobuf resolves first for the shared soname and the service crash-loops at link time.

There is no `/vendor/lib/libprotobuf-cpp-lite.so` — dropping the O-era one there does nothing, because the shared soname still resolves to `/system/lib` first.

## Fix

Rename the dependency to a `/vendor`-only soname:

- `libprotobuf-cpp-v27.so` — the VNDK v27 (O) `libprotobuf-cpp-lite.so`, which still exports `empty_string_`. Installed to `/vendor/lib/`.
- `libwvhidl.so`, `libwvdrmengine.so` — `DT_NEEDED` patched in place from `libprotobuf-cpp-lite.so\0` to `libprotobuf-cpp-v27.so\0\0` (equal 24-byte length; extra NUL is a harmless empty strtab entry). Installed over `/vendor/lib/` and `/vendor/lib/mediadrm/`.

## Apply

Vendor is a frozen prebuilt never rebuilt from source, so this deploys directly to the `/vendor` partition (survives system-only flashes). Re-run only after a `vendor.img` reflash.

```
adb push . /data/local/tmp/widevine && adb shell su -c 'sh /data/local/tmp/widevine/install.sh'
```

## Verify

```
adb shell getprop init.svc.drm-widevine-hal-1-0   # -> running (not restarting)
adb shell 'logcat -d | grep "Registered android.hardware.drm@1.0"'
# -> IDrmFactory/widevine + ICryptoFactory/widevine
```

Security level (L1 vs L3) still needs an app-level query (DRM Info / Netflix). The engine has the L1 OEMCrypto adapter and an `IDM1013` provisioning dir exists in `/vendor/nvdata`, so L1 is plausible but unconfirmed; L1 also depends on the OEMCrypto TEE path being functional.
