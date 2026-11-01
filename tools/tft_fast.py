# PlatformIO pre-script (cyd-s3): TFT_eSPI's start for the ILI9341 without its two fixed waits, 270 ms of every start:
#   - no software reset, and 5 ms instead of 150 after it: the init sequence sets every register anyway, and the
#     panel's power-on reset is long done (its 120 ms before Sleep Out pass while the chip boots)
#   - 10 ms instead of 120 after Sleep Out: the datasheet asks 5 before the next command, and the backlight stays off
#     until the app's frame is drawn (ui::frame), so the panel settling isn't seen
# For TFT_eSPI 2.5.43 (platformio.ini): a line that isn't there as expected stops the build.
import os

Import("env")

MARK = "fm1buddy: tools/tft_fast.py"
PATCHES = [
    ("TFT_eSPI.cpp", "  else writecommand(TFT_SWRST); // Software reset\n", "  // (" + MARK + ": no software reset)\n"),
    ("TFT_eSPI.cpp", "  delay(150); // Wait for reset to complete\n", "  delay(5); // " + MARK + "\n"),
    (
        "TFT_Drivers/ILI9341_Init.h",
        "writecommand(ILI9341_SLPOUT); //Exit Sleep\n\nend_tft_write();\ndelay(120);\n",
        "writecommand(ILI9341_SLPOUT); //Exit Sleep\n\nend_tft_write();\ndelay(10); // " + MARK + "\n",
    ),
]

root = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "TFT_eSPI")
for name, old, new in PATCHES:
    path = os.path.join(root, name)
    with open(path) as f:
        text = f.read()
    if new in text:
        continue
    if text.count(old) != 1:
        raise SystemExit("tft_fast.py: %s isn't as expected (another TFT_eSPI?)" % path)
    with open(path, "w") as f:
        f.write(text.replace(old, new))
    print("tft_fast.py: patched " + name)
