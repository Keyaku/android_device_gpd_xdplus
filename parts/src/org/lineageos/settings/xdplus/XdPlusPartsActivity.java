/*
 * Host for the GPD XD+ pages.
 *
 * This app is reached from Settings as an injected dashboard tile -- Settings
 * discovers the activity below through its manifest metadata at runtime, which
 * is why nothing in packages/apps/Settings has to be modified to carry this
 * menu. A forked Settings would have put a GPD XD+ entry on every device built
 * from that fork; a tile only exists where this app is installed.
 *
 * Sub-pages are opened here rather than by the preference framework: the
 * platform PreferenceFragment shows a nested PreferenceScreen in a dialog,
 * which looks nothing like the rest of Settings. The landing page's entries
 * carry android:fragment and XdPlusSettings hands the class name back here.
 */

package org.lineageos.settings.xdplus;

import android.app.Activity;
import android.app.Fragment;
import android.os.Bundle;

public class XdPlusPartsActivity extends Activity {

    /** Which fragment to show; absent means the landing page. */
    public static final String EXTRA_FRAGMENT = "org.lineageos.settings.xdplus.FRAGMENT";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) {
            return;  // the framework restores the fragment itself
        }

        final String name = getIntent().getStringExtra(EXTRA_FRAGMENT);
        show(name != null ? name : XdPlusSettings.class.getName(), false);
    }

    /** Replace the current page, optionally leaving it on the back stack. */
    void show(String fragmentName, boolean addToBackStack) {
        final Fragment f = Fragment.instantiate(this, fragmentName);
        final android.app.FragmentTransaction t =
                getFragmentManager().beginTransaction().replace(android.R.id.content, f);
        if (addToBackStack) {
            t.addToBackStack(null);
        }
        t.commit();
    }
}
