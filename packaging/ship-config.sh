# ship-config.sh — the shipped engine-build / driver pair, sourced by
# packaging/release-build.sh (expects $BAR to be set).
#
# Referenced by release-build.sh and build-engine.sh but absent from the public
# tree; this restores it with the defaults the scripts already document:
# build-engine.sh builds into $BAR/build-engine, and build-mesa-kk.sh notes that
# the release pipeline uses deps/mesa-native-release (never the dev driver).
# Both stay overridable through ENGINE_BUILD / MESA_PREFIX as before.
SHIP_ENGINE_BUILD="${SHIP_ENGINE_BUILD:-$BAR/build-engine}"
SHIP_MESA_PREFIX="${SHIP_MESA_PREFIX:-$BAR/deps/mesa-native-release}"
