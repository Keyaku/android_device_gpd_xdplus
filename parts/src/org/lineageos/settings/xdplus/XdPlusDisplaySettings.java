/*
 * GPD XD+ → Display output. mini-HDMI bring-up and teardown, plus the knobs the
 * bring-up recipe depends on. Each property is documented at its use site in
 * the HDMI bring-up script and in the display HAL shim.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;
import android.os.SystemProperties;

import android.preference.Preference;
import android.preference.PreferenceScreen;
import android.preference.SwitchPreference;


public class XdPlusDisplaySettings extends XdPlusFragmentBase {

    private static final String KEY_HDMI_UP = "xdplus_hdmi_up";
    private static final String KEY_HDMI_DOWN = "xdplus_hdmi_down";
    private static final String KEY_HDMI_MIRROR_MODE = "xdplus_hdmi_mirror_mode";
    private static final String KEY_HDMI_RES = "xdplus_hdmi_res";
    private static final String KEY_HDMI_NOVSYNC = "xdplus_hdmi_novsync";

    // SurfaceFlinger reads this once at startup: 1 = forced validate for
    // external displays (plain extension mode), 0 = mirror-capable. The toggle
    // is therefore reboot-to-apply, and its sense is inverted here
    // ("mirror mode" checked == prop 0).
    private static final String PROP_HDMI_FORCE_VALIDATE =
            "persist.sys.xdplus.hdmi_force_validate";
    private static final String PROP_HDMI_RES = "persist.sys.xdplus.hdmi_res";
    // Read by xdplus_tweaks.sh during hdmi_up (not applied live).
    private static final String PROP_HDMI_NOVSYNC = "persist.sys.xdplus.hdmi_novsync";
    // Read by PhoneWindowManager on every F12 (the spare "Gamepad Mapper"
    // button): when armed, that button bounces the display off/on to recover a
    // frozen pipeline. Applied live — the policy reads it per press.

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_display_settings);

        // Inverted sense: checked == hdmi_force_validate 0 == mirror-capable.
        // SF only reads the prop at startup, so this takes effect on reboot; the
        // summary says so and hdmi_up refuses with a log if run before one.
        final SwitchPreference mirrorMode =
                (SwitchPreference) findPreference(KEY_HDMI_MIRROR_MODE);
        mirrorMode.setChecked(
                "0".equals(SystemProperties.get(PROP_HDMI_FORCE_VALIDATE, "1")));
        mirrorMode.setOnPreferenceChangeListener((p, v) -> {
            SystemProperties.set(PROP_HDMI_FORCE_VALIDATE, ((Boolean) v) ? "0" : "1");
            return true;
        });

        // Consumed by the bring-up script, so there is nothing to poke here.
        bindSwitch(KEY_HDMI_NOVSYNC, PROP_HDMI_NOVSYNC);
        bindList(KEY_HDMI_RES, PROP_HDMI_RES, "2");
    }

    @Override
    public boolean onPreferenceTreeClick(PreferenceScreen screen, Preference preference) {
        final String key = preference.getKey();
        if (KEY_HDMI_UP.equals(key)) {
            dispatch("hdmi_up", R.string.xdplus_hdmi_up_toast);
            return true;
        }
        if (KEY_HDMI_DOWN.equals(key)) {
            dispatch("hdmi_down", R.string.xdplus_hdmi_down_toast);
            return true;
        }
        return super.onPreferenceTreeClick(screen, preference);
    }
}
