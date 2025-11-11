#include "globals.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <hal/nrf_gpio.h>

#include "system.h"

// @brief 這個檔案主要負責系統層級的功能，特別是非揮發性儲存 (NVS) 的管理。
// NVS 用於儲存重啟後仍需保留的資料，例如設定或配對金鑰。

static struct nvs_fs fs;

// @brief 定義 NVS 所使用的 Flash 分割區。
// 這些宏是從 Zephyr 的 flash_map API 來的，用於取得 `storage_partition` 分割區的裝置指標和偏移量。
#define NVS_PARTITION storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

LOG_MODULE_REGISTER(system, LOG_LEVEL_INF);

static bool nvs_init = false;

/**
 * @brief 初始化 NVS (Non-Volatile Storage) 系統
 *
 * 這個函式會掛載 (mount) 在 Flash 上的一個分割區，作為 NVS 檔案系統使用。
 * NVS 是一個輕量級的、基於 ID 的儲存系統，適合用來儲存小量的鍵值對資料。
 * 如果 NVS 空間已滿或損壞，它會嘗試擦除並重新掛載。
 *
 * @return 0 表示成功, 非 0 表示失敗
 */
static int sys_nvs_init(void) {
	if (nvs_init) {
		return 0;
	}
	struct flash_pages_info info;
	fs.flash_device = NVS_PARTITION_DEVICE;
	fs.offset = NVS_PARTITION_OFFSET;
	if (flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info)) {
		LOG_ERR("Failed to get page info");
		return 1;
	}
	fs.sector_size = info.size;
	fs.sector_count = 4U;
	int err = nvs_mount(&fs);
	if (err == -EDEADLK) {
		LOG_WRN("All sectors closed, erasing all sectors...");
		err = flash_flatten(
			fs.flash_device,
			fs.offset,
			fs.sector_size * fs.sector_count
		);
		if (!err) {
			err = nvs_mount(&fs);
		}
	}
	if (err) {
		LOG_ERR("Failed to mount NVS");
		return 1;
	}
	nvs_init = true;
	return 0;
}

// @brief 使用 Zephyr 的系統初始化機制，在應用程式啟動初期自動呼叫 sys_nvs_init()。
// 這確保了在其他模組需要使用 NVS 之前，它已經被初始化完成。
SYS_INIT(sys_nvs_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);


/**
 * @brief 從 NVS 讀取重啟計數器
 */
uint8_t reboot_counter_read(void) {
	uint8_t reboot_counter;
	nvs_read(&fs, RBT_CNT_ID, &reboot_counter, sizeof(reboot_counter));
	return reboot_counter;
}

/**
 * @brief 將重啟計數器寫入 NVS
 */
void reboot_counter_write(uint8_t reboot_counter) {
	nvs_write(&fs, RBT_CNT_ID, &reboot_counter, sizeof(reboot_counter));
}

/**
 * @brief 將資料寫入 NVS
 *
 * @param id 資料的唯一識別 ID
 * @param retained_ptr (未使用)
 * @param data 要寫入的資料指標
 * @param len 資料長度
 */
void sys_write(uint16_t id, void* retained_ptr, const void* data, size_t len) {
	sys_nvs_init(); // 確保 NVS 已初始化
	int err = nvs_write(&fs, id, data, len);
	if (err < 0)
	{
		LOG_ERR("Failed to write to NVS, error: %d", err);
		return;
	}
}

/**
 * @brief 從 NVS 讀取資料
 *
 * @param id 要讀取的資料 ID
 * @param data 用於存放讀取結果的緩衝區指標
 * @param len 預期讀取的資料長度
 */
void sys_read(uint16_t id, void* data, size_t len) {
	sys_nvs_init(); // 確保 NVS 已初始化
	int err = nvs_read(&fs, id, data, len);
	if (err < 0)
	{
		if (err == -ENOENT) // 如果找不到對應 ID 的項目，這是正常情況
		{
			LOG_DBG("No entry exists for ID %d, read data set to zero", id);
		}
		else
		{
			LOG_ERR("Failed to read from NVS, error: %d", err);
			LOG_WRN("Read data set to zero");
		}
		// 讀取失敗時，將緩衝區清零，避免使用到未初始化的記憶體
		memset(data, 0, len);
		return;
	}
}
