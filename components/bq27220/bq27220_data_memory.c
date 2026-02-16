#include "bq27220_data_memory.h"

/* ******************************************************************************
 *              Arduino Nesso N1 — CEDV Gauging Configuration
 *              Battery: 3.7V 250mAh LiPo (single cell)
 *              Charger: AW32001ECSR @ 128mA (0.5C)
 * ******************************************************************************
 *
 * CAPACITY VALUES: Correct for Nesso N1 (250 mAh).
 *
 * CEDV MODEL PARAMETERS (EMF, C0, R0, T0, R1, TC, C1):
 *   These are ESTIMATED based on scaling from a known 1300mAh LiPo profile
 *   and typical small-cell LiPo characteristics. Internal resistance (R0, R1)
 *   is scaled up because smaller cells have higher impedance.
 *
 *   For production-quality gauging accuracy, generate proper values using:
 *     TI GAUGEPARCAL tool: https://www.ti.com/tool/GAUGEPARCAL
 *   with the actual cell's discharge curves from the battery datasheet.
 *
 * DOD VOLTAGE PROFILE (DOD0..DOD100):
 *   Generic single-cell LiPo profile at low discharge rate (~0.2C).
 *   Reasonable for a 250mAh cell. Fine-tune after observing a full
 *   charge/discharge cycle with real voltage data from the gauge.
 *
 * EDV THRESHOLDS:
 *   EDV0 = 3200 mV (absolute minimum, below this cell may be damaged)
 *   EDV1 = 3250 mV (low battery warning)
 *   EDV2 = 3300 mV (empty threshold for SOC=0%)
 *   Slightly lower than the T-Embed values because smaller cells can
 *   sag more under load, and we want to avoid premature shutdown.
 *
 ***************************************************************************** */

const BQ27220DMGaugingConfig data_memory_gauging_config = {
    .CCT = 1,        // Use CC % of FullChargeCapacity().
    .CSYNC = 0,      // RM is not changed when charge termination is reached
    .EDV_CMP = 0,    // EDV compensation is disabled.
    .SC = 1,         // Learning cycle is optimized for independent charger.
    .FIXED_EDV0 = 1, // EDV0 will always use Fixed EDV0.
    .FCC_LIM = 1,    // FCC is limited to Design Capacity mAh.
    .FC_FOR_VDQ = 1, // FC is required to get VDQ
    .IGNORE_SD = 1,  // Coulomb counter increments only if there is a real discharge
    .SME0 = 0,       // Smoothing towards EDV0 disabled.
};

const BQ27220DMData gauge_data_memory[] = {
    /* --- Gauging Config flags --- */
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1GaugingConfig,
        .type = BQ27220DMTypePtr16,
        .value.u32 = (uint32_t)&data_memory_gauging_config,
    },

    /* --- Capacity: 250 mAh --- */
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1FullChargeCapacity,
        .type = BQ27220DMTypeU16,
        .value.u16 = 250,
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1DesignCapacity,
        .type = BQ27220DMTypeU16,
        .value.u16 = 250,
    },

    /* --- CEDV Model Parameters (estimated for 250mAh LiPo) --- */
    {
        /* EMF: Open-circuit voltage at ~50% SOC (mV) */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1EMF,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3680,
    },
    {
        /* C0: Capacity compensation factor */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1C0,
        .type = BQ27220DMTypeU16,
        .value.u16 = 430,
    },
    {
        /* R0: Typical internal resistance (mΩ × capacity factor)
         * Smaller cells have higher impedance. Scaled from 334 @ 1300mAh.
         * Rough scaling: R0_new ≈ R0_old × (Cap_old / Cap_new)^0.6
         * 334 × (1300/250)^0.6 ≈ 334 × 3.0 ≈ 1000 */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1R0,
        .type = BQ27220DMTypeU16,
        .value.u16 = 1000,
    },
    {
        /* T0: Temperature compensation reference (0.1K units)
         * 4626 = 462.6K... this is actually a coefficient, not temperature.
         * Keep same as T-Embed — chemistry-dependent, not size-dependent. */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1T0,
        .type = BQ27220DMTypeU16,
        .value.u16 = 4626,
    },
    {
        /* R1: Resistance gradient (higher for smaller cells)
         * Scaled: 408 × (1300/250)^0.6 ≈ 1220 */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1R1,
        .type = BQ27220DMTypeU16,
        .value.u16 = 1220,
    },
    {
        /* TC: Temperature coefficient (chemistry-dependent) */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1TC,
        .type = BQ27220DMTypeU8,
        .value.u8 = 11,
    },
    {
        /* C1: Secondary capacity compensation */
        .address = BQ27220DMAddressGasGaugingCEDVProfile1C1,
        .type = BQ27220DMTypeU8,
        .value.u8 = 0,
    },

    /* --- DOD Voltage Profile (mV at 0%, 10%, 20%... 100% depth-of-discharge) ---
     * Generic single-cell LiPo profile.
     * DOD0 = fully charged (~4.18V), DOD100 = empty (~3.2V)
     */
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD0,
        .type = BQ27220DMTypeU16,
        .value.u16 = 4180,   /* 0% DOD = full charge: ~4.18V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD10,
        .type = BQ27220DMTypeU16,
        .value.u16 = 4000,   /* 10% DOD: ~4.00V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD20,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3890,   /* 20% DOD: ~3.89V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD30,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3790,   /* 30% DOD: ~3.79V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD40,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3710,   /* 40% DOD: ~3.71V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD50,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3650,   /* 50% DOD: ~3.65V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD60,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3590,   /* 60% DOD: ~3.59V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD70,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3530,   /* 70% DOD: ~3.53V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD80,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3450,   /* 80% DOD: ~3.45V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD90,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3350,   /* 90% DOD: ~3.35V */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1StartDOD100,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3200,   /* 100% DOD = empty: ~3.20V */
    },

    /* --- End-of-Discharge Voltage Thresholds (mV) --- */
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1EDV0,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3200,   /* Absolute minimum — protect the cell */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1EDV1,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3250,   /* Low battery warning */
    },
    {
        .address = BQ27220DMAddressGasGaugingCEDVProfile1EDV2,
        .type = BQ27220DMTypeU16,
        .value.u16 = 3300,   /* Empty threshold (triggers SOC=0%) */
    },

    /* --- Calibration & Power --- */
    {
        /* Current Deadband: minimum current (mA) for coulomb counting.
         * 1 mA is fine for a 250mAh cell. */
        .address = BQ27220DMAddressCalibrationCurrentDeadband,
        .type = BQ27220DMTypeU8,
        .value.u8 = 1,
    },
    {
        /* Sleep Current threshold (mA).
         * Below this, gauge enters low-power SLEEP mode.
         * 1 mA is appropriate for a small IoT device. */
        .address = BQ27220DMAddressConfigurationPowerSleepCurrent,
        .type = BQ27220DMTypeI16,
        .value.i16 = 1,
    },

    /* --- End sentinel --- */
    {
        .type = BQ27220DMTypeEnd,
    },
};
