# Plugin identity: the ONE place to set the company name, codes and IDs.
#
# Everything marked PLACEHOLDER must be replaced before you ship anything.
# Changing the manufacturer or plugin code later makes hosts treat the plugin
# as a different product (saved projects will no longer find it), so settle
# on final values before the first public build.

set(LITHIUM_PRODUCT_NAME "Lithium Drums")

# PLACEHOLDER: your company or personal brand, shown by hosts as the vendor.
set(LITHIUM_COMPANY_NAME "Placeholder Audio")

# PLACEHOLDER: four characters, at least one upper-case letter. Register a
# unique code with Apple if you ship an AU.
set(LITHIUM_MANUFACTURER_CODE "Plhd")

# PLACEHOLDER: four characters, at least one upper-case letter, unique among
# your own plugins.
set(LITHIUM_PLUGIN_CODE "Ldrm")

# PLACEHOLDER: reverse-DNS identifiers derived from your domain.
set(LITHIUM_BUNDLE_ID "com.placeholder.lithiumdrums")
set(LITHIUM_CLAP_ID "com.placeholder.lithium-drums")
