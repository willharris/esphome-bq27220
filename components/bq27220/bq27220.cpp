#include "bq27220.h"
#include "bq27220_data_memory.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#define BQ27220_ID (0x0220u)
/** Delay between 2 writes into Subclass/MAC area. Fails at ~120us. */
#define BQ27220_MAC_WRITE_DELAY_US (250u)
/** Delay between we ask chip to load data to MAC and it become valid. Fails at ~500us. */
#define BQ27220_SELECT_DELAY_US (1000u)
/** Delay between 2 control operations(like unseal or full access). Fails at ~2500us.*/
#define BQ27220_MAGIC_DELAY_US (5000u)
/** Delay before freshly written configuration can be read. Fails at ? */
#define BQ27220_CONFIG_DELAY_US (10000u)
/** Config apply delay. Must wait, or DM read returns garbage. */
#define BQ27220_CONFIG_APPLY_US (2000000u)
/** Timeout for common operations. */
#define BQ27220_TIMEOUT_COMMON_US (2000000u)
/** Timeout for reset operation. Normally reset takes ~2s. */
#define BQ27220_TIMEOUT_RESET_US (4000000u)
/** Timeout cycle interval  */
#define BQ27220_TIMEOUT_CYCLE_INTERVAL_US (1000u)
/** Timeout cycles count helper */
#define BQ27220_TIMEOUT(timeout_us) ((timeout_us) / (BQ27220_TIMEOUT_CYCLE_INTERVAL_US))

namespace esphome {
namespace bq27220 {

static const char *const TAG = "bq27220";

static uint8_t bq27220_get_checksum(uint8_t* data, uint16_t len) {
    uint8_t ret = 0;
    for(uint16_t i = 0; i < len; i++) {
        ret += data[i];
    }
    return 0xFF - ret;
}

// NOTE: parameterCheck and dateMemoryCheck use write_register/read_register
// with ErrorCode returns. The original code used !this->write_register(...)
// which is INVERTED for ErrorCode (0 = OK, non-zero = error).
// Fixed to use != i2c::ERROR_OK.

bool BQ27220Component::parameterCheck(uint16_t address, uint32_t value, size_t size, bool update)
{
    if(!(size == 1 || size == 2 || size == 4)) {
        ESP_LOGD(TAG, "(%d) Parameter size error\n", __LINE__);
        return false;
    }

    bool ret = false;
    uint8_t buffer[6] = {0};
    uint8_t old_data[4] = {0};

    do {
        buffer[0] = address & 0xFF;
        buffer[1] = (address >> 8) & 0xFF;

        for(size_t i = 0; i < size; i++) {
            buffer[1 + size - i] = (value >> (i * 8)) & 0xFF;
        }

        if(update) {
            if(this->write_register(static_cast<uint8_t>(CommandSelectSubclass), buffer, size + 2) != i2c::ERROR_OK) {
                ESP_LOGD(TAG, "(%d) DM write failed\n", __LINE__);
                break;
            }
            delayMicroseconds(BQ27220_MAC_WRITE_DELAY_US);

            uint8_t checksum = bq27220_get_checksum(buffer, size + 2);
            buffer[0] = checksum;
            buffer[1] = 2 + size + 1 + 1;
            if(this->write_register(static_cast<uint8_t>(CommandMACDataSum), buffer, size + 2) != i2c::ERROR_OK) {
                ESP_LOGD(TAG, "(%d) CRC write failed\n", __LINE__);
                break;
            }
            delayMicroseconds(BQ27220_CONFIG_DELAY_US);
            ret = true;
        } else {
            if(this->write_register(static_cast<uint8_t>(CommandSelectSubclass), buffer, 2) != i2c::ERROR_OK) {
                ESP_LOGD(TAG, "(%d) DM SelectSubclass for read failed\n", __LINE__);
                break;
            }
            delayMicroseconds(BQ27220_SELECT_DELAY_US);

            if(this->read_register(static_cast<uint8_t>(CommandMACData), old_data, size) != i2c::ERROR_OK) {
                ESP_LOGD(TAG, "(%d) DM read failed\n", __LINE__);
                break;
            }
            delayMicroseconds(BQ27220_SELECT_DELAY_US);

            if(*(uint32_t*)&(old_data[0]) != *(uint32_t*)&(buffer[2])) {
                ESP_LOGD(TAG, 
                    "(%d) Data at 0x%04x(%zu): 0x%08lx!=0x%08lx\n", __LINE__,
                    address,
                    size,
                    *(uint32_t*)&(old_data[0]),
                    *(uint32_t*)&(buffer[2]));
            } else {
                ret = true;
            }
        }
    } while(0);

    return ret;
}

bool BQ27220Component::dateMemoryCheck(const BQ27220DMData *data_memory, bool update)
{
    if(update) {
        const uint16_t cfg_request = Control_ENTER_CFG_UPDATE;
        if(this->write_register(static_cast<uint8_t>(CommandSelectSubclass), (uint8_t*)&cfg_request, sizeof(cfg_request)) != i2c::ERROR_OK) {
            ESP_LOGD(TAG, "(%d) ENTER_CFG_UPDATE command failed", __LINE__);
            return false;
        }

        uint32_t timeout = BQ27220_TIMEOUT(BQ27220_TIMEOUT_COMMON_US);
        BQ27220OperationStatus operation_status;
        while(--timeout > 0) {
            if(!getOperationStatus(&operation_status)) {
                ESP_LOGD(TAG, "(%d) Failed to get operation status, retries left %lu", __LINE__, timeout);
            } else if(operation_status.reg.CFGUPDATE) {
                break;
            };
            delayMicroseconds(BQ27220_TIMEOUT_CYCLE_INTERVAL_US);
        }

        if(timeout == 0) {
            ESP_LOGD(TAG, 
                "(%d) Enter CFGUPDATE mode failed, CFG %u, SEC %u", __LINE__,
                operation_status.reg.CFGUPDATE,
                operation_status.reg.SEC);
            return false;
        }
    }

    bool result = true;
    while (data_memory->type != BQ27220DMTypeEnd)
    {
        if(data_memory->type == BQ27220DMTypeWait) {
            delayMicroseconds(data_memory->value.u32);
        } else if(data_memory->type == BQ27220DMTypeU8) {
            result &= parameterCheck(data_memory->address, data_memory->value.u8, 1, update);
        } else if(data_memory->type == BQ27220DMTypeU16) {
            result &= parameterCheck(data_memory->address, data_memory->value.u16, 2, update);
        } else if(data_memory->type == BQ27220DMTypeU32) {
            result &= parameterCheck(data_memory->address, data_memory->value.u32, 4, update);
        } else if(data_memory->type == BQ27220DMTypeI8) {
            result &= parameterCheck(data_memory->address, data_memory->value.i8, 1, update);
        } else if(data_memory->type == BQ27220DMTypeI16) {
            result &= parameterCheck(data_memory->address, data_memory->value.i16, 2, update);
        } else if(data_memory->type == BQ27220DMTypeI32) {
            result &= parameterCheck(data_memory->address, data_memory->value.i32, 4, update);
        } else if(data_memory->type == BQ27220DMTypeF32) {
            result &= parameterCheck(data_memory->address, data_memory->value.u32, 4, update);
        } else if(data_memory->type == BQ27220DMTypePtr8) {
            result &= parameterCheck(data_memory->address, *(uint8_t*)data_memory->value.u32, 1, update);
        } else if(data_memory->type == BQ27220DMTypePtr16) {
            result &= parameterCheck(data_memory->address, *(uint16_t*)data_memory->value.u32, 2, update);
        } else if(data_memory->type == BQ27220DMTypePtr32) {
            result &= parameterCheck(data_memory->address, *(uint32_t*)data_memory->value.u32, 4, update);
        } else {
            ESP_LOGD(TAG, "(%d) Invalid DM Type\n", __LINE__);
        }
        data_memory++;
    }
    
    if(update && result) {
        controlSubCmd(Control_EXIT_CFG_UPDATE_REINIT);
        delayMicroseconds(BQ27220_CONFIG_APPLY_US);

        uint32_t timeout = BQ27220_TIMEOUT(BQ27220_TIMEOUT_COMMON_US);
        BQ27220OperationStatus operation_status;
        while(--timeout > 0) {
            if(!getOperationStatus(&operation_status)) {
                ESP_LOGD(TAG, "(%d) Failed to get operation status, retries left %lu\n", __LINE__, timeout);
            } else if(operation_status.reg.CFGUPDATE != true) {
                break;
            }
            delayMicroseconds(BQ27220_TIMEOUT_CYCLE_INTERVAL_US);
        }

        if(timeout == 0) {
            ESP_LOGD(TAG, "(%d) Exit CFGUPDATE mode failed\n", __LINE__);
            return false;
        }
    }
    return result;
}

bool BQ27220Component::init(const BQ27220DMData *data_memory)
{
    bool result = false;
    bool reset_and_provisioning_required = false;

    do{
        uint16_t data = getDeviceNumber();
        if(data != BQ27220_ID) {
            ESP_LOGD(TAG, "(%d) Invalid Device Number %04x != 0x0220\n", __LINE__, data);
            break;
        }
        
        if(!unsealAccess()) {
            break;
        }

        BQ27220OperationStatus operat;
        if(!getOperationStatus(&operat)) {
            break;
        }
        if(!operat.reg.INITCOMP || operat.reg.CFGUPDATE) {
            ESP_LOGD(TAG, "(%d) Incorrect state, reset needed\n", __LINE__);
            reset_and_provisioning_required = true;
        }

        ESP_LOGD(TAG, "(%d) Checking chosen profile\n", __LINE__);
        BQ27220ControlStatus control_status;
        if(!getControlStatus(&control_status)) {
            ESP_LOGD(TAG, "(%d) Failed to get control status\n", __LINE__);
            break;
        }
        if(control_status.reg.BATT_ID != 0) {
            ESP_LOGD(TAG, "(%d) Incorrect profile, reset needed\n", __LINE__);
            reset_and_provisioning_required = true;
        }

        if(!reset_and_provisioning_required) {
            ESP_LOGD(TAG, "(%d) Checking data memory\n", __LINE__);
            if(!dateMemoryCheck(data_memory, false)) {
                ESP_LOGD(TAG, "(%d) Incorrect configuration data, reset needed\n", __LINE__);
                reset_and_provisioning_required = true;
            }
        }

        if(reset_and_provisioning_required) {
            if(!reset()) {
                ESP_LOGD(TAG, "(%d) Failed to reset device\n", __LINE__);
            }

            if(!fullAccess()) {
                break;
            }

            ESP_LOGD(TAG, "(%d) Updating data memory\n", __LINE__);
            dateMemoryCheck(data_memory, true);
            if(!dateMemoryCheck(data_memory, false)) {
                ESP_LOGD(TAG, "(%d) Data memory update failed\n", __LINE__);
                break;
            }
        }

        if(!sealAccess()) {
            ESP_LOGD(TAG, "(%d) Seal failed\n", __LINE__);
            break;
        }

        result = true;  // All steps succeeded
    } while(0);
    return result;
}

bool BQ27220Component::reset(void)
{
    bool result = false;
    do{
        controlSubCmd(Control_RESET);

        uint32_t timeout = BQ27220_TIMEOUT(BQ27220_TIMEOUT_RESET_US);
        BQ27220OperationStatus operat = {0};
        while (--timeout > 0)
        {
            if(!getOperationStatus(&operat)){
                ESP_LOGD(TAG, "Failed to get operation status, retries left %lu\n", timeout);
            }else if(operat.reg.INITCOMP == true){
                break;
            }
            delayMicroseconds(BQ27220_TIMEOUT_CYCLE_INTERVAL_US);
        }
        if(timeout == 0) {
            ESP_LOGD(TAG, "INITCOMP timeout after reset");
            break;
        }
        ESP_LOGD(TAG, "(%d) Cycles left: %lu\n", __LINE__, timeout);
        result = true;
    } while(0);
    return result;
}

bool BQ27220Component::sealAccess(void) 
{
    bool result = false;
    BQ27220OperationStatus operat = {0};
    do{
        getOperationStatus(&operat);
        if(operat.reg.SEC == Bq27220OperationStatusSecSealed)
        {
            result = true;
            break;
        }

        controlSubCmd(Control_SEALED);
        delayMicroseconds(BQ27220_SELECT_DELAY_US);

        getOperationStatus(&operat);
        if(operat.reg.SEC != Bq27220OperationStatusSecSealed)
        {
            ESP_LOGD(TAG, "Seal failed %u\n", operat.reg.SEC);
            break;
        }
        result = true;
    } while(0);

    return result;
}

bool BQ27220Component::unsealAccess(void) 
{
    bool result = false;
    BQ27220OperationStatus operat = {0};

    do{
        getOperationStatus(&operat);
        if(operat.reg.SEC != Bq27220OperationStatusSecSealed)
        {
            result = true;
            break;
        }

        controlSubCmd(UnsealKey1);
        delayMicroseconds(BQ27220_MAGIC_DELAY_US);
        controlSubCmd(UnsealKey2);
        delayMicroseconds(BQ27220_MAGIC_DELAY_US);

        getOperationStatus(&operat);
        if(operat.reg.SEC != Bq27220OperationStatusSecUnsealed)
        {
            ESP_LOGD(TAG, "Unseal failed %u\n", operat.reg.SEC);
            break;
        }
        result = true;
    } while (0);

    return result;
}

bool BQ27220Component::fullAccess(void) 
{
    bool result = false;
    BQ27220OperationStatus operat = {0};

    do{
        uint32_t timeout = BQ27220_TIMEOUT(BQ27220_TIMEOUT_COMMON_US);
        while (--timeout > 0)
        {
            if(!getOperationStatus(&operat)){
                ESP_LOGD(TAG, "Failed to get operation status, retries left %lu\n", timeout);
            }else {
                break;
            }
        }
        if(timeout == 0) {
            ESP_LOGD(TAG, "Failed to get operation status");
            break;
        }

        if(operat.reg.SEC == Bq27220OperationStatusSecFull){
            result = true;
            break;
        }
        if(operat.reg.SEC != Bq27220OperationStatusSecUnsealed){
            ESP_LOGD(TAG, "(%d) Not in unsealed state\n", __LINE__);
            break;
        }

        controlSubCmd(FullAccessKey);
        delayMicroseconds(BQ27220_MAGIC_DELAY_US);
        controlSubCmd(FullAccessKey);
        delayMicroseconds(BQ27220_MAGIC_DELAY_US);

        if(!getOperationStatus(&operat)){
            ESP_LOGD(TAG, "Status query failed");
            break;
        }
        if(operat.reg.SEC != Bq27220OperationStatusSecFull){
            ESP_LOGD(TAG, "Full access failed %u\n", operat.reg.SEC);
            break;
        }
        result = true;
    } while (0);
    return result;
}

uint16_t BQ27220Component::getDeviceNumber(void)
{
    uint16_t devid = 0;
    controlSubCmd(Control_DEVICE_NUMBER);
    delayMicroseconds(BQ27220_SELECT_DELAY_US);
    this->read_register(static_cast<uint8_t>(CommandMACData), (uint8_t *)&devid, 2);
    return devid;
}

uint16_t BQ27220Component::getVoltage(void)
{
    return readRegU16(CommandVoltage);
}
int16_t BQ27220Component::getCurrent(void)
{
    return readRegU16(CommandCurrent);
}
bool BQ27220Component::getControlStatus(BQ27220ControlStatus *ctrl_sta)
{
    (*ctrl_sta).full = readRegU16(CommandControl);
    return true;
}
bool BQ27220Component::getBatteryStatus(BQ27220BatteryStatus *batt_sta)
{
    (*batt_sta).full = readRegU16(CommandBatteryStatus);
    return true;
}
bool BQ27220Component::getOperationStatus(BQ27220OperationStatus *oper_sta)
{
    (*oper_sta).full = readRegU16(CommandOperationStatus);
    return true;
}
bool BQ27220Component::getGaugingStatus(BQ27220GaugingStatus *gauging_sta)
{
    controlSubCmd(Control_GAUGING_STATUS);
    delayMicroseconds(BQ27220_SELECT_DELAY_US);
    (*gauging_sta).full = readRegU16(CommandMACData);
    return true;
}
uint16_t BQ27220Component::getTemperature(void)
{
    return readRegU16(CommandTemperature);
}
uint16_t BQ27220Component::getFullChargeCapacity(void)
{
    return readRegU16(CommandFullChargeCapacity);
}
uint16_t BQ27220Component::getDesignCapacity(void)
{
    return readRegU16(CommandDesignCapacity);
}
uint16_t BQ27220Component::getRemainingCapacity(void)
{
    return readRegU16(CommandRemainingCapacity);
}
uint16_t BQ27220Component::getStateOfCharge(void)
{
    return readRegU16(CommandStateOfCharge);
}
uint16_t BQ27220Component::getStateOfHealth(void)
{
    return readRegU16(CommandStateOfHealth);
}


// Helper: publish NaN to all sensors (shows "unavailable" in HA)
void BQ27220Component::publish_all_nan_() {
    if (voltage_sensor_ != nullptr) voltage_sensor_->publish_state(NAN);
    if (current_sensor_ != nullptr) current_sensor_->publish_state(NAN);
    if (soc_sensor_ != nullptr) soc_sensor_->publish_state(NAN);
    if (remaining_capacity_sensor_ != nullptr) remaining_capacity_sensor_->publish_state(NAN);
    if (temperature_sensor_ != nullptr) temperature_sensor_->publish_state(NAN);
    if (full_charge_capacity_sensor_ != nullptr) full_charge_capacity_sensor_->publish_state(NAN);
    if (design_capacity_sensor_ != nullptr) design_capacity_sensor_->publish_state(NAN);
    if (state_of_health_sensor_ != nullptr) state_of_health_sensor_->publish_state(NAN);
    if (device_number_sensor_ != nullptr) device_number_sensor_->publish_state(NAN);
}


void BQ27220Component::setup() {
    ESP_LOGI(TAG, "Initializing BQ27220...");

    // Probe the device: try reading the Control register
    uint8_t probe[2] = {0, 0};
    auto err = this->read_register(static_cast<uint8_t>(CommandControl), probe, 2);
    if (err == i2c::ERROR_OK) {
        this->gauge_available_ = true;
        ESP_LOGI(TAG, "BQ27220 detected on I2C bus (probe OK)");

        uint16_t devid = this->getDeviceNumber();
        ESP_LOGI(TAG, "BQ27220 Device Number: 0x%04X (expected 0x%04X)", devid, BQ27220_ID);
        if (devid != BQ27220_ID) {
            ESP_LOGW(TAG, "Unexpected device ID! Gauge may not be a BQ27220.");
        }

        // Initialize CEDV gauging configuration (250mAh Nesso N1 battery)
        // This checks current config, only writes if values differ, then re-seals.
        ESP_LOGI(TAG, "Checking/applying CEDV configuration...");
        if (this->init(gauge_data_memory)) {
            ESP_LOGI(TAG, "CEDV configuration OK (250mAh profile)");
            this->cedv_configured_ = true;
        } else {
            ESP_LOGW(TAG, "CEDV configuration failed! SOC readings may be inaccurate.");
            ESP_LOGW(TAG, "Voltage/current/temperature readings are still valid.");
        }
    } else {
        this->gauge_available_ = false;
        ESP_LOGW(TAG, "BQ27220 NOT detected at 0x55! (err=%d)", (int)err);
        ESP_LOGW(TAG, "Fuel gauge may be in SHUTDOWN mode.");
        ESP_LOGW(TAG, "Try: charge via USB 1+ hour, then power-cycle.");
        ESP_LOGW(TAG, "Readings will show unavailable until gauge responds.");
    }
}

  
void BQ27220Component::update() {

    // If gauge was previously unavailable, try probing again
    if (!this->gauge_available_) {
        uint8_t probe[2] = {0, 0};
        auto err = this->read_register(static_cast<uint8_t>(CommandControl), probe, 2);
        if (err == i2c::ERROR_OK) {
            this->gauge_available_ = true;
            ESP_LOGI(TAG, "BQ27220 is now responding! Resuming readings.");

            // If init() didn't run at setup (gauge was in SHUTDOWN), do it now
            if (!this->cedv_configured_) {
                uint16_t devid = this->getDeviceNumber();
                ESP_LOGI(TAG, "BQ27220 Device Number: 0x%04X", devid);
                ESP_LOGI(TAG, "Checking/applying CEDV configuration...");
                if (this->init(gauge_data_memory)) {
                    ESP_LOGI(TAG, "CEDV configuration OK (250mAh profile)");
                    this->cedv_configured_ = true;
                } else {
                    ESP_LOGW(TAG, "CEDV configuration failed! Will retry next cycle.");
                }
            }
        } else {
            ESP_LOGD(TAG, "BQ27220 still not responding, skipping update.");
            this->publish_all_nan_();
            return;
        }
    }

    // Reset flag; readRegU16() will set it false on any I2C error
    this->gauge_available_ = true;

    // --- Read all values first, bail if comms fail ---

    uint16_t voltage_mv = this->getVoltage();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    int16_t current_raw = this->getCurrent();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t soc = this->getStateOfCharge();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t remaining_mah = this->getRemainingCapacity();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t raw_temp = this->getTemperature();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t fcc_mah = this->getFullChargeCapacity();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t dc_mah = this->getDesignCapacity();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t soh = this->getStateOfHealth();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    uint16_t devnum = this->getDeviceNumber();
    if (!this->gauge_available_) { this->publish_all_nan_(); return; }

    // --- All reads succeeded, publish values ---

    float temperature = (raw_temp * 0.1f) - 273.15f;

    ESP_LOGD(TAG, "Voltage: %u mV", voltage_mv);
    ESP_LOGD(TAG, "Current: %d mA", current_raw);
    ESP_LOGD(TAG, "SOC: %u%%", soc);
    ESP_LOGD(TAG, "Remaining: %u mAh", remaining_mah);
    ESP_LOGD(TAG, "Temp: %.2f C (raw %u)", temperature, raw_temp);
    ESP_LOGD(TAG, "FCC: %u mAh", fcc_mah);
    ESP_LOGD(TAG, "Design Cap: %u mAh", dc_mah);
    ESP_LOGD(TAG, "SOH: %u%%", soh);
    ESP_LOGD(TAG, "Device: 0x%04X", devnum);

    if (voltage_sensor_ != nullptr)
        voltage_sensor_->publish_state(voltage_mv / 1000.0f);
    if (current_sensor_ != nullptr)
        current_sensor_->publish_state(current_raw);
    if (soc_sensor_ != nullptr)
        soc_sensor_->publish_state(soc);
    if (remaining_capacity_sensor_ != nullptr)
        remaining_capacity_sensor_->publish_state(remaining_mah);
    if (temperature_sensor_ != nullptr)
        temperature_sensor_->publish_state(temperature);
    if (full_charge_capacity_sensor_ != nullptr)
        full_charge_capacity_sensor_->publish_state(fcc_mah);
    if (design_capacity_sensor_ != nullptr)
        design_capacity_sensor_->publish_state(dc_mah);
    if (state_of_health_sensor_ != nullptr)
        state_of_health_sensor_->publish_state(soh);
    if (device_number_sensor_ != nullptr)
        device_number_sensor_->publish_state(devnum);
}

}  // namespace bq27220
}  // namespace esphome
