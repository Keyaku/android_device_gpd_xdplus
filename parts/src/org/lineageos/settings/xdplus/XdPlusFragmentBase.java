/*
 * Shared plumbing for the GPD XD+ tweak pages.
 *
 * None of these pages does privileged work itself: a toggle sets a
 * sys.xdplus.* / persist.sys.xdplus.* property and an init service dispatches
 * the root-side action. Anything backed by Settings.Global is applied directly.
 *
 * This uses the platform preference framework (android.preference) rather than
 * androidx: it is deprecated but present on API 30, and it keeps this app free
 * of static library dependencies. Nested pages are navigated by the host
 * activity, not by the framework -- see XdPlusPartsActivity.
 */

package org.lineageos.settings.xdplus;

import android.os.SystemProperties;
import android.preference.ListPreference;
import android.preference.PreferenceFragment;
import android.preference.SwitchPreference;
import android.text.TextUtils;
import android.widget.Toast;

public abstract class XdPlusFragmentBase extends PreferenceFragment {

    // Momentary action dispatch prop -- init resets it to "none" after handling.
    protected static final String PROP_ACTION = "sys.xdplus.action";

    /** Boolean property toggle: checked == property is set and not "0". */
    protected SwitchPreference bindSwitch(String key, String prop) {
        return bindSwitch(key, prop, false);
    }

    /** As above, for a property whose unset state means on. */
    protected SwitchPreference bindSwitch(String key, String prop, boolean def) {
        final SwitchPreference pref = (SwitchPreference) findPreference(key);
        pref.setChecked(getBoolProp(prop, def));
        pref.setOnPreferenceChangeListener((p, v) -> {
            SystemProperties.set(prop, ((Boolean) v) ? "1" : "0");
            return true;
        });
        return pref;
    }

    /** Value list whose summary tracks the selected entry. */
    protected ListPreference bindList(String key, String prop, String def) {
        final ListPreference pref = (ListPreference) findPreference(key);
        pref.setValue(SystemProperties.get(prop, def));
        pref.setSummary(pref.getEntry());
        pref.setOnPreferenceChangeListener((p, v) -> {
            SystemProperties.set(prop, (String) v);
            pref.setValue((String) v);
            pref.setSummary(pref.getEntry());
            return true;
        });
        return pref;
    }

    /** As above, with a summary format whose one argument is the selected entry. */
    protected ListPreference bindList(String key, String prop, String def, int summaryRes) {
        final ListPreference pref = (ListPreference) findPreference(key);
        pref.setValue(SystemProperties.get(prop, def));
        pref.setSummary(getString(summaryRes, pref.getEntry()));
        pref.setOnPreferenceChangeListener((p, v) -> {
            SystemProperties.set(prop, (String) v);
            pref.setValue((String) v);
            pref.setSummary(getString(summaryRes, pref.getEntry()));
            return true;
        });
        return pref;
    }

    protected void dispatch(String action, int toastRes) {
        SystemProperties.set(PROP_ACTION, action);
        Toast.makeText(getContext(), toastRes, Toast.LENGTH_LONG).show();
    }

    protected static boolean getBoolProp(String prop) {
        return getBoolProp(prop, false);
    }

    protected static boolean getBoolProp(String prop, boolean def) {
        final String v = SystemProperties.get(prop, def ? "1" : "0");
        return !TextUtils.isEmpty(v) && !"0".equals(v);
    }
}
