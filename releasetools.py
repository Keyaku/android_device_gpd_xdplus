#
# Copyright (C) 2026 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# An OTA for this device carries system and boot only, so a feature split across
# the vendor partition and the kernel applies half. Refuse to install onto a
# vendor partition this build does not know about.
#
# The marker is read out of /vendor/build.prop with file_getprop, never with
# edify's getprop, which returns the running recovery's own properties.
#
# This gate guards a bare bacon zip, which writes system and boot only. The
# published package writes vendor itself, and inject_vendor.sh strips the gate
# when it adds that write -- keeping it would abort the from-stock install.

VENDOR_REV_PROP = "ro.vendor.xdplus.rev"
# rev 3 patches the audio HAL's HDMI sink-format check, which is stubbed to always
# refuse in the OEM blob and therefore kept AUX_DIGITAL from ever being opened.
# rev 2 adds hwcomposer.xdplus.so and the ro.hardware.hwcomposer line that selects it.
# Older revs are still accepted: both are vendor-side capabilities that nothing on the
# system side depends on, so a build installs and runs on an older partition -- it just
# does not get the mirror fix (rev 2) or HDMI audio (rev 3).
VENDOR_REV_ACCEPTED = ("1", "2", "3")

VENDOR_DEVICE = "/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/vendor"
VENDOR_MOUNT = "/vendor"


def FullOTA_Assertions(info):
    read_rev = 'file_getprop("{}/build.prop", "{}")'.format(
        VENDOR_MOUNT, VENDOR_REV_PROP)
    accepted = " || ".join(
        '{} == "{}"'.format(read_rev, rev) for rev in VENDOR_REV_ACCEPTED)

    # TWRP mounts vendor itself, and mounting the same device a second time
    # fails with EBUSY, so reuse whatever is already there.
    info.script.AppendExtra("""
ifelse(is_mounted("{mount}") == "",
    mount("ext4", "EMMC", "{device}", "{mount}", "ro"),
    "{mount}");
ifelse(is_mounted("{mount}") == "",
    abort("Cannot read the vendor partition, so this package refuses to install. Boot to recovery and make sure {device} is readable."),
    ifelse({accepted},
        ui_print("Vendor partition accepted."),
        ifelse({read_rev} == "",
            abort("Your vendor partition predates this build and would leave the device half-updated. Install the published release package, which writes vendor itself."),
            abort("This build does not support vendor revision " + {read_rev} + ". Install the published release package, which writes vendor itself."))));
""".format(device=VENDOR_DEVICE, mount=VENDOR_MOUNT,
           accepted=accepted, read_rev=read_rev))
