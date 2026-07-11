/*
 * android::hardware::IPCThreadState::disableBackgroundScheduling(bool) shim
 * for the prebuilt 8.1 vendor wpa_supplicant. The symbol vanished from R's
 * libhidlbase/libhwbinder; it only toggled a SCHED_BATCH policy flag for
 * hwbinder threads, so a no-op is safe. Defined with C linkage under the
 * exact mangled name to avoid needing the class definition.
 */
extern "C" void _ZN7android8hardware14IPCThreadState27disableBackgroundSchedulingEb(bool) {}
