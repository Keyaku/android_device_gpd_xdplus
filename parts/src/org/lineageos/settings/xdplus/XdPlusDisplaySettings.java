/*
 * GPD XD+ → Display output: the mini-HDMI knobs. Each property is documented
 * at its use site in the HDMI bring-up script and the HDMI daemon.
 */

package org.lineageos.settings.xdplus;

import android.os.Bundle;


public class XdPlusDisplaySettings extends XdPlusFragmentBase {

    private static final String KEY_HDMI_RES = "xdplus_hdmi_res";
    private static final String KEY_HDMI_SLEEP = "xdplus_hdmi_sleep";
    private static final String KEY_HWC_VDS = "xdplus_hwc_vds";

    private static final String PROP_HDMI_RES = "persist.sys.xdplus.hdmi_res";
    private static final String PROP_HDMI_SLEEP = "persist.sys.xdplus.hdmi_sleep";
    private static final String PROP_HWC_VDS = "persist.sys.xdplus.hwc_vds";

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.gpd_xdplus_display_settings);

        bindList(KEY_HDMI_RES, PROP_HDMI_RES, "2", R.string.xdplus_hdmi_res_summary);
        bindList(KEY_HDMI_SLEEP, PROP_HDMI_SLEEP, "0", R.string.xdplus_hdmi_sleep_summary);
        bindSwitch(KEY_HWC_VDS, PROP_HWC_VDS);
    }

}
