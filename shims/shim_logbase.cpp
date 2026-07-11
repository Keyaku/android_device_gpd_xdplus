/*
 * O-era android::base::LogMessage constructor shim for the prebuilt 8.1 vendor
 * blobs (libhwminijail, libnvram, sensors impl, wifi/widevine services, ...).
 *
 * Old (O/8.1):  LogMessage(const char* file, unsigned line, LogId, LogSeverity, int error)
 * New (R/11):   LogMessage(const char* file, unsigned line, LogId, LogSeverity, const char* tag, int error)
 *
 * Implemented as mangled-name trampolines so we don't have to re-declare the
 * class. Both C1 (complete) and C2 (base) constructor symbols are provided.
 */

extern "C" {

// R libbase constructors (still exported there).
void _ZN7android4base10LogMessageC1EPKcjNS0_5LogIdENS0_11LogSeverityES3_i(
		void* thisptr, const char* file, unsigned int line, int id, int severity,
		const char* tag, int error);
void _ZN7android4base10LogMessageC2EPKcjNS0_5LogIdENS0_11LogSeverityES3_i(
		void* thisptr, const char* file, unsigned int line, int id, int severity,
		const char* tag, int error);

// O-era constructors expected by the vendor blobs.
void _ZN7android4base10LogMessageC1EPKcjNS0_5LogIdENS0_11LogSeverityEi(
		void* thisptr, const char* file, unsigned int line, int id, int severity,
		int error) {
	_ZN7android4base10LogMessageC1EPKcjNS0_5LogIdENS0_11LogSeverityES3_i(
			thisptr, file, line, id, severity, nullptr, error);
}

void _ZN7android4base10LogMessageC2EPKcjNS0_5LogIdENS0_11LogSeverityEi(
		void* thisptr, const char* file, unsigned int line, int id, int severity,
		int error) {
	_ZN7android4base10LogMessageC2EPKcjNS0_5LogIdENS0_11LogSeverityES3_i(
			thisptr, file, line, id, severity, nullptr, error);
}

}  // extern "C"
