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

#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	/*
	 * @brief 程式主進入點
	 *
	 * Zephyr RTOS 會在核心初始化完成後呼叫 main() 函式。
	 * 在這個專案中，大部分的初始化和主要邏輯都透過 Zephyr 的
	 * application-specific system initialization 功能來完成，
	 * 這意味著許多模組會在 main() 被呼叫之前就自行初始化。
	 *
	 * 詳情可參考 Zephyr 的文件：
	 * https://docs.zephyrproject.org/latest/application/index.html#application-specific-system-initialization
	 */

	// 這裡的程式碼會在所有自動初始化的模組都完成後執行。
	// 目前它只做了一件事：設定 LED 的狀態，以表示系統正在正常運作。
	set_led(SYS_LED_PATTERN_ACTIVE_PERSIST, SYS_LED_PRIORITY_SYSTEM);

	// main 函式在 Zephyr 中通常不會返回。如果返回 0，系統會進入 idle 狀態。
	return 0;
}