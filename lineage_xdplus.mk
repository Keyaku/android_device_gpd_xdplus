# 64-bit primary zygote with 32-bit app support (arm64-v8a,armeabi-v7a,armeabi)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

$(call inherit-product, device/gpd/xdplus/xdplus.mk)

# Common CM stuff
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Match the shipped Android 11 build fingerprint (ALLDOCUBE U1005E 8.1 vendor base)

PRODUCT_NAME := lineage_xdplus
PRODUCT_DEVICE := xdplus
PRODUCT_BRAND := GPD
PRODUCT_MANUFACTURER := GPD
PRODUCT_MODEL := xdplus

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRODUCT_DEVICE="xdplus"
