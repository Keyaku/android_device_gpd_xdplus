/*
 * GPD XD+ → Display output. mini-HDMI bring-up and teardown, plus the knobs the
 * bring-up recipe depends on. Each property is documented at its use site in
 * the HDMI bring-up script and in the display HAL shim.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;

import android.preference.Preference;
import android.preference.PreferenceScreen;


public class XdPlusDisplaySettings extends XdPlusFragmentBase {

    private static final String KEY_HDMI_UP = "xdplus_hdmi_up";
    private static final String KEY_HDMI_DOWN = "xdplus_hdmi_down";
    private static final String KEY_HDMI_RES = "xdplus_hdmi_res";
    private static final String KEY_HDMI_PIN_LAYER = "xdplus_hdmi_pin_layer";

    private static final String PROP_HDMI_RES = "persist.sys.xdplus.hdmi_res";
    // Polled by the xdplus-mirrorpin service, so the toggle applies within a couple of
    // seconds and needs no dispatch. Unset means on.
    private static final String PROP_HDMI_PIN_LAYER = "persist.sys.xdplus.mirror_pin";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_display_settings);

        bindSwitch(KEY_HDMI_PIN_LAYER, PROP_HDMI_PIN_LAYER, true);
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
