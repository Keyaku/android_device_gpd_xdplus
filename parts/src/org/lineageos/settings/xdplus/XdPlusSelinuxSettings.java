/*
 * GPD XD+ → SELinux.
 *
 * The build ships permissive; this page turns enforcing on for one device
 * without moving that default, and exports what the policy denied.
 *
 * Nothing here is privileged, and deliberately so: the page only sets
 * properties. Reading denials needs the kernel ring buffer and writing them
 * needs a directory outside this app's sandbox, so both belong to
 * xdplus_tweaks, which has a domain for it. An earlier version parsed the log
 * in-app and had to be given access to it — that is the access this page no
 * longer needs.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;
import android.os.SystemProperties;

import android.preference.Preference;
import android.preference.PreferenceScreen;

import java.io.BufferedReader;
import java.io.FileReader;


public class XdPlusSelinuxSettings extends XdPlusFragmentBase {

    private static final String KEY_ENFORCE = "xdplus_selinux_enforce";
    private static final String KEY_AVCLOG = "xdplus_selinux_avclog";
    private static final String KEY_EXPORT = "xdplus_selinux_export";
    private static final String KEY_CLEAR = "xdplus_selinux_clear";

    // Read by xdplus_tweaks at boot, and by its `selinux` action on demand.
    private static final String PROP_ENFORCE = "persist.sys.xdplus.selinux_enforce";
    // init starts/stops xdplus_avcd on this property's edges.
    private static final String PROP_AVCLOG = "persist.sys.xdplus.avclog";
    // xdplus_tweaks publishes the live mode here because system_app cannot read
    // /sys/fs/selinux/enforce under enforcement.
    private static final String PROP_MODE = "sys.xdplus.selinux_mode";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_selinux_settings);

        bindSwitch(KEY_ENFORCE, PROP_ENFORCE);
        bindSwitch(KEY_AVCLOG, PROP_AVCLOG);
    }

    @Override
    public void onResume() {
        super.onResume();
        findPreference(KEY_ENFORCE).setSummary(
                getString(R.string.xdplus_selinux_enforce_summary, currentMode()));
    }

    @Override
    public boolean onPreferenceTreeClick(PreferenceScreen screen, Preference preference) {
        if (KEY_EXPORT.equals(preference.getKey())) {
            dispatch("avclog_export", R.string.xdplus_selinux_export_toast);
            return true;
        }
        if (KEY_CLEAR.equals(preference.getKey())) {
            dispatch("avclog_clear", R.string.xdplus_selinux_clear_toast);
            return true;
        }
        return super.onPreferenceTreeClick(screen, preference);
    }

    // The live mode, not the boot argument: enforcing can be turned on after boot.
    // The privileged side publishes it because system_app cannot read selinuxfs.
    private String currentMode() {
        boolean enforcing;
        String mode = SystemProperties.get(PROP_MODE, "");
        if ("enforcing".equals(mode)) {
            enforcing = true;
        } else if ("permissive".equals(mode)) {
            enforcing = false;
        } else {
            enforcing = SystemProperties.getBoolean(PROP_ENFORCE, false)
                    || !"permissive".equals(SystemProperties.get("ro.boot.selinux", ""));
        }
        return getString(enforcing
                ? R.string.xdplus_selinux_enforcing
                : R.string.xdplus_selinux_permissive);
    }
}
