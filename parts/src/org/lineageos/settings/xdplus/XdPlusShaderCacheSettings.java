/*
 * GPD XD+ → Vulkan shader cache.
 *
 * The Vulkan shim (device/gpd/xdplus/vulkan/vkshim.c) keeps a pipeline cache per
 * app so a game only pays its shader compile once. A pipeline cache blob is
 * opaque, so entries cannot be evicted individually and the file only ever
 * grows — hence a per-app limit and a way to wipe it. The shim freezes a cache
 * at its last good size once the next write would cross the limit, rather than
 * resetting it, which would throw away every compiled shader at once.
 *
 * Each cache lives inside its own app's sandbox
 * (/data/user/<user>/<pkg>/cache/vkshim.pcache), which has two consequences for
 * this page. There is no all-apps limit any more — the platform's own low-space
 * cache reclaim is what bounds the total — and Settings, running as system
 * rather than as each app, can neither size nor delete those files. So the
 * summaries report the chosen limit only, and "clear" is a request the shims
 * honour themselves on their next start.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;
import android.os.SystemProperties;
import android.widget.Toast;

import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceScreen;


public class XdPlusShaderCacheSettings extends XdPlusFragmentBase {

    private static final String KEY_VKCACHE_MAX = "xdplus_vkcachemax";
    private static final String KEY_VKCACHE_CLEAR = "xdplus_vkcache_clear";
    private static final String KEY_VKNOTIFY = "xdplus_vknotify";

    // In MB, and takes effect on the next launch of the app concerned.
    private static final String PROP_VKCACHE_MAX = "persist.sys.xdplus.vkcachemax";
    private static final String ACTION_CLEAR = "vkcache_clear";
    // Read by xdplus_tweaks.sh vknotify (and double-checked by VkCompileReceiver).
    private static final String PROP_VKNOTIFY = "persist.sys.xdplus.vknotify";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_shadercache_settings);

        bindList(KEY_VKNOTIFY, PROP_VKNOTIFY, "notification");

        setUpCacheLimit(KEY_VKCACHE_MAX, PROP_VKCACHE_MAX, "64");
    }

    @Override
    public boolean onPreferenceTreeClick(PreferenceScreen screen, Preference preference) {
        if (KEY_VKCACHE_CLEAR.equals(preference.getKey())) {
            clearShaderCache();
            return true;
        }
        return super.onPreferenceTreeClick(screen, preference);
    }

    // A list of MB values; the summary repeats the choice. It used to add "N in
    // use now", which is no longer readable from here — the caches sit in app
    // sandboxes.
    private void setUpCacheLimit(String key, String prop, String def) {
        final ListPreference pref = bindList(key, prop, def);
        pref.setOnPreferenceChangeListener((p, v) -> {
            SystemProperties.set(prop, (String) v);
            pref.setValue((String) v);
            updateCacheSummary(pref);
            return true;
        });
        updateCacheSummary(pref);
    }

    private void updateCacheSummary(ListPreference pref) {
        pref.setSummary(getString(R.string.xdplus_vkcache_summary, pref.getEntry()));
    }

    // Nothing here can delete another app's cache: SELinux allows only the app
    // itself and installd to unlink app_data_file, and root is no exception.
    // The clear is a handshake instead — xdplus_tweaks bumps a generation
    // property and each shim drops its own cache when it next starts. An app
    // already running keeps its copy until it exits, which was true of the old
    // root-side delete too.
    private void clearShaderCache() {
        SystemProperties.set(PROP_ACTION, ACTION_CLEAR);
        Toast.makeText(getActivity(), R.string.xdplus_vkcache_clear_toast,
                Toast.LENGTH_LONG).show();
    }
}
