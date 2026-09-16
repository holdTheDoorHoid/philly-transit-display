"""PlatformIO pre-script for the `*-https` mirror envs (platformio.ini, "HTTPS prototype" block).

[esp32_base] defines BOARD_NAME from ${PIOENV}; in a mirror env that would be "cyd-3248S035R-https",
and web_server.cpp bakes BOARD_NAME into the PTD-BOARD marker that POST /api/ota compares with the
running firmware's (DESIGN.md 12). Left alone, a serial-flashed prototype image could only ever be
replaced over serial again, and the prototype could not be OTA'd onto a board running the normal
build. Strip the suffix so the prototype identifies as the board it is built for.

It has to be done on the raw BUILD_FLAGS option, not on CPPDEFINES: PlatformIO turns build_flags
into CPPDEFINES in the platform's main build script, which runs AFTER pre: scripts, so at this
point CPPDEFINES does not contain BOARD_NAME yet. And at this point the flag still reads
'-D BOARD_NAME="${PIOENV}"' literally - the ${PIOENV} placeholder is interpolated later too - so
the script substitutes the base env's name itself instead of looking for "-https" in a string
that does not contain it yet (both found out the hard way: the first two builds of this env
carried "PTD-BOARD:cyd-3248S035R-https;"). Check an image with
`strings firmware.bin | grep PTD-BOARD`.
"""
Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

base_env = env["PIOENV"].replace("-https", "")
flags = env.get("BUILD_FLAGS", [])
if isinstance(flags, str):
    flags = [flags]
fixed = []
for flag in flags:
    if "BOARD_NAME=" in flag:
        flag = flag.replace("${PIOENV}", base_env).replace("-https", "")
    fixed.append(flag)
env.Replace(BUILD_FLAGS=fixed)
print("https_env.py: BOARD_NAME now", [f for f in fixed if "BOARD_NAME=" in f])
