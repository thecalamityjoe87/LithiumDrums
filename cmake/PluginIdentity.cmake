# Plugin identity: the ONE place to set the company name, codes and IDs.
#
# Changing the manufacturer or plugin code later makes hosts treat the plugin
# as a different product (saved projects will no longer find it), so these
# must not change after the first public build.

set(LITHIUM_PRODUCT_NAME "Lithium Drums")

# The brand hosts show as the plugin's vendor.
set(LITHIUM_COMPANY_NAME "Calamity Audio")

# The author, for copyright notices and the About panel.
set(LITHIUM_AUTHOR "Isaac Joseph")
set(LITHIUM_WEBSITE "https://github.com/thecalamityjoe87")

# Four characters, at least one upper-case letter. Apple reserves all-lower-
# case codes; register this one with Apple before shipping the AU widely.
set(LITHIUM_MANUFACTURER_CODE "Clmy")

# Four characters, at least one upper-case letter, unique among your own
# plugins.
set(LITHIUM_PLUGIN_CODE "Ldrm")

# Reverse-DNS identifiers. They only need to be unique; io.github.<user> is
# the usual choice when the brand has no domain of its own.
set(LITHIUM_BUNDLE_ID "io.github.thecalamityjoe87.lithiumdrums")
set(LITHIUM_CLAP_ID "io.github.thecalamityjoe87.lithium-drums")
