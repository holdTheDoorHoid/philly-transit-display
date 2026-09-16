# PlatformIO extra script for [env:ui-sim] (platformio.ini): compiles firmware/sim/*.cpp into the
# host build. `build_src_filter` can only select files under src/, and the simulator must not
# live in src/ where every ESP32 environment would try to compile it, so the documented
# BuildSources() hook adds the directory instead. sim/stubs/ holds headers only.
#
# Must be a `pre:` script (BuildSources is refused once the program target exists), and at that
# point the library dependency finder has not yet added lvgl/ArduinoJson/lib/* to the include
# path - those go onto `projenv`, not `env` - so they are added here by hand.
import glob
import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
libdeps = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))
include_paths = [
    env.subst("$PROJECT_SRC_DIR"),
    os.path.join(libdeps, "lvgl"),
    os.path.join(libdeps, "ArduinoJson", "src"),
] + sorted(glob.glob(os.path.join(project_dir, "lib", "*", "include")))
env.Append(CPPPATH=include_paths)

env.BuildSources(
    os.path.join("$BUILD_DIR", "sim"),
    os.path.join("$PROJECT_DIR", "sim"),
    src_filter="+<*.cpp>",
)
