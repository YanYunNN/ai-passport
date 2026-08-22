# ESP32-C3 Firmware Release Guide

Use this document whenever a firmware build is prepared for distribution or flashing with an official tool.

The current board target is **ESP32-C3 with 8 MB Flash**. A release uses one raw merged image containing the bootloader, partition table, and factory application. The image is flashed once at offset `0x0`.

## Release requirements

- ESP-IDF **v5.5.3**.
- CMake available on `PATH`.
- A clean, reviewed source revision. Do not publish a firmware binary produced from unrelated or uncommitted work.
- A network connection for the first ESP-IDF/Component Manager build.
- A physical FoloToy AI Passport for device acceptance; a successful build is not hardware validation.

The known macOS installation used by this project is:

```sh
export IDF_PATH="$HOME/esp/esp-idf-v5.5.3"
source "$IDF_PATH/export.sh"
```

For a new installation, obtain ESP-IDF v5.5.3 and install only the required target tools:

```sh
git clone --branch v5.5.3 --depth 1 https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf-v5.5.3"
cd "$HOME/esp/esp-idf-v5.5.3"
./install.sh esp32c3
```

## 1. Prepare the release revision

Run every command below from the repository root.

```sh
git status --short
source "$HOME/esp/esp-idf-v5.5.3/export.sh"
idf.py set-target esp32c3
```

`git status --short` should be empty before publishing an official artifact. `set-target` is needed for a fresh checkout or after building another target. If generated configuration is stale, use `idf.py fullclean` before setting the target again; it deletes only generated build state.

## 2. Run release checks

Run the independent pure-C smoke test and the firmware build:

```sh
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_ui_pixel_math.c main/ui_pixel_math.c \
  -o /tmp/test_ui_pixel_math
/tmp/test_ui_pixel_math

idf.py build
```

A successful build produces these component images:

```text
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/FoloToy-AI-Passport.bin
```

The factory partition starts at `0x10000` and is 1 MB. Confirm that the build reports the application fits this partition.

## 3. Create the one-file release image

Create a raw merged image with the board's fixed Flash geometry. The output below includes all required image segments and is ready to flash at `0x0`:

```sh
FIRMWARE="build/FoloToy-AI-Passport-esp32c3-8mb.bin"

python -m esptool --chip esp32c3 merge_bin \
  --output "$FIRMWARE" \
  --format raw \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin
```

Expected merged layout:

| Offset | Content |
| ---: | --- |
| `0x0` | Bootloader |
| `0x8000` | Partition table |
| `0x10000` | Factory application |

Use the explicit `esptool merge_bin` command above for releases. It specifies `8MB` rather than `detect`, so it also works with newer esptool versions that reject `detect` during image merging.

## 4. Verify and publish the artifact

Record the SHA-256 checksum next to the published binary. Recipients must verify it before flashing.

```sh
shasum -a 256 "$FIRMWARE"
ls -lh "$FIRMWARE"
```

A release record should include:

```text
Release/tag:
Git commit:
ESP-IDF: v5.5.3
Target: esp32c3
Flash: 8 MB, DIO, 80 MHz
Artifact: FoloToy-AI-Passport-esp32c3-8mb.bin
SHA-256:
Host smoke test: PASS / FAIL
idf.py build: PASS / FAIL
Device tests: PASS / FAIL / NOT RUN
```

`build/` is generated output and should normally be attached to a release rather than committed to source control. Preserve `dependencies.lock` when it exists so Component Manager resolves the same dependency versions for later builds.

## 5. Flash the single BIN

### Official flashing GUI

If the official tool supports one BIN, select:

| Field | Value |
| --- | --- |
| Chip | ESP32-C3 |
| BIN file | `build/FoloToy-AI-Passport-esp32c3-8mb.bin` |
| Flash offset | `0x0` |
| Flash size | 8 MB |
| Flash mode | DIO |
| Flash frequency | 80 MHz |

Do not also add the individual bootloader, partition table, or application BIN files when flashing this merged image: they are already embedded in it.

### ESP-IDF / command-line flashing

Connect the board by USB, identify its macOS serial device, then flash the same merged image:

```sh
ls /dev/cu.usb*

python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 "$FIRMWARE"
```

The board uses native USB Serial/JTAG. If the flasher cannot connect, hold **BOOT**, tap **RESET**, release **BOOT**, then retry.

## 6. Device acceptance before release

After flashing, use the serial monitor and perform the checks that apply to the release:

```sh
idf.py -p /dev/cu.usbmodemXXXX monitor
```

- Startup logs are stable; there is no reboot loop, assertion, or watchdog reset.
- The display has correct orientation, colors, backlight, and refresh behavior.
- `UP`, `DOWN`, short `OK`, and long `OK` follow the expected menu behavior.
- Reader opens all three bundled books, including the Chinese `青岚问道` sample; it changes pages, returns to its library on short `OK`, and returns to the main menu on long `OK`.
- Audio and battery pages behave correctly when their hardware is present; the menu marks optional unavailable peripherals as failed without blocking other pages.
- Repeated page transitions do not visibly leak objects, tasks, or heap.

Record actual hardware observations separately from the build result. A release is not device-validated until these checks pass on the intended board revision.
