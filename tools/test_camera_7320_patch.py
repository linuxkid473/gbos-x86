#!/usr/bin/env python3
"""Test the camera source patch against an existing kernel source tree."""
import difflib
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = pathlib.Path(sys.argv[1])
FILES = (
    "drivers/platform/x86/intel/int3472/tps68470_board_data.c",
    "drivers/platform/x86/intel/int3472/tps68470.h",
    "drivers/platform/x86/intel/int3472/tps68470.c",
    "drivers/media/i2c/ov5675.c",
    "drivers/media/i2c/ov8856.c",
    "drivers/media/pci/intel/ipu-bridge.c",
)

with tempfile.TemporaryDirectory() as tmp:
    tree = pathlib.Path(tmp) / "src"
    for name in FILES:
        target = tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SOURCE / name, target)

    patch = ROOT / "kernel/isa_emu_patch/camera_7320.py"
    subprocess.run([sys.executable, patch, tree], check=True)
    once = [(tree / name).read_bytes() for name in FILES]
    subprocess.run([sys.executable, patch, tree], check=True)
    for name, first, second in zip(FILES, once, ((tree / name).read_bytes() for name in FILES)):
        if first != second:
            diff = "".join(difflib.unified_diff(
                first.decode().splitlines(True),
                second.decode().splitlines(True),
                fromfile="after first application",
                tofile="after second application",
            ))
            raise AssertionError(f"camera patch is not idempotent for {name}:\n{diff}")

    board = (tree / FILES[0]).read_text()
    board_header = (tree / FILES[1]).read_text()
    board_driver = (tree / FILES[2]).read_text()
    assert "struct software_node;" in board_header
    assert "const struct software_node *tps68470_gpio_swnode;" in board_header
    assert "cells[2].swnode = board_data->tps68470_gpio_swnode;" in board_driver
    for mapping in (
        'REGULATOR_SUPPLY("dvdd", "i2c-OVTI8856:00")',
        'REGULATOR_SUPPLY("avdd", "i2c-OVTI8856:00")',
        'REGULATOR_SUPPLY("dovdd", "i2c-OVTI8856:00")',
        'REGULATOR_SUPPLY("vcc", "i2c-OVTI8856:00-VCM")',
        'GPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW)',
        'PROPERTY_ENTRY_BOOL("daisy-chain-enable")',
    ):
        assert mapping in board
    assert '.dev_name = "i2c-INT3472:07"' in board
    assert '.tps68470_gpio_swnode = &dell_7320_detachable_gpio_swnode' in board
    assert '&dell_7320_detachable_ovti8856_gpios' in board
    assert 'DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dell Inc.")' in board
    assert 'DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Latitude 7320 Detachable")' in board

    for sensor, name in (("ov5675", "i2c-OVTI5678:00"),
                         ("ov8856", "i2c-OVTI8856:00")):
        source = (tree / f"drivers/media/i2c/{sensor}.c").read_text()
        start = source.index("\t/* gbos: wait for IPU bridge endpoint")
        end = source.index(f"\t{sensor}->xvclk =", start)
        guard = source[start:end]
        assert source.index("if (!fwnode)", source.index(f"static int {sensor}_get_hwcfg")) < start
        assert f'"{name}"' in guard
        assert source.count("#include <linux/dmi.h>") == 1
        assert source.count("#include <linux/string.h>") == 1
        if sensor == "ov8856":
            diagnostic = source[source.index(
                "ret = i2c_transfer(client->adapter"):
                source.index("*val = get_unaligned_be32")]
            assert "reg == OV8856_REG_CHIP_ID" in diagnostic
            assert 'is_acpi_node(dev_fwnode(&client->dev))' in diagnostic
            assert 'dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable")' in diagnostic
            assert '!strcmp(dev_name(&client->dev), "i2c-OVTI8856:00")' in diagnostic
            assert "reg, ret, ARRAY_SIZE(msgs), client->adapter->nr" in diagnostic
            assert "return -EIO;" in diagnostic
            assert "struct acpi_device *adev = ACPI_COMPANION(ov8856->dev);" in source
            assert "acpi_device_power_manageable(adev)" in source
            assert 'power-manageable=%d' in source
            helper_start = source.index(
                "static bool ov8856_is_gbos_7320_device")
            helper_end = source.index("\n}\n", helper_start) + 3
            helper = source[helper_start:helper_end]
            assert 'dmi_match(DMI_SYS_VENDOR, "Dell Inc.")' in helper
            assert 'dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable")' in helper
            assert '!strcmp(dev_name(dev), "i2c-OVTI8856:00")' in helper

            power_start = source.index("static int ov8856_power_on")
            power_end = source.index("static int ov8856_power_off", power_start)
            power_on = source[power_start:power_end]
            power_guard_start = power_on.index(
                "\tif (is_acpi_node(dev_fwnode(dev))")
            power_guard_end = power_on.index(
                "\n\n\tret = clk_prepare_enable", power_guard_start)
            power_guard = power_on[power_guard_start:power_guard_end]
            assert "&&\n\t    !ov8856_is_gbos_7320_device(dev)" in power_guard

            power_off_start = source.index("static int ov8856_power_off")
            power_off_end = source.index("static int ov8856_get_hwcfg", power_off_start)
            power_off = source[power_off_start:power_off_end]
            assert "&&\n\t    !ov8856_is_gbos_7320_device(dev)" in power_off

            hwcfg_start = source.index("static int ov8856_get_hwcfg")
            hwcfg_end = source.index("static int ov8856_", hwcfg_start + 1)
            hwcfg = source[hwcfg_start:hwcfg_end]
            assert ("if (!is_acpi_node(fwnode) || "
                    "ov8856_is_gbos_7320_device(dev)) {") in hwcfg
            assert "devm_gpiod_get_optional(dev, \"reset\"" in hwcfg
            assert "devm_regulator_bulk_get(dev" in hwcfg
        harness = r"""
#include <string.h>
#define DMI_SYS_VENDOR 1
#define DMI_PRODUCT_NAME 2
#define EPROBE_DEFER 517
struct device { const char *name; };
struct fwnode_handle { int unused; };
static int vendor_ok, product_ok, endpoint_ok, puts;
static struct fwnode_handle endpoint;
static int dmi_match(int field, const char *value) {
    (void)value;
    return field == DMI_SYS_VENDOR ? vendor_ok : product_ok;
}
static const char *dev_name(struct device *dev) { return dev->name; }
static struct fwnode_handle *fwnode_graph_get_next_endpoint(
    struct fwnode_handle *fwnode, struct fwnode_handle *prev) {
    (void)fwnode; (void)prev;
    return endpoint_ok ? &endpoint : NULL;
}
static void fwnode_handle_put(struct fwnode_handle *node) {
    if (node == &endpoint) puts++;
}
static int dev_err_probe(struct device *dev, int error, const char *message) {
    (void)dev; (void)message; return error;
}
static int probe(struct device *dev, struct fwnode_handle *fwnode) {
    struct fwnode_handle *ep;
""" + guard + r"""
    return 0;
}
int main(void) {
    struct device dev = { TARGET };
    struct fwnode_handle fwnode;
    vendor_ok = product_ok = 1;
    if (probe(&dev, &fwnode) != -EPROBE_DEFER || puts != 0) return 1;
    endpoint_ok = 1;
    if (probe(&dev, &fwnode) != 0 || puts != 1) return 2;
    endpoint_ok = 0;
    vendor_ok = 0;
    if (probe(&dev, &fwnode) != 0 || puts != 1) return 3;
    vendor_ok = 1; product_ok = 0;
    if (probe(&dev, &fwnode) != 0 || puts != 1) return 4;
    product_ok = 1; dev.name = "i2c-other";
    if (probe(&dev, &fwnode) != 0 || puts != 1) return 5;
    return 0;
}
""".replace("TARGET", f'"{name}"')
        cfile = pathlib.Path(tmp) / f"{sensor}-test.c"
        binary = pathlib.Path(tmp) / f"{sensor}-test"
        cfile.write_text(harness)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(cfile),
                        "-o", str(binary)], check=True)
        subprocess.run([binary], check=True)

    helper_harness = r"""
#include <stdbool.h>
#include <string.h>
#define DMI_SYS_VENDOR 1
#define DMI_PRODUCT_NAME 2
struct device { const char *name; int acpi; };
static int vendor_ok, product_ok;
static int dmi_match(int field, const char *value) {
    if (field == DMI_SYS_VENDOR)
        return vendor_ok && strcmp(value, "Dell Inc.") == 0;
    return product_ok && strcmp(value, "Latitude 7320 Detachable") == 0;
}
static void *dev_fwnode(struct device *dev) { return dev; }
static int is_acpi_node(void *node) {
    return ((struct device *)node)->acpi;
}
static const char *dev_name(struct device *dev) { return dev->name; }
""" + helper + r"""
static int should_power_on(struct device *dev) {
""" + power_guard + r"""
    return 1;
}
int main(void) {
    struct device dev = { .name = "i2c-OVTI8856:00", .acpi = 1 };
    vendor_ok = product_ok = 1;
    if (should_power_on(&dev) != 1) return 1;
    product_ok = 0;
    if (should_power_on(&dev) != 0) return 2;
    product_ok = 1; vendor_ok = 0;
    if (should_power_on(&dev) != 0) return 3;
    vendor_ok = 1; dev.name = "i2c-other";
    if (should_power_on(&dev) != 0) return 4;
    dev.name = "i2c-OVTI8856:00"; dev.acpi = 0;
    if (should_power_on(&dev) != 1) return 5;
    return 0;
}
"""
    cfile = pathlib.Path(tmp) / "ov8856-acpi-power-test.c"
    binary = pathlib.Path(tmp) / "ov8856-acpi-power-test"
    cfile.write_text(helper_harness)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(cfile),
                    "-o", str(binary)], check=True)
    subprocess.run([binary], check=True)

    read_block = diagnostic[diagnostic.index(
        "\tif (ret != ARRAY_SIZE(msgs)) {"):]
    harness = r"""
#include <stddef.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DMI_SYS_VENDOR 1
#define DMI_PRODUCT_NAME 2
#define EIO 5
#define OV8856_REG_CHIP_ID 0x300a
typedef unsigned short u16;
struct device { const char *name; };
struct i2c_adapter { int nr; };
struct i2c_client {
    struct device dev;
    struct i2c_adapter *adapter;
    unsigned short addr;
};
struct i2c_msg { int unused; };
static int vendor_ok, product_ok, acpi_ok, log_count;
static int dmi_match(int field, const char *value) {
    (void)value;
    return field == DMI_SYS_VENDOR ? vendor_ok : product_ok;
}
static void *dev_fwnode(struct device *dev) { return dev; }
static int is_acpi_node(void *node) { (void)node; return acpi_ok; }
static const char *dev_name(struct device *dev) { return dev->name; }
#define dev_err fake_dev_err
static int fake_dev_err(struct device *dev, const char *format, ...) {
    (void)dev; (void)format;
    log_count++;
    return 0;
}
static int read_chip_id(int result, u16 reg, const char *name) {
    struct i2c_adapter adapter = { .nr = 5 };
    struct i2c_client client_data = {
        .dev = { .name = name }, .adapter = &adapter, .addr = 0x10,
    };
    struct i2c_client *client = &client_data;
    struct i2c_msg msgs[2];
    int ret = result;
""" + read_block + r"""
    return 0;
}
int main(void) {
    vendor_ok = product_ok = acpi_ok = 1;
    if (read_chip_id(1, OV8856_REG_CHIP_ID, "i2c-OVTI8856:00") != -EIO ||
        log_count != 1) return 1;
    log_count = 0;
    product_ok = 0;
    if (read_chip_id(1, OV8856_REG_CHIP_ID, "i2c-OVTI8856:00") != -EIO ||
        log_count != 0) return 2;
    product_ok = 1;
    acpi_ok = 0;
    if (read_chip_id(1, OV8856_REG_CHIP_ID, "i2c-OVTI8856:00") != -EIO ||
        log_count != 0) return 3;
    acpi_ok = 1;
    if (read_chip_id(1, 0x0100, "i2c-OVTI8856:00") != -EIO ||
        log_count != 0) return 4;
    if (read_chip_id(2, OV8856_REG_CHIP_ID, "i2c-OVTI8856:00") != 0 ||
        log_count != 0) return 5;
    return 0;
}
"""
    cfile = pathlib.Path(tmp) / "ov8856-diagnostic-test.c"
    binary = pathlib.Path(tmp) / "ov8856-diagnostic-test"
    cfile.write_text(harness)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True)

print("CAMERA_7320_PATCH_TEST_PASS")
