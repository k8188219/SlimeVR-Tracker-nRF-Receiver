/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2025 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#include "globals.h"
#include "system/system.h"
#include "hid.h"

#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/crc.h>

#include "esb.h"

// @brief 這個檔案負責處理與 SlimeVR Trackers 之間的無線通訊。
// 它使用了 Nordic 的 Enhanced ShockBurst (ESB) 協定，這是一種低功耗的私有 2.4GHz 協定。

// @brief 用於接收和發送 ESB 封包的緩衝區
static struct esb_payload rx_payload;
static struct esb_payload tx_payload_pair = ESB_CREATE_PAYLOAD(0, 0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_sync = ESB_CREATE_PAYLOAD(0, 0, 0, 0, 0);

// @brief 用於暫存配對過程中收到的 8 位元組封包
uint8_t pairing_buf[8] = {0};
// @brief 用於過濾掉偶然出現的、來自未配對 tracker 的雜訊封包。
// 一個 tracker 需要連續被偵測到 DETECTION_THRESHOLD 次，其資料才會被接受。
static uint8_t discovered_trackers[MAX_TRACKERS] = {0};

LOG_MODULE_REGISTER(esb_event, LOG_LEVEL_INF);

// @brief 執行緒原型宣告
static void esb_packet_filter_thread(void);
K_THREAD_DEFINE(esb_packet_filter_thread_id, 256, esb_packet_filter_thread, NULL, NULL, NULL, 6, 0, 0);

static void esb_thread(void);
K_THREAD_DEFINE(esb_thread_id, 1024, esb_thread, NULL, NULL, NULL, 6, 0, 0);

static void esb_parse_pair(void);

/**
 * @brief ESB 事件處理回呼函式
 *
 * 這個函式由 ESB 驅動程式在發生特定事件 (如成功發送、發送失敗、收到封包) 時呼叫。
 * 這是所有無線通訊的入口點。
 */
void event_handler(struct esb_evt const *event)
{
	switch (event->evt_id)
	{
	case ESB_EVENT_TX_SUCCESS:
		LOG_DBG("TX SUCCESS");
		break;
	case ESB_EVENT_TX_FAILED:
		LOG_DBG("TX FAILED");
		break;
	case ESB_EVENT_RX_RECEIVED:
		// 當收到一個封包時
		if (esb_read_rx_payload(&rx_payload) == 0)
		{
			// 根據封包長度來判斷其類型
			switch (rx_payload.length)
			{
			case 8: // 8 位元組長度的是配對封包
				LOG_DBG("rx pairing packet: %16llX", *(uint64_t *)rx_payload.data);
				memcpy(pairing_buf, rx_payload.data, 8);
				// 簡單的日誌記錄，用於除錯配對流程
				switch (pairing_buf[1])
				{
				case 1:
					LOG_DBG("RX Pairing Sent ACK");
					break;
				case 2:
					LOG_DBG("RX Pairing ACK Receiver");
					break;
				default:
					LOG_INF("RX Pairing Request");
					break;
				}
				break;
			case 16: // 16 位元組長度的是標準的 tracker 資料封包
				uint8_t imu_id = rx_payload.data[1]; // 取得 tracker ID
				if (imu_id >= stored_trackers) { // 如果 ID 超出已儲存的 tracker 數量，則忽略
					return;
				}
				// 雜訊過濾
				if (discovered_trackers[imu_id] < DETECTION_THRESHOLD)
				{
					discovered_trackers[imu_id]++;
					return;
				}
				if (rx_payload.data[0] > 223) { // 封包類型 > 223 為接收器保留，忽略
					break;
				}
				// 將有效的 tracker 資料寫入 HID FIFO，準備發送給電腦
				hid_write_packet_n(rx_payload.data, rx_payload.rssi);
				break;
			default:
				break;
			}
		}
		else
		{
			LOG_ERR("Error while reading rx packet");
		}
		break;
	}
}

/**
 * @brief 啟動 nRF SoC 的高頻時鐘 (HFCLK)
 *
 * ESB 無線電需要高頻時鐘才能運作。這個函式確保 HFCLK 已經被啟動。
 * 它使用了 Zephyr 的 onoff manager 服務來以非同步的方式請求時鐘資源。
 *
 * @return 0 表示成功, 非 0 表示失敗
 */
int clocks_start(void)
{
	int err;
	int res;
	struct onoff_manager *clk_mgr;
	struct onoff_client clk_cli;

	clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (!clk_mgr)
	{
		LOG_ERR("Unable to get the Clock manager");
		return -ENXIO;
	}

	sys_notify_init_spinwait(&clk_cli.notify);

	err = onoff_request(clk_mgr, &clk_cli);
	if (err < 0)
	{
		LOG_ERR("Clock request failed: %d", err);
		return err;
	}

	// 等待時鐘啟動完成
	do
	{
		err = sys_notify_fetch_result(&clk_cli.notify, &res);
		if (!err && res)
		{
			LOG_ERR("Clock could not be started: %d", res);
			return res;
		}
	} while (err);

	LOG_DBG("HF clock started");
	return 0;
}


// @brief 定義了 ESB 在不同模式下使用的無線電位址。
// ESB 使用一組 base address 和一組 prefix 來構成最多 8 個不同的通訊管道 (pipe)。

// @brief 這些是用於「發現/配對模式」的固定位址。
// 所有的 SlimeVR 接收器和 tracker 在進入配對模式時，都會監聽和使用這些共同的位址。
static const uint8_t discovery_base_addr_0[4] = {0x62, 0x39, 0x8A, 0xF2};
static const uint8_t discovery_base_addr_1[4] = {0x28, 0xFF, 0x50, 0xB8};
static const uint8_t discovery_addr_prefix[8] = {0xFE, 0xFF, 0x29, 0x27, 0x09, 0x02, 0xB2, 0xD6};

// @brief 這些變數用於儲存當前正在使用的位址。
static uint8_t base_addr_0[4], base_addr_1[4], addr_prefix[8] = {0};

static bool esb_initialized = false;

/**
 * @brief 初始化 ESB 模組
 *
 * @param tx (在此專案中未使用，接收器總是作為 PRX (Primary Receiver) 運作)
 * @return 0 表示成功, 非 0 表示失敗
 */
int esb_initialize(bool tx)
{
	if (esb_initialized) {
		LOG_WRN("ESB already initialized");
	}
	int err;

	// 使用 Zephyr 提供的預設 ESB 設定
	struct esb_config config = ESB_DEFAULT_CONFIG;

	// 無論 tx 參數為何，接收器都設定為 PRX 模式
	config.mode = ESB_MODE_PRX;
	config.event_handler = event_handler; // 設定事件回呼函式
	config.tx_output_power = 30; // 設定發射功率
	config.tx_mode = ESB_TXMODE_AUTO; // PRX 模式下，ACK 會自動發送
	config.selective_auto_ack = true; // 啟用選擇性自動 ACK

	LOG_INF("Initializing ESB, %sX mode", tx ? "T" : "R");
	err = esb_init(&config);
	if (err) {
		LOG_ERR("ESB initialization failed: %d", err);
		set_status(SYS_STATUS_CONNECTION_ERROR, true);
		return err;
	}

	// 設定 ESB 使用的基地位址和前綴
	err = esb_set_base_address_0(base_addr_0);
	if (err) return err;

	err = esb_set_base_address_1(base_addr_1);
	if (err) return err;

	err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
	if (err) return err;


	esb_initialized = true;
	return 0;
}

/**
 * @brief 停用並反初始化 ESB 模組
 *
 * 這個函式會安全地關閉 ESB 無線電。
 */
static void esb_deinitialize(void)
{
	LOG_INF("ESB deinitialize requested");
	if (esb_initialized)
	{
		esb_initialized = false;
		LOG_INF("Deinitializing ESB");
		k_msleep(10); // 短暫等待，確保正在進行的傳輸可以完成
		if (esb_initialized) // 在等待期間，可能有其他執行緒重新初始化了 ESB
		{
			LOG_INF("ESB deinitialize cancelled");
			return;
		}
		esb_disable(); // 實際停用 ESB 硬體
	}
	esb_initialized = false;
}

/**
 * @brief 將 ESB 位址設定為「發現/配對模式」
 *
 * 這個函式會將 `base_addr_0`, `base_addr_1`, 和 `addr_prefix` 設為固定的發現位址。
 */
inline void esb_set_addr_discovery(void)
{
	memcpy(base_addr_0, discovery_base_addr_0, sizeof(base_addr_0));
	memcpy(base_addr_1, discovery_base_addr_1, sizeof(base_addr_1));
	memcpy(addr_prefix, discovery_addr_prefix, sizeof(addr_prefix));
}

/**
 * @brief 將 ESB 位址設定為「已配對模式」
 *
 * 在這個模式下，接收器會使用基於自身硬體位址 (NRF_FICR->DEVICEADDR) 生成的獨特位址。
 * 這樣可以確保每個接收器和它的 trackers 之間的通訊是獨立的，不會互相干擾。
 * 位址的生成演算法確保了產生的位址符合 nRF 晶片的規範 (例如，避免使用 0x00, 0x55, 0xAA 等特殊值)。
 */
inline void esb_set_addr_paired(void)
{
	// 從 nRF 的工廠資訊配置暫存器 (FICR) 中讀取唯一的裝置位址
	uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR;
	uint8_t buf[6] = {0};
	memcpy(buf, addr, 6);
	uint8_t addr_buffer[16] = {0};
	// 根據裝置位址生成 16 個位元組的偽隨機資料
	for (int i = 0; i < 4; i++)
	{
		addr_buffer[i] = buf[i];
		addr_buffer[i + 4] = buf[i] + buf[4];
	}
	for (int i = 0; i < 8; i++)
		addr_buffer[i + 8] = buf[5] + i;
	// 確保生成的位元組中不包含 ESB 不合法的值
	for (int i = 0; i < 16; i++)
	{
		if (addr_buffer[i] == 0x00 || addr_buffer[i] == 0x55 || addr_buffer[i] == 0xAA)
			addr_buffer[i] += 8;
	}
	// 從生成的資料中提取 base_addr 和 prefix
	memcpy(base_addr_0, addr_buffer, sizeof(base_addr_0));
	memcpy(base_addr_1, addr_buffer + 4, sizeof(base_addr_1));
	memcpy(addr_prefix, addr_buffer + 8, sizeof(addr_prefix));
}

static bool esb_pairing = false;
static bool esb_paired = false;

/**
 * @brief 新增一個 tracker 到已配對清單
 *
 * @param addr 要新增的 tracker 的 6 位元組無線位址
 * @param checksum (未使用)
 */
void esb_add_pair(uint64_t addr, bool checksum)
{
	int id = stored_trackers;

	// 檢查這個 tracker 是否已經被儲存過
	for (int i = 0; i < stored_trackers; i++)
	{
		if (addr != 0 && stored_tracker_addr[i] == addr)
		{
			id = i;
			break;
		}
	}

	if (id == stored_trackers)
	{
		// 如果是新的 tracker，就將它的位址和新的 ID 存入 NVS
		LOG_INF("Added device on id %d with address %012llX", id, addr);
		stored_tracker_addr[id] = addr;
		sys_write(STORED_ADDR_0 + id, NULL, &stored_tracker_addr[id], sizeof(stored_tracker_addr[0]));
		stored_trackers++;
		sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
	}
	else
	{
		LOG_INF("Device already stored with id %d", id);
	}
}

/**
 * @brief 從已配對清單中移除最後一個加入的 tracker
 */
void esb_pop_pair(void)
{
	if (stored_trackers > 0)
	{
		stored_trackers--;
		// 更新 NVS 中儲存的 tracker 數量
		sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
		LOG_INF("Removed device on id %d with address %012llX", stored_trackers, stored_tracker_addr[stored_trackers]);
	}
	else
	{
		LOG_WRN("No devices to remove");
	}
}

/**
 * @brief 解析收到的配對請求封包
 *
 * 這個函式會從 `pairing_buf` 中提取 tracker 的無線位址，檢查其 CRC8 校驗和，
 * 並判斷這是一個新的 tracker 還是一個已知的 tracker。
 * 如果是新的 tracker，就呼叫 `esb_add_pair` 將它儲存起來。
 * 最後，它會準備一個回應封包，其中包含了分配給這個 tracker 的 ID。
 */
void esb_parse_pair()
{
	// 從 8 位元組的配對封包中提取 6 位元組的 tracker 位址
	uint64_t found_addr = (*(uint64_t *)pairing_buf >> 16) & 0xFFFFFFFFFFFF;
	uint16_t send_tracker_id = stored_trackers; // 預設分配一個新的 ID

	// 檢查這個位址是否已經存在於已儲存的清單中
	for (int i = 0; i < stored_trackers; i++)
	{
		if (found_addr != 0 && stored_tracker_addr[i] == found_addr)
		{
			send_tracker_id = i; // 如果存在，就使用舊的 ID
			break;
		}
	}
	// 計算收到的 tracker 位址的 CRC8 校驗和，以驗證封包的完整性
	uint8_t checksum = crc8_ccitt(0x07, &pairing_buf[2], 6);
	if (checksum == 0) checksum = 8; // CRC 結果不能為 0

	// 驗證校驗和是否與封包中包含的校驗和 (pairing_buf[0]) 相符
	if (checksum == pairing_buf[0] && found_addr != 0 && send_tracker_id == stored_trackers && stored_trackers < MAX_TRACKERS)
	{
		// 如果校驗和正確，且這是一個新的 tracker，且還有空間，就新增它
		esb_add_pair(found_addr, false);
		set_led(SYS_LED_PATTERN_ONESHOT_PROGRESS, SYS_LED_PRIORITY_HIGHEST); // 閃爍 LED 提示
	}

	// 準備回應封包
	if (checksum == pairing_buf[0] && send_tracker_id < MAX_TRACKERS) {
		// 如果校驗和正確且 tracker ID 有效，則在回應封包中也使用相同的校驗和，
		// tracker 會用這個來確認這是對自己請求的回應。
		tx_payload_pair.data[0] = pairing_buf[0];
	} else {
		tx_payload_pair.data[0] = 0; // 否則，將校驗和設為 0，使回應封包無效
	}
	tx_payload_pair.data[1] = send_tracker_id; // 在回應封包中包含分配的 tracker ID
}

/**
 * @brief 執行配對流程的主函式
 *
 * 這是一個阻塞函式，它會持續執行直到 `esb_pairing` 被設為 `false`。
 * 它會將 ESB 設定為發現模式，然後在一個迴圈中等待並處理來自 tracker 的配對請求。
 */
void esb_pair(void)
{
	LOG_INF("Pairing");
	esb_set_addr_discovery(); // 設定為發現位址
	esb_initialize(false); // 初始化 ESB
	esb_start_rx(); // 開始接收
	tx_payload_pair.noack = false; // 我們需要收到 tracker 的 ACK

	// 在回應封包中包含接收器自身的位址，以便 tracker 能夠儲存
	uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR;
	memcpy(&tx_payload_pair.data[2], addr, 6);
	LOG_INF("Device address: %012llX", *addr & 0xFFFFFFFFFFFF);
	set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_CONNECTION); // 設定 LED 為配對模式的閃爍方式

	esb_pairing = true;
	pairing_buf[1] = 255; // 初始化封包旗標，255 表示尚未收到有效封包

	while (esb_pairing)
	{
		if (!esb_initialized)
		{
			// 如果中途 ESB 被反初始化了，就重新初始化
			esb_initialize(false);
			esb_start_rx();
		}

		// `pairing_buf[1]` 的值會在 `event_handler` 中被更新
		switch (pairing_buf[1])
		{
		case 2: // 收到 tracker 的 ACK
			esb_flush_tx(); // 清空 TX 緩衝區，準備下一次配對
		case 255: // 初始狀態或無有效封包
			break;
		default: // 收到了配對請求
			esb_parse_pair(); // 解析請求
			LOG_DBG("tx: %16llX", *(uint64_t *)tx_payload_pair.data);
			esb_write_payload(&tx_payload_pair); // 將回應封包寫入 TX 緩衝區，等待發送
			break;
		}
		pairing_buf[1] = 255; // 重置旗標
		k_usleep(100); // 短暫休眠，避免 CPU 佔用過高
	}

	set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_CONNECTION); // 結束配對，關閉 LED
	esb_deinitialize(); // 反初始化 ESB
}

/**
 * @brief 重置配對狀態
 *
 * 這個函式會停用 ESB 並將 `esb_paired` 旗標設為 false，
 * 這會讓主執行緒在下一次迴圈時重新進入配對模式。
 */
void esb_reset_pair(void)
{
	esb_deinitialize(); // 確保 ESB 已關閉
	esb_paired = false;
}

/**
 * @brief 從外部結束配對流程
 *
 * 將 `esb_pairing` 設為 false，會讓 `esb_pair` 函式的迴圈結束。
 */
void esb_finish_pair(void)
{
	esb_pairing = false;
}

/**
 * @brief 清除所有已儲存的配對資訊
 *
 * 這個函式會將 NVS 中儲存的 tracker 數量設為 0，並重置配對狀態。
 * 這是一個恢復原廠設定的功能。
 */
void esb_clear(void)
{
	stored_trackers = 0;
	sys_write(STORED_TRACKERS, NULL, &stored_trackers, sizeof(stored_trackers));
	LOG_INF("NVS Reset");
	esb_reset_pair();
}

/**
 * @brief 發送同步封包 (目前功能未完全實現)
 */
void esb_write_sync(uint16_t led_clock)
{
	if (!esb_initialized || !esb_paired)
		return;
	tx_payload_sync.noack = false;
	tx_payload_sync.data[0] = (led_clock >> 8) & 255;
	tx_payload_sync.data[1] = led_clock & 255;
	esb_write_payload(&tx_payload_sync);
}

/**
 * @brief 切換到已配對模式
 *
 * 這個函式會將 ESB 的位址設定為基於接收器自身位址的獨特位址，
 * 並將 `esb_paired` 旗標設為 true。
 */
void esb_receive(void)
{
	esb_set_addr_paired();
	esb_paired = true;
}

/**
 * @brief 雜訊封包過濾器執行緒
 *
 * 這個執行緒會定期 (每秒) 檢查 `discovered_trackers` 陣列。
 * 如果某個 tracker 的計數在一段時間內沒有達到 `DETECTION_THRESHOLD`，
 * 就將其計數歸零。這可以避免單一的雜訊封包長時間佔用一個計數位置。
 */
static void esb_packet_filter_thread(void)
{
	memset(discovered_trackers, 0, sizeof(discovered_trackers));
	while (1)
	{
		k_msleep(1000);
		for (int i = 0; i < MAX_TRACKERS; i++)
			if (discovered_trackers[i] < DETECTION_THRESHOLD)
				discovered_trackers[i] = 0;
	}
}

/**
 * @brief ESB 無線通訊的主執行緒
 *
 * 這個執行緒是整個無線通訊功能的狀態機和主迴圈。
 */
static void esb_thread(void)
{
	clocks_start(); // 啟動高頻時鐘

	// 從 NVS 中讀取已儲存的 tracker 數量和位址
	sys_read(STORED_TRACKERS, &stored_trackers, sizeof(stored_trackers));
	if (stored_trackers > 0) {
		esb_paired = true;
	}
	for (int i = 0; i < stored_trackers; i++) {
		sys_read(STORED_ADDR_0 + i, &stored_tracker_addr[i], sizeof(stored_tracker_addr[0]));
	}
	LOG_INF("%d/%d devices stored", stored_trackers, MAX_TRACKERS);

	// 如果已經有配對過的 tracker，就直接進入已配對模式
	if (esb_paired)
	{
		esb_receive();
		esb_initialize(false);
		esb_start_rx();
	}

	// 主迴圈
	while (1)
	{
		// 如果當前不是已配對狀態 (例如，剛開機且沒有儲存的 tracker，或是手動重置了配對)
		if (!esb_paired)
		{
			esb_pair(); // 進入配對流程
			// 配對流程結束後 (可能是成功配對了，或是被外部中斷)
			esb_receive(); // 切換到已配對模式的位址
			esb_initialize(false); // 重新初始化 ESB
			esb_start_rx(); // 開始接收 tracker 資料
		}
		k_msleep(100); // 主迴圈的休眠，釋放 CPU 資源
	}
}
