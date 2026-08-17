/*
 * GPD XD+ → SELinux.
 *
 * The build ships permissive; this page turns enforcing on for one device
 * without moving that default, and shows what enforcing would have blocked.
 * Denials are collected by the xdplus_avcd service into a log on /data —
 * nothing here is privileged, it reads that file and counts the lines.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;
import android.os.SystemProperties;

import android.preference.Preference;
import android.preference.PreferenceCategory;
import android.preference.PreferenceScreen;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;


public class XdPlusSelinuxSettings extends XdPlusFragmentBase {

    private static final String KEY_ENFORCE = "xdplus_selinux_enforce";
    private static final String KEY_AVCLOG = "xdplus_selinux_avclog";
    private static final String KEY_DENIALS = "xdplus_selinux_denials";
    private static final String KEY_CLEAR = "xdplus_selinux_clear";

    // Read by xdplus_tweaks at boot, and by its `selinux` action on demand.
    private static final String PROP_ENFORCE = "persist.sys.xdplus.selinux_enforce";
    // init starts/stops xdplus_avcd on this property's edges.
    private static final String PROP_AVCLOG = "persist.sys.xdplus.avclog";

    private static final String LOG_PATH = "/data/misc/xdplus/avc-denials.log";
    private static final int MAX_SHOWN = 60;

    private static final Pattern DENIAL = Pattern.compile(
            "\\{ ([^}]*) \\} for .*?scontext=(\\S+) tcontext=(\\S+) tclass=(\\S+)");

    private PreferenceCategory mDenials;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_selinux_settings);

        bindSwitch(KEY_ENFORCE, PROP_ENFORCE);
        bindSwitch(KEY_AVCLOG, PROP_AVCLOG);
        mDenials = (PreferenceCategory) findPreference(KEY_DENIALS);
    }

    @Override
    public void onResume() {
        super.onResume();
        findPreference(KEY_ENFORCE).setSummary(
                getString(R.string.xdplus_selinux_enforce_summary, currentMode()));
        showDenials();
    }

    @Override
    public boolean onPreferenceTreeClick(PreferenceScreen screen, Preference preference) {
        if (KEY_CLEAR.equals(preference.getKey())) {
            dispatch("avclog_clear", R.string.xdplus_selinux_clear_toast);
            mDenials.removeAll();
            return true;
        }
        return super.onPreferenceTreeClick(screen, preference);
    }

    // The live mode, not the boot argument: enforcing can be turned on after boot.
    private String currentMode() {
        boolean enforcing;
        try (BufferedReader r = new BufferedReader(new FileReader("/sys/fs/selinux/enforce"))) {
            enforcing = "1".equals(r.readLine().trim());
        } catch (Exception e) {
            enforcing = !"permissive".equals(SystemProperties.get("ro.boot.selinux", ""));
        }
        return getString(enforcing
                ? R.string.xdplus_selinux_enforcing
                : R.string.xdplus_selinux_permissive);
    }

    /** One entry per distinct rule, newest first, with how often it was seen. */
    private void showDenials() {
        mDenials.removeAll();

        final File f = new File(LOG_PATH);
        if (!f.canRead()) {
            mDenials.addPreference(note(getString(SystemProperties.getBoolean(PROP_AVCLOG, false)
                    ? R.string.xdplus_selinux_none
                    : R.string.xdplus_selinux_off)));
            return;
        }

        final Map<String, int[]> counts = new LinkedHashMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                final Matcher m = DENIAL.matcher(line);
                if (!m.find()) {
                    continue;
                }
                final String key = m.group(2) + " → " + m.group(3)
                        + "\n" + m.group(4) + " { " + m.group(1) + " }";
                final int[] n = counts.get(key);
                if (n == null) {
                    counts.put(key, new int[] {1});
                } else {
                    n[0]++;
                }
            }
        } catch (Exception e) {
            mDenials.addPreference(note(getString(R.string.xdplus_selinux_unreadable)));
            return;
        }

        if (counts.isEmpty()) {
            mDenials.addPreference(note(getString(R.string.xdplus_selinux_none)));
            return;
        }

        final List<String> keys = new ArrayList<>(counts.keySet());
        Collections.reverse(keys);
        int shown = 0;
        for (String key : keys) {
            if (shown++ >= MAX_SHOWN) {
                break;
            }
            final int n = counts.get(key)[0];
            final int split = key.indexOf('\n');
            final Preference p = new Preference(getContext());
            p.setSelectable(false);
            p.setTitle(key.substring(split + 1));
            p.setSummary(getString(R.string.xdplus_selinux_denial_summary,
                    key.substring(0, split), n));
            mDenials.addPreference(p);
        }
    }

    private Preference note(String text) {
        final Preference p = new Preference(getContext());
        p.setSelectable(false);
        p.setSummary(text);
        return p;
    }
}
