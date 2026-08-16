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

VENDOR_REV_PROP = "ro.vendor.xdplus.rev"
# rev 2 adds hwcomposer.xdplus.so and the ro.hardware.hwcomposer line that selects it.
# rev 1 is still accepted: nothing on the system side depends on the wrapper, so a build
# installs and runs on the older partition -- it just does not get the mirror fix.
VENDOR_REV_ACCEPTED = ("1", "2")

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
            abort("Your vendor partition predates this build and would leave the device half-updated. Flash the vendor zip published alongside this release first, then install this package."),
            abort("This build does not support vendor revision " + {read_rev} + ". Flash the vendor zip published alongside this release."))));
""".format(device=VENDOR_DEVICE, mount=VENDOR_MOUNT,
           accepted=accepted, read_rev=read_rev))
