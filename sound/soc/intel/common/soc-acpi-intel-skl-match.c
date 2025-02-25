// SPDX-License-Identifier: GPL-2.0
/*
 * soc-apci-intel-skl-match.c - tables and support for SKL ACPI enumeration.
 *
 * Copyright (c) 2018, Intel Corporation.
 *
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include "../skylake/skl.h"

static struct skl_machine_pdata skl_dmic_data;

static struct snd_soc_acpi_codecs skl_codecs = {
	.num_codecs = 1,
	.codecs = {"10508825"}
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_skl_machines[] = {
	{
		.id = "INT343A",
		.drv_name = "skl_alc286s_i2s",
		.fw_filename = "intel/dsp_fw_release.bin",
		.sof_fw_filename = "intel/sof-skl.ri",
		.sof_tplg_filename = "intel/sof-skl.tplg",
		.asoc_plat_name = "0000:00:1f.03",
	},
	{
		.id = "INT343B",
		.drv_name = "skl_n88l25_s4567",
		.fw_filename = "intel/dsp_fw_release.bin",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &skl_codecs,
		.pdata = &skl_dmic_data,
		.sof_fw_filename = "intel/sof-skl.ri",
		.sof_tplg_filename = "intel/sof-skl.tplg",
		.asoc_plat_name = "0000:00:1f.03",
	},
	{
		.id = "MX98357A",
		.drv_name = "skl_n88l25_m98357a",
		.fw_filename = "intel/dsp_fw_release.bin",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &skl_codecs,
		.pdata = &skl_dmic_data,
		.sof_fw_filename = "intel/sof-skl.ri",
		.sof_tplg_filename = "intel/sof-skl.tplg",
		.asoc_plat_name = "0000:00:1f.03",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_skl_machines);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Common ACPI Match module");
