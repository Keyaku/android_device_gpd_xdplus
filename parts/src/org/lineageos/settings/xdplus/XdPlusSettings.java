/*
 * GPD XD+ device tweaks — the landing page for this port's runtime toggles.
 *
 * Two doors plus one switch. The flat version had grown to three unrelated
 * groups on one scroll, so it was split; the "Experimental patches" door was
 * later dropped because it had nothing experimental left on it -- the Wi-Fi
 * standard label became unconditional, the shader-compile notification moved
 * next to the shader cache, and hiding crash dialogs is an ordinary handheld
 * preference, so it sits here directly.
 *
 * Sub-pages are opened through the host activity: the landing entries carry
 * android:fragment, and the platform preference framework would otherwise show
 * a nested screen in a dialog.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;
import android.provider.Settings;

import android.preference.Preference;
import android.preference.PreferenceScreen;
import android.preference.SwitchPreference;

public class XdPlusSettings extends XdPlusFragmentBase {

    private static final String KEY_HIDE_ERROR_DIALOGS = "xdplus_hide_error_dialogs";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_settings);

        // Settings.Global rather than a system property, so it is applied
        // directly here instead of being dispatched through init.
        final SwitchPreference hideErr =
                (SwitchPreference) findPreference(KEY_HIDE_ERROR_DIALOGS);
        hideErr.setChecked(Settings.Global.getInt(getActivity().getContentResolver(),
                Settings.Global.HIDE_ERROR_DIALOGS, 0) != 0);
        hideErr.setOnPreferenceChangeListener((p, v) -> {
            Settings.Global.putInt(getActivity().getContentResolver(),
                    Settings.Global.HIDE_ERROR_DIALOGS, ((Boolean) v) ? 1 : 0);
            return true;
        });
    }

    /*
     * The landing entries are plain Preferences carrying android:fragment. The
     * platform framework does nothing with that attribute, so route it.
     */
    @Override
    public boolean onPreferenceTreeClick(PreferenceScreen screen, Preference preference) {
        final String fragment = preference.getFragment();
        if (fragment != null) {
            ((XdPlusPartsActivity) getActivity()).show(fragment, true);
            return true;
        }
        return super.onPreferenceTreeClick(screen, preference);
    }
}
