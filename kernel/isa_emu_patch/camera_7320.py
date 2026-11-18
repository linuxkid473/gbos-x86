#!/usr/bin/env python3
"""Add Dell Latitude 7320 Detachable camera support to the kernel source.

The board data and front-sensor wiring are adapted from the GPL-2.0 v2 patch
by Sahan Nissanka and Charles Drolet:
https://github.com/githomeserver/latitude-7320-camera/blob/af110bc4c54ccf031508becac77e04b4dfc6c680/kernel-patches/v2-0001-platform-x86-int3472-Add-TPS68470-board-data-for-Dell-7320-Detachable.patch

The rear OV8856 power, VCM, and reset settings are an experimental adaptation of
a separate Latitude 7320 report, not verified on this target:
https://github.com/githomeserver/latitude-7320-camera/issues/1
"""
import pathlib
import sys


def replace_function_once(path, function, next_function, original, replacement, tag):
    source = path.read_text()
    start = source.index(function)
    end = source.index(next_function, start + len(function))
    body = source[start:end]
    if replacement in body:
        return
    if body.count(original) != 1:
        raise SystemExit(
            "camera patch: expected one replacement point for "
            + tag
            + " in "
            + str(path)
        )
    path.write_text(source[:start] + body.replace(original, replacement, 1) + source[end:])


ROOT = pathlib.Path(sys.argv[1])


def insert_once(path, marker, content, tag):
    source = path.read_text()
    if tag in source:
        return
    if source.count(marker) != 1:
        raise SystemExit(
            "camera patch: expected one insertion point for "
            + tag
            + " in "
            + str(path)
        )
    path.write_text(source.replace(marker, content + marker, 1))


def replace_section(path, start_marker, end_marker, replacement, tag):
    source = path.read_text()
    replacement = replacement.rstrip("\n") + "\n\n"
    if start_marker in source:
        if source.count(start_marker) != 1:
            raise SystemExit(
                "camera patch: expected one existing section for "
                + tag
                + " in "
                + str(path)
            )
        start = source.index(start_marker)
        end = source.index(end_marker, start + len(start_marker))
        if source[start:end] == replacement:
            return
        path.write_text(source[:start] + replacement + source[end:])
        return
    if tag in source:
        return
    if source.count(end_marker) != 1:
        raise SystemExit(
            "camera patch: expected one insertion point for "
            + tag
            + " in "
            + str(path)
        )
    path.write_text(source.replace(end_marker, replacement + end_marker, 1))


def replace_once(path, original, replacement, tag):
    source = path.read_text()
    if tag in source:
        return
    if source.count(original) != 1:
        raise SystemExit(
            "camera patch: expected one replacement point for "
            + tag
            + " in "
            + str(path)
        )
    path.write_text(source.replace(original, replacement, 1))


platform_dir = ROOT / "drivers/platform/x86/intel/int3472"
board_data = platform_dir / "tps68470_board_data.c"
tps68470_h = platform_dir / "tps68470.h"
tps68470_c = platform_dir / "tps68470.c"
insert_once(
    board_data,
    "#include <linux/platform_data/tps68470.h>\n",
    "#include <linux/property.h>\n",
    "#include <linux/property.h>",
)
replace_section(
    board_data,
    "/* Dell Latitude 7320 Detachable:",
    "static struct gpiod_lookup_table surface_go_int347a_gpios = {",
    """/* Dell Latitude 7320 Detachable: front wiring and experimental rear OV8856 mapping. */
static struct regulator_consumer_supply dell_7320_detachable_vsio_consumer_supplies[] = {
\tREGULATOR_SUPPLY("avdd", "i2c-OVTI5678:00"),
\tREGULATOR_SUPPLY("dovdd", "i2c-OVTI8856:00"),
};

static struct regulator_consumer_supply dell_7320_detachable_aux1_consumer_supplies[] = {
\tREGULATOR_SUPPLY("dvdd", "i2c-OVTI5678:00"),
};

static struct regulator_consumer_supply dell_7320_detachable_aux2_consumer_supplies[] = {
\tREGULATOR_SUPPLY("dovdd", "i2c-OVTI5678:00"),
};

static struct regulator_consumer_supply dell_7320_detachable_ovti8856_core_consumer_supplies[] = {
\tREGULATOR_SUPPLY("dvdd", "i2c-OVTI8856:00"),
};

static struct regulator_consumer_supply dell_7320_detachable_ovti8856_ana_consumer_supplies[] = {
\tREGULATOR_SUPPLY("avdd", "i2c-OVTI8856:00"),
};

static struct regulator_consumer_supply dell_7320_detachable_ovti8856_vcm_consumer_supplies[] = {
\tREGULATOR_SUPPLY("vcc", "i2c-OVTI8856:00-VCM"),
};

static const struct regulator_init_data dell_7320_detachable_core_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 1200000,
\t\t.max_uV = 1200000,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_ovti8856_core_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_ovti8856_core_consumer_supplies,
};

static const struct regulator_init_data dell_7320_detachable_ana_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 2815200,
\t\t.max_uV = 2815200,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_ovti8856_ana_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_ovti8856_ana_consumer_supplies,
};

static const struct regulator_init_data dell_7320_detachable_vcm_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 2815200,
\t\t.max_uV = 2815200,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_ovti8856_vcm_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_ovti8856_vcm_consumer_supplies,
};

static const struct regulator_init_data dell_7320_detachable_vio_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 1800600,
\t\t.max_uV = 1800600,
\t\t.apply_uV = true,
\t\t.always_on = true,
\t},
};

static const struct regulator_init_data dell_7320_detachable_vsio_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 1800600,
\t\t.max_uV = 1800600,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_vsio_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_vsio_consumer_supplies,
};

static const struct regulator_init_data dell_7320_detachable_aux1_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 1213200,
\t\t.max_uV = 1213200,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_aux1_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_aux1_consumer_supplies,
};

static const struct regulator_init_data dell_7320_detachable_aux2_reg_init_data = {
\t.constraints = {
\t\t.min_uV = 1800600,
\t\t.max_uV = 1800600,
\t\t.apply_uV = true,
\t\t.valid_ops_mask = REGULATOR_CHANGE_STATUS,
\t},
\t.num_consumer_supplies = ARRAY_SIZE(dell_7320_detachable_aux2_consumer_supplies),
\t.consumer_supplies = dell_7320_detachable_aux2_consumer_supplies,
};

static const struct tps68470_regulator_platform_data dell_7320_detachable_tps68470_pdata = {
\t.reg_init_data = {
\t\t[TPS68470_CORE] = &dell_7320_detachable_core_reg_init_data,
\t\t[TPS68470_ANA] = &dell_7320_detachable_ana_reg_init_data,
\t\t[TPS68470_VCM] = &dell_7320_detachable_vcm_reg_init_data,
\t\t[TPS68470_VIO] = &dell_7320_detachable_vio_reg_init_data,
\t\t[TPS68470_VSIO] = &dell_7320_detachable_vsio_reg_init_data,
\t\t[TPS68470_AUX1] = &dell_7320_detachable_aux1_reg_init_data,
\t\t[TPS68470_AUX2] = &dell_7320_detachable_aux2_reg_init_data,
\t},
};

""",
    "dell_7320_detachable_vsio_consumer_supplies",
)

replace_section(
    board_data,
    "static struct gpiod_lookup_table dell_7320_detachable_gpios = {",
    "static const struct int3472_tps68470_board_data surface_go_tps68470_board_data = {",
    """static struct gpiod_lookup_table dell_7320_detachable_ovti5678_gpios = {
\t.dev_id = "i2c-OVTI5678:00",
\t.table = {
\t\tGPIO_LOOKUP("tps68470-gpio", 5, "reset", GPIO_ACTIVE_LOW),
\t\t{ }
\t}
};

static struct gpiod_lookup_table dell_7320_detachable_ovti8856_gpios = {
\t.dev_id = "i2c-OVTI8856:00",
\t.table = {
\t\tGPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW),
\t\t{ }
\t}
};

static const struct property_entry dell_7320_detachable_gpio_props[] = {
\tPROPERTY_ENTRY_BOOL("daisy-chain-enable"),
\t{ }
};

static const struct software_node dell_7320_detachable_gpio_swnode = {
\t.properties = dell_7320_detachable_gpio_props,
};

""",
    "dell_7320_detachable_ovti5678_gpios",
)

replace_section(
    board_data,
    "static const struct int3472_tps68470_board_data dell_7320_detachable_tps68470_board_data = {",
    "static const struct dmi_system_id int3472_tps68470_board_data_table[] = {",
    """static const struct int3472_tps68470_board_data dell_7320_detachable_tps68470_board_data = {
\t.dev_name = "i2c-INT3472:07",
\t.tps68470_regulator_pdata = &dell_7320_detachable_tps68470_pdata,
\t.tps68470_gpio_swnode = &dell_7320_detachable_gpio_swnode,
\t.n_gpiod_lookups = 2,
\t.tps68470_gpio_lookup_tables = {
\t\t&dell_7320_detachable_ovti5678_gpios,
\t\t&dell_7320_detachable_ovti8856_gpios,
\t},
};

""",
    "dell_7320_detachable_tps68470_board_data",
)

dmi_end = """\t\t.driver_data = (void *)&dell_7212_tps68470_board_data,
\t},
\t{ }
};"""
dmi_entry = """\t\t.driver_data = (void *)&dell_7212_tps68470_board_data,
\t},
\t{
\t\t.matches = {
\t\t\tDMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
\t\t\tDMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Latitude 7320 Detachable"),
\t\t},
\t\t.driver_data = (void *)&dell_7320_detachable_tps68470_board_data,
\t},
\t{ }
};"""
source = board_data.read_text()
if dmi_entry not in source:
    if source.count(dmi_end) != 1:
        raise SystemExit("camera patch: expected one Dell DMI table ending")
    board_data.write_text(source.replace(dmi_end, dmi_entry, 1))

ov5675 = ROOT / "drivers/media/i2c/ov5675.c"
insert_once(
    ov5675,
    '\t{"OVTI5675"},\n',
    '\t{"OVTI5678"},\n',
    '\t{"OVTI5678"}',
)
replace_once(
    ov5675,
    """\tov5675 = devm_kzalloc(&client->dev, sizeof(*ov5675), GFP_KERNEL);
\tif (!ov5675)
\t\treturn -ENOMEM;
""",
    """\tdev_info(&client->dev, "probe invoked\\n");
\tov5675 = devm_kzalloc(&client->dev, sizeof(*ov5675), GFP_KERNEL);
\tif (!ov5675)
\t\treturn dev_err_probe(&client->dev, -ENOMEM,
\t\t\t\t     "failed to allocate sensor state\\n");
""",
    "probe invoked",
)
replace_once(
    ov5675,
    """\tif (!fwnode)
\t\treturn -ENXIO;
""",
    """\tif (!fwnode)
\t\treturn dev_err_probe(dev, -ENXIO, "missing firmware node\\n");
""",
    "missing firmware node",
)
replace_once(
    ov5675,
    """\tret = devm_regulator_bulk_get(dev, OV5675_NUM_SUPPLIES,
\t\t\t\t      ov5675->supplies);
\tif (ret)
\t\treturn ret;
""",
    """\tret = devm_regulator_bulk_get(dev, OV5675_NUM_SUPPLIES,
\t\t\t\t      ov5675->supplies);
\tif (ret)
\t\treturn dev_err_probe(dev, ret, "failed to get regulators\\n");
""",
    "failed to get regulators",
)
replace_once(
    ov5675,
    """\tep = fwnode_graph_get_next_endpoint(fwnode, NULL);
\tif (!ep)
\t\treturn -ENXIO;
""",
    """\tep = fwnode_graph_get_next_endpoint(fwnode, NULL);
\tif (!ep)
\t\treturn dev_err_probe(dev, -ENXIO,
\t\t\t\t     "missing CSI-2 firmware endpoint\\n");
""",
    "missing CSI-2 firmware endpoint",
)
replace_once(
    ov5675,
    """\tret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
\tfwnode_handle_put(ep);
\tif (ret)
\t\treturn ret;
""",
    """\tret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
\tfwnode_handle_put(ep);
\tif (ret)
\t\treturn dev_err_probe(dev, ret,
\t\t\t\t     "failed to parse CSI-2 firmware endpoint\\n");
""",
    "failed to parse CSI-2 firmware endpoint",
)

# The Windows driver for this RGB-IR module (ACPI OVTI5678, "OV5678", module 0BF501T3) uses
# Intel's OV5675 mode table but writes 0x5000 = 0x71 (Linux 0x77) and 0x3d8c/0x3d8d =
# 0x71/0xe7 (Linux: not written); see docs/ov5678-windows-vs-linux.md. What these bits do is
# not established, so the Windows values are opt-in (module parameter, writable at runtime;
# read at stream start) and limited to the Latitude 7320's front sensor. The values in effect
# are read back and logged at every stream start.
insert_once(
    ov5675,
    "static int ov5675_start_streaming(struct ov5675 *ov5675)\n",
    """/* gbos: Windows driver values for the Latitude 7320 Detachable's RGB-IR OV5678 module */
static bool gbos_windows_isp;
module_param(gbos_windows_isp, bool, 0644);
MODULE_PARM_DESC(gbos_windows_isp,
\t\t "Latitude 7320 OVTI5678 only: write the Windows driver's 0x5000/0x3d8c/0x3d8d values at stream start");

static const struct ov5675_reg gbos_ov5678_windows_regs[] = {
\t{0x5000, 0x71},
\t{0x3d8c, 0x71},
\t{0x3d8d, 0xe7},
};

static bool ov5675_is_gbos_7320_front(struct device *dev)
{
\treturn dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
\t       dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable") &&
\t       !strcmp(dev_name(dev), "i2c-OVTI5678:00");
}

static int ov5675_gbos_7320_regs(struct ov5675 *ov5675)
{
\tconst struct ov5675_reg_list list = {
\t\t.num_of_regs = ARRAY_SIZE(gbos_ov5678_windows_regs),
\t\t.regs = gbos_ov5678_windows_regs,
\t};
\tbool windows = READ_ONCE(gbos_windows_isp);
\tu32 isp, otp_hi, otp_lo;
\tint ret;

\tif (!ov5675_is_gbos_7320_front(ov5675->dev))
\t\treturn 0;
\tif (windows) {
\t\tret = ov5675_write_reg_list(ov5675, &list);
\t\tif (ret)
\t\t\treturn ret;
\t}
\tif (!ov5675_read_reg(ov5675, 0x5000, OV5675_REG_VALUE_08BIT, &isp) &&
\t    !ov5675_read_reg(ov5675, 0x3d8c, OV5675_REG_VALUE_08BIT, &otp_hi) &&
\t    !ov5675_read_reg(ov5675, 0x3d8d, OV5675_REG_VALUE_08BIT, &otp_lo))
\t\tdev_info(ov5675->dev,
\t\t\t "gbos: Windows ISP values %s: 0x5000=0x%02x 0x3d8c=0x%02x 0x3d8d=0x%02x\\n",
\t\t\t windows ? "on" : "off", isp, otp_hi, otp_lo);
\treturn 0;
}

""",
    "gbos_windows_isp",
)
replace_function_once(
    ov5675,
    "static int ov5675_start_streaming(struct ov5675 *ov5675)",
    "static void ov5675_stop_streaming(struct ov5675 *ov5675)",
    """\t\tdev_err(ov5675->dev, "failed to set mode");
\t\treturn ret;
\t}
""",
    """\t\tdev_err(ov5675->dev, "failed to set mode");
\t\treturn ret;
\t}

\tret = ov5675_gbos_7320_regs(ov5675);
\tif (ret) {
\t\tdev_err(ov5675->dev, "gbos: failed to set Windows ISP values");
\t\treturn ret;
\t}
""",
    "gbos 7320 Windows ISP values",
)

ov8856 = ROOT / "drivers/media/i2c/ov8856.c"
insert_once(
    ov8856,
    "static int ov8856_power_on(struct device *dev)\n",
    """/* Keep ACPI power sequencing limited to this reported 7320 device. */
static bool ov8856_is_gbos_7320_device(struct device *dev)
{
\treturn dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
\t       dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable") &&
\t       !strcmp(dev_name(dev), "i2c-OVTI8856:00");
}

""",
    "ov8856_is_gbos_7320_device",
)
replace_function_once(
    ov8856,
    "static int ov8856_power_on(struct device *dev)",
    "static int ov8856_power_off(struct device *dev)",
    """\tif (is_acpi_node(dev_fwnode(dev)))
\t\treturn 0;""",
    """\tif (is_acpi_node(dev_fwnode(dev)) &&
\t    !ov8856_is_gbos_7320_device(dev))
\t\treturn 0;

""",
    "gbos 7320 OV8856 ACPI power on",
)
replace_function_once(
    ov8856,
    "static int ov8856_power_off(struct device *dev)",
    "static int ov8856_set_format(",
    """\tif (is_acpi_node(dev_fwnode(dev)))
\t\treturn 0;""",
    """\tif (is_acpi_node(dev_fwnode(dev)) &&
\t    !ov8856_is_gbos_7320_device(dev))
\t\treturn 0;""",
    "gbos 7320 OV8856 ACPI power off",
)
replace_function_once(
    ov8856,
    "static int ov8856_get_hwcfg(",
    "static int ov8856_",
    "\tif (!is_acpi_node(fwnode)) {",
    "\tif (!is_acpi_node(fwnode) || ov8856_is_gbos_7320_device(dev)) {",
    "gbos 7320 OV8856 ACPI resources",
)
replace_once(
    ov8856,
    """\tov8856 = devm_kzalloc(&client->dev, sizeof(*ov8856), GFP_KERNEL);
\tif (!ov8856)
\t\treturn -ENOMEM;
""",
    """\tdev_info(&client->dev, "probe invoked\\n");
\tov8856 = devm_kzalloc(&client->dev, sizeof(*ov8856), GFP_KERNEL);
\tif (!ov8856)
\t\treturn dev_err_probe(&client->dev, -ENOMEM,
\t\t\t\t     "failed to allocate sensor state\\n");
""",
    "probe invoked",
)
replace_once(
    ov8856,
    """\tif (!fwnode)
\t\treturn -ENXIO;
""",
    """\tif (!fwnode)
\t\treturn dev_err_probe(dev, -ENXIO, "missing firmware node\\n");
""",
    "missing firmware node",
)
replace_once(
    ov8856,
    """\t\tif (IS_ERR(ov8856->reset_gpio))
\t\t\treturn PTR_ERR(ov8856->reset_gpio);
""",
    """\t\tif (IS_ERR(ov8856->reset_gpio))
\t\t\treturn dev_err_probe(dev, PTR_ERR(ov8856->reset_gpio),
\t\t\t\t\t     "failed to get reset GPIO\\n");
""",
    "failed to get reset GPIO",
)
replace_once(
    ov8856,
    """\t\tret = devm_regulator_bulk_get(dev,
\t\t\t\t\t      ARRAY_SIZE(ov8856_supply_names),
\t\t\t\t\t      ov8856->supplies);
\t\tif (ret)
\t\t\treturn ret;
""",
    """\t\tret = devm_regulator_bulk_get(dev,
\t\t\t\t\t      ARRAY_SIZE(ov8856_supply_names),
\t\t\t\t\t      ov8856->supplies);
\t\tif (ret)
\t\t\treturn dev_err_probe(dev, ret, "failed to get regulators\\n");
""",
    "failed to get regulators",
)
replace_once(
    ov8856,
    """\tep = fwnode_graph_get_next_endpoint(fwnode, NULL);
\tif (!ep)
\t\treturn -ENXIO;
""",
    """\tep = fwnode_graph_get_next_endpoint(fwnode, NULL);
\tif (!ep)
\t\treturn dev_err_probe(dev, -ENXIO,
\t\t\t\t     "missing CSI-2 firmware endpoint\\n");
""",
    "missing CSI-2 firmware endpoint",
)
replace_once(
    ov8856,
    """\tret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
\tfwnode_handle_put(ep);
\tif (ret)
\t\treturn ret;
""",
    """\tret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
\tfwnode_handle_put(ep);
\tif (ret)
\t\treturn dev_err_probe(dev, ret,
\t\t\t\t     "failed to parse CSI-2 firmware endpoint\\n");
""",
    "failed to parse CSI-2 firmware endpoint",
)

# The IPU bridge supplies the endpoint after the sensor drivers may already
# have probed. Defer before acquiring any sensor resources on this board.
for sensor_path, device_name in (
    (ov5675, "i2c-OVTI5678:00"),
    (ov8856, "i2c-OVTI8856:00"),
):
    insert_once(
        sensor_path,
        "#include <linux/gpio/consumer.h>\n",
        "#include <linux/dmi.h>\n",
        "#include <linux/dmi.h>",
    )
    insert_once(
        tps68470_h,
        "struct tps68470_regulator_platform_data;\n",
        "struct software_node;\n",
        "struct software_node",
    )
    insert_once(
        tps68470_h,
        "\tconst struct tps68470_regulator_platform_data *tps68470_regulator_pdata;\n",
        "\tconst struct software_node *tps68470_gpio_swnode;\n",
        "tps68470_gpio_swnode",
    )
    replace_once(
        tps68470_c,
        '\t\tcells[2].name = "tps68470-gpio";\n',
        '\t\tcells[2].name = "tps68470-gpio";\n'
        "\t\tcells[2].swnode = board_data->tps68470_gpio_swnode;\n",
        "cells[2].swnode = board_data->tps68470_gpio_swnode;",
    )
    insert_once(
        sensor_path,
        "#include <linux/unaligned.h>\n",
        "#include <linux/string.h>\n",
        "#include <linux/string.h>",
    )
    clock_get = (
        "\tov5675->xvclk = devm_v4l2_sensor_clk_get(dev, NULL);\n"
        if sensor_path == ov5675
        else '\tov8856->xvclk = devm_v4l2_sensor_clk_get_legacy(dev, "xvclk", false, 0);\n'
    )
    insert_once(
        sensor_path,
        clock_get,
        f"""\t/* gbos: wait for IPU bridge endpoint before acquiring resources */
\tif (dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
\t    dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable") &&
\t    !strcmp(dev_name(dev), "{device_name}")) {{
\t\tep = fwnode_graph_get_next_endpoint(fwnode, NULL);
\t\tif (!ep)
\t\t\treturn dev_err_probe(dev, -EPROBE_DEFER,
\t\t\t\t\t     "waiting for IPU bridge endpoint\\n");
\t\tfwnode_handle_put(ep);
\t}}

""",
        "gbos: wait for IPU bridge endpoint",
    )

replace_once(
    ov8856,
    """\tret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
\tif (ret != ARRAY_SIZE(msgs))
\t\treturn -EIO;
""",
    """\tret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
\tif (ret != ARRAY_SIZE(msgs)) {
\t\tif (reg == OV8856_REG_CHIP_ID &&
\t\t    is_acpi_node(dev_fwnode(&client->dev)) &&
\t\t    dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
\t\t    dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable") &&
\t\t    !strcmp(dev_name(&client->dev), "i2c-OVTI8856:00"))
\t\t\tdev_err(&client->dev,
\t\t\t\t"chip ID I2C read failed: reg=0x%04x ret=%d "
\t\t\t\t"expected=%zu adapter=%d addr=0x%02x\\n",
\t\t\t\treg, ret, ARRAY_SIZE(msgs), client->adapter->nr,
\t\t\t\t(unsigned int)client->addr);
\t\treturn -EIO;
\t}
""",
    "chip ID I2C read failed",
)

replace_once(
    ov8856,
    """\t\tif (ret) {
\t\t\tdev_err(ov8856->dev, "failed to find sensor: %d", ret);
\t\t\tgoto probe_power_off;
\t\t}
""",
    """\t\tif (ret) {
\t\t\tstruct acpi_device *adev = ACPI_COMPANION(ov8856->dev);

\t\t\tif (adev &&
\t\t\t    dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
\t\t\t    dmi_match(DMI_PRODUCT_NAME, "Latitude 7320 Detachable") &&
\t\t\t    !strcmp(dev_name(ov8856->dev), "i2c-OVTI8856:00"))
\t\t\t\tdev_err(ov8856->dev,
\t\t\t\t\t"failed to find sensor: %d (ACPI D0=%d "
\t\t\t\t\t"power-manageable=%d)\\n",
\t\t\t\t\tret, full_power,
\t\t\t\t\tacpi_device_power_manageable(adev));
\t\t\telse
\t\t\t\tdev_err(ov8856->dev, "failed to find sensor: %d", ret);
\t\t\tgoto probe_power_off;
\t\t}
""",
    "power-manageable=%d",
)

ipu_bridge = ROOT / "drivers/media/pci/intel/ipu-bridge.c"
insert_once(
    ipu_bridge,
    '\tIPU_SENSOR_CONFIG("OVTI8856", 3, 180000000, 360000000, 720000000),',
    '\t/* Dell Latitude 7320 Detachable front OV5675 */\n'
    '\tIPU_SENSOR_CONFIG("OVTI5678", 1, 450000000),\n',
    'IPU_SENSOR_CONFIG("OVTI5678", 1, 450000000)',
)
