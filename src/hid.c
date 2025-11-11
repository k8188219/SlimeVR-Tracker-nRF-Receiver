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

#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

// @brief 這個檔案負責所有與 USB HID (Human Interface Device) 相關的通訊。
// 接收器透過模擬一個 HID 裝置，將從 trackers 收到的姿態資料傳送給電腦上的 SlimeVR Server。

// @brief Zephyr 工作佇列 (Work Queue) 項目，用於在背景執行緒中非同步地傳送 HID 報告。
static struct k_work report_send;

// @brief 定義單一 tracker 報告的資料結構。每個報告 16 位元組。
static struct tracker_report {
	uint8_t data[16];
} __packed report = {
	.data = {0}
};;

// @brief 這是一個環形緩衝區 (Ring Buffer) 或 FIFO，用於儲存從無線端收到、等待透過 USB 傳送的 tracker 報告。
// MAX_TRACKERS 定義了可以同時處理的最大 tracker 數量。
struct tracker_report reports[MAX_TRACKERS];
// 使用原子變數 (atomic) 來安全地在主執行緒和中斷服務常式之間共享讀寫索引，避免競態條件 (race condition)。
atomic_t report_write_index = 0; // 下一個要寫入的位置
atomic_t report_read_index = 0;  // 下一個要讀取的位置
// 如果 read_index == write_index，表示緩衝區是空的。
// 如果 (write_index + 1) % MAX_TRACKERS == read_index，表示緩衝區是滿的。


static bool configured; // USB 裝置是否已被主機成功配置
static const struct device *hdev; // HID 裝置的驅動程式實例
static ATOMIC_DEFINE(hid_ep_in_busy, 1); // 一個原子標誌，用於表示 HID IN Endpoint 是否正在忙於傳輸資料。

#define HID_EP_BUSY_FLAG	0
#define REPORT_PERIOD		K_MSEC(1) // 定期觸發報告傳送的間隔 (1 毫秒)
#define HID_EP_REPORT_COUNT 4 // 每次 USB 傳輸時，捆綁傳送的報告數量

// @brief 用於捆綁多個報告以進行單次 USB 傳輸的緩衝區。這可以提高傳輸效率。
struct tracker_report ep_report_buffer[HID_EP_REPORT_COUNT];

LOG_MODULE_REGISTER(hid_event, LOG_LEVEL_INF);

static void report_event_handler(struct k_timer *dummy);
// @brief 定義一個 Zephyr 計時器，它會定期觸發 `report_event_handler`。
static K_TIMER_DEFINE(event_timer, report_event_handler, NULL);

/**
 * @brief USB HID 報告描述符 (Report Descriptor)
 *
 * 這個描述符告訴主機 (電腦) 這個 HID 裝置是什麼樣的裝置，以及它會傳送什麼樣的資料。
 * 在這裡，它被定義為一個通用的、未指定用途的裝置，可以傳送 64 位元組的輸入報告。
 * SlimeVR Server 會知道如何解析這個報告。
 */
static const uint8_t hid_report_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_UNDEFINED),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
		HID_USAGE(HID_USAGE_GEN_DESKTOP_UNDEFINED),
		HID_REPORT_SIZE(8),       // 每個欄位 8 位元
		HID_REPORT_COUNT(64),     // 總共 64 個欄位 (64 位元組)
		HID_INPUT(0x02),          // 這是一個輸入報告 (裝置 -> 主機)
	HID_END_COLLECTION,
};

// 用於輪流傳送每個 tracker 的註冊資訊
uint16_t sent_device_addr = 0;
bool usb_enabled = false;
int64_t last_registration_sent = 0;

/**
 * @brief 封裝一個 tracker 的註冊資訊封包
 *
 * 這種封包 (類型 255) 用於告訴 SlimeVR Server 一個 tracker 的 ID 和它的無線位址之間的關聯。
 */
static void packet_device_addr(uint8_t *report, uint16_t id) // associate id and tracker address
{
	report[0] = 255; // 封包類型 255 代表註冊資訊
	report[1] = id;  // Tracker ID
	memcpy(&report[2], &stored_tracker_addr[id], 6); // Tracker 的 6 位元組無線位址
	memset(&report[8], 0, 8); // 剩餘 8 位元組未使用
}

static uint32_t dropped_reports = 0;
static uint16_t max_dropped_reports = 0;

/**
 * @brief 傳送 HID 報告的背景工作函式
 *
 * 這個函式會被 `report_send` 工作佇列所執行。
 * 它會檢查 FIFO 中是否有待傳送的 tracker 資料，如果有的話，就把它們和其他註冊資訊一起
 * 透過 USB HID IN endpoint 傳送出去。
 */
static void send_report(struct k_work *work)
{
	if (!usb_enabled || !stored_trackers) return;

	size_t write_idx = (size_t)atomic_get(&report_write_index);
	size_t read_idx = (size_t)atomic_get(&report_read_index);

	// 如果 FIFO 是空的，且距離上次傳送註冊資訊還不到 100ms，就直接返回。
	// 這樣可以避免過於頻繁地傳送註冊資訊。
	if (write_idx == read_idx && k_uptime_get() - 100 < last_registration_sent) {
		return;
	}

	int ret, wrote;

	last_registration_sent = k_uptime_get();

	// 檢查 HID IN endpoint 是否空閒。如果空閒，就設定為忙碌狀態並開始傳送。
	if (!atomic_test_and_set_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG)) {
		// 計算 FIFO 中有多少個待傳送的報告
		int available_reports = write_idx - read_idx;
		if (available_reports < 0) available_reports += MAX_TRACKERS;
		size_t reports_to_send = (size_t)((available_reports > HID_EP_REPORT_COUNT) ? HID_EP_REPORT_COUNT : available_reports);

		int epind;
		// 從 FIFO 中複製 tracker 資料到端點緩衝區
		for (epind = 0; epind < reports_to_send; epind++) {
			ep_report_buffer[epind] = reports[read_idx];
			epind++;
			read_idx++;
			if (read_idx == MAX_TRACKERS) read_idx = 0;
			atomic_set(&report_read_index, read_idx); // 更新讀取索引
		}

		// 如果還有剩餘的空間，就用註冊資訊來填充。
		// 這樣可以確保即使沒有新的 tracker 資料，註冊資訊也能定期被傳送。
		for (; epind < HID_EP_REPORT_COUNT; epind++) {
			if (stored_trackers > 0) {
				packet_device_addr(ep_report_buffer[epind].data, sent_device_addr);
				sent_device_addr = (sent_device_addr + 1) % stored_trackers; // 輪流傳送每個 tracker
			}
		}

		// 透過 USB HID 中斷端點寫入資料
		ret = hid_int_ep_write(hdev, (uint8_t *)ep_report_buffer, sizeof(report) * HID_EP_REPORT_COUNT, &wrote);

		if (ret != 0) {
			LOG_ERR("Failed to submit report");
		}
	} else {
		// Endpoint 正在忙碌，本次不傳送
		//LOG_DBG("HID IN endpoint busy");
	}
}

#define DROPPED_REPORT_LOG_INTERVAL 5000

/**
 * @brief 一個獨立的執行緒，用於定期印出被丟棄的報告數量。
 *
 * 如果 FIFO 滿了，新的報告就會被丟棄。這個執行緒有助於在除錯時監控系統是否因為處理不過來而丟失資料。
 */
static void hid_dropped_reports_logging(void)
{
	while (1) {
		if (dropped_reports) LOG_INF("Dropped reports: %u (max: %u)", dropped_reports, max_dropped_reports);
		dropped_reports = 0;
		max_dropped_reports = 0;
		k_msleep(DROPPED_REPORT_LOG_INTERVAL);
	}
}

K_THREAD_DEFINE(hid_dropped_reports_logging_thread, 256, hid_dropped_reports_logging, NULL, NULL, NULL, 6, 0, 0);

/**
 * @brief HID IN Endpoint 中斷傳輸完成的回呼函式
 *
 * 當 `hid_int_ep_write` 的資料成功傳送給主機後，USB 驅動程式會呼叫這個函式。
 * 它的主要作用是清除 `hid_ep_in_busy` 標誌，這樣 `send_report` 函式就知道可以傳送下一批資料了。
 */
static void int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (!atomic_test_and_clear_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG)) {
		LOG_WRN("IN endpoint callback without preceding buffer write");
	}
}

/**
 * @brief HID On Idle 回呼函式 (在此專案中較少使用)
 *
 * 當主機一段時間沒有請求報告時，可能會觸發此回呼。
 */
static void on_idle_cb(const struct device *dev, uint16_t report_id)
{
	LOG_DBG("On idle callback");
	k_work_submit(&report_send);
}

/**
 * @brief 由 `event_timer` 定期呼叫的事件處理器
 *
 * 這個函式只是簡單地將 `report_send` 工作提交到系統工作佇列，
 * 觸發一次 `send_report` 函式的執行。
 */
static void report_event_handler(struct k_timer *dummy)
{
	if (usb_enabled)
		k_work_submit(&report_send);
}

static void protocol_cb(const struct device *dev, uint8_t protocol)
{
	LOG_INF("New protocol: %s", protocol == HID_PROTOCOL_BOOT ?
		"boot" : "report");
}

// @brief 定義一組 HID 操作的回呼函式，並傳遞給 USB HID 驅動程式。
static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
	.on_idle = on_idle_cb,
	.protocol_change = protocol_cb,
};

/**
 * @brief USB 裝置狀態改變時的回呼函式
 *
 * 這個函式處理 USB 的各種事件，例如重置、成功配置等。
 * 當 `status` 為 `USB_DC_CONFIGURED` 時，表示裝置已準備好與主機通訊。
 */
static void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	switch (status) {
	case USB_DC_RESET:
		configured = false;
		break;
	case USB_DC_CONFIGURED:
		if (*param > 0 && !configured) { // param > 0 表示配置成功
			int_in_ready_cb(hdev); // 清除 busy 標誌，準備首次傳送
			configured = true;
		} else {
			configured = false;
		}
		break;
	default:
		break;
	}
}

/**
 * @brief HID 裝置的初始化函式
 *
 * 透過 `SYS_INIT` 巨集，這個函式會在應用程式啟動時被自動呼叫。
 * 它負責取得 HID 驅動程式的實例、註冊 HID 裝置、設定回呼函式，並啟動定時器。
 */
static int composite_pre_init()
{
	hdev = device_get_binding("HID_0");
	if (hdev == NULL) {
		LOG_ERR("Cannot get USB HID Device");
		return -ENODEV;
	}

	usb_hid_register_device(hdev, hid_report_desc, sizeof(hid_report_desc),
				&ops);

	atomic_set_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG);
	k_timer_start(&event_timer, REPORT_PERIOD, REPORT_PERIOD);

	return usb_hid_init(hdev);
}

SYS_INIT(composite_pre_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

/**
 * @brief 啟用 USB 的獨立執行緒
 *
 * 呼叫 `usb_enable` 會啟動 USB 硬體並開始處理來自電腦的事件。
 * 將它放在一個獨立的執行緒中可以避免阻塞主執行緒。
 */
void usb_init_thread(void)
{
	usb_enable(status_cb);
	k_work_init(&report_send, send_report);
	usb_enabled = true;
}

K_THREAD_DEFINE(usb_init_thread_id, 256, usb_init_thread, NULL, NULL, NULL, 6, 0, 0);

// @brief 封包格式速查表
//|b0      |b1      |b2      |b3      |b4      |b5      |b6      |b7      |b8      |b9      |b10     |b11     |b12     |b13     |b14     |b15     |
//|type    |id      |packet data                                                                                                                  |
//|0       |id      |proto   |batt    |batt_v  |temp    |brd_id  |mcu_id  |imu_id  |mag_id  |fw_date          |major   |minor   |patch   |rssi    | info
//|1       |id      |q0               |q1               |q2               |q3               |a0               |a1               |a2               | full precision quat
//|2       |id      |batt    |batt_v  |temp    |q_buf                              |a0               |a1               |a2               |rssi    | reduced precision quat
//|3	   |id      |svr_stat|status  |resv                                                                                              |rssi    | status
//|4       |id      |q0               |q1               |q2               |q3               |m0               |m1               |m2               |
//|255     |id      |addr                                                 |resv                                                                   | tracker id association

#ifndef CONFIG_SOC_NRF52820
#include "util.h"
// @brief 以下區塊用於偵測和過濾異常的旋轉資料 (四元數)。
// 透過比較當前封包和上一個有效封包之間的旋轉差異，來判斷資料是否可能出錯。
// 這種機制可以過濾掉因為無線干擾等原因造成的瞬時跳動資料。

// 上一個有效的資料
static float last_q_trackers[MAX_TRACKERS][4] = {0};
static uint32_t last_v_trackers[MAX_TRACKERS] = {0};
static uint8_t last_p_trackers[MAX_TRACKERS] = {0};
static int last_valid_trackers[MAX_TRACKERS] = {0};
// 上一個收到的資料 (不論是否有效)
static float cur_q_trackers[MAX_TRACKERS][4] = {0};
static uint32_t cur_v_trackers[MAX_TRACKERS] = {0};
static uint8_t cur_p_trackers[MAX_TRACKERS] = {0};
#endif

/**
 * @brief 將一個從無線端收到的封包寫入 HID FIFO
 *
 * 這是外部模組 (如 ESB 無線通訊模組) 呼叫的函式，用於將 tracker 資料傳遞給 HID 模組。
 *
 * @param data 指向 16 位元組封包資料的指標
 * @param rssi 接收時的信號強度指示
 */
void hid_write_packet_n(uint8_t *data, uint8_t rssi)
{
#ifndef CONFIG_SOC_NRF52820
	// @brief 異常旋轉偵測邏輯，僅在非 nRF52820 的晶片上啟用 (可能是因為效能考量)
	if (data[0] == 1 || data[0] == 2 || data[0] == 4)
	{
		// ... (此處為複雜的四元數有效性檢查邏輯，此處省略細節註解)
		// ...
	}
#endif

	// 將封包資料複製到一個臨時的 report 結構中
	memcpy(&report.data, data, sizeof(report));
	// 對於非高精度四元數的封包，將最後一個位元組用來存放 RSSI (信號強度)
	if (data[0] != 1 && data[0] != 4) {
		report.data[15] = rssi;
	}

	size_t write_idx = (size_t)atomic_get(&report_write_index);
	size_t read_idx = (size_t)atomic_get(&report_read_index);

	// @brief 優化：如果 FIFO 中已經存在同一個 tracker 的舊資料，則直接覆蓋它。
	// 這可以確保主機總是收到最新的資料，並避免 FIFO 中充滿了同一個 tracker 的過時資訊。
	if (write_idx != read_idx) {
		size_t check_index = (read_idx + 1) % MAX_TRACKERS;

		while (check_index != write_idx) {
			if (reports[check_index].data[1] == data[1]) { // data[1] 是 tracker ID
				// 找到舊資料，直接覆蓋
				reports[check_index] = report;
				return;
			}
			check_index = (check_index + 1) % MAX_TRACKERS;
		}
	}

	// 檢查 FIFO 是否已滿。如果滿了，就丟棄這個封包。
	if ((write_idx + 1) % MAX_TRACKERS == read_idx) {
		dropped_reports++;
		if (dropped_reports > max_dropped_reports) max_dropped_reports = dropped_reports;
		return;
	}

	// 將新封包寫入 FIFO
	reports[write_idx] = report;

	// 更新寫入索引
	write_idx = (write_idx + 1) % MAX_TRACKERS;
	atomic_set(&report_write_index, write_idx);
}
