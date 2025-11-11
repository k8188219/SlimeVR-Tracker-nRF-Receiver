# SlimeNRF 接收器韌體中文說明

這份文件旨在幫助您了解此專案的結構、如何建置開發環境、編譯韌體，以及如何為新的開發板添加支援。

## 1. 如何建置開發環境

對於沒有嵌入式系統開發經驗的初學者，我們強烈建議使用 Visual Studio Code (VS Code) 搭配 Nordic Semiconductor 官方提供的 nRF Connect for VS Code 擴充功能。這個擴充功能會自動幫您安裝所有必要的工具鏈 (toolchain) 和軟體開發套件 (SDK)，大幅簡化環境設定的複雜度。

### 所需工具

1.  **[Git](https://git-scm.com/download/win):** 用於版本控制和下載專案原始碼。
2.  **[Visual Studio Code (VS Code)](https://code.visualstudio.com/download):** 主要的程式碼編輯器。
3.  **[nRF Connect for VS Code](https://marketplace.visualstudio.com/items?itemName=nordic-semiconductor.nrf-connect-vs-code):** VS Code 的擴充功能，整合了 nRF Connect SDK 和 Zephyr RTOS 的開發環境。

### 安裝步驟

1.  **安裝 Git 和 VS Code:**
    *   前往上面的連結下載並安裝 Git 和 VS Code。如果您已經安裝過，可以跳過此步驟。

2.  **安裝 nRF Connect for VS Code 擴充功能:**
    *   打開 VS Code。
    *   點擊左側的擴充功能圖示 (Extensions)。
    *   在搜尋框中輸入 `nRF Connect for VS Code`。
    *   找到由 Nordic Semiconductor 開發的擴充功能，點擊 "Install"。

3.  **安裝 nRF Connect SDK 和 Toolchain:**
    *   安裝完擴充功能後，VS Code 左側會出現一個新的 nRF Connect 圖示，點擊它。
    *   在 nRF Connect 的面板中，找到 "SDK & Toolchain" 的部分。
    *   點擊 "Install SDK"，選擇 "nRF Connect SDK"，然後選擇 **`v3.1.0`** 版本進行安裝。擴充功能會將 SDK 安裝到預設路徑。
    *   安裝完 SDK 後，點擊 "Install Toolchain"，同樣選擇 **`v3.1.0`** 版本。

4.  **下載 SlimeNRF 接收器原始碼:**
    *   打開您的終端機 (Command Prompt, Terminal, or Git Bash)。
    *   使用 `cd` 指令切換到您想要存放專案的資料夾。
    *   執行以下指令來下載原始碼，這個指令會同時下載專案本身以及所有相依的子模組 (submodules):
        ```bash
        git clone --recurse-submodules https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver.git
        ```
    *   **重要:** 建議將專案存放在不包含中文、空白或特殊字元的路徑下，以避免後續編譯時發生問題。

5.  **在 VS Code 中打開專案:**
    *   在 VS Code 中，選擇 "File" > "Open Folder..."，然後選擇您剛剛下載的 `SlimeVR-Tracker-nRF-Receiver` 資料夾。
    *   當 VS Code 偵測到這是一個 nRF Connect 專案時，它可能會詢問您是否要信任此工作區，請選擇 "Yes, I trust the authors"。

至此，您的開發環境已經設定完成！

## 2. 如何執行 Build (編譯韌體)

在 VS Code 中編譯韌體非常直觀。

1.  **打開 nRF Connect 面板:**
    *   點擊 VS Code 左側的 nRF Connect 圖示。

2.  **新增一個建置設定 (Build Configuration):**
    *   在 "APPLICATIONS" 區塊，點擊 `+ Add Application`，然後選擇您剛剛打開的專案路徑。
    *   VS Code 會自動偵測到這是一個 Zephyr 專案。
    *   接著，點擊 `+ Add build configuration`。

3.  **選擇開發板和設定:**
    *   在跳出的視窗中，您需要選擇您要為哪一塊開發板編譯韌體。
    *   在 "Board" 欄位，您可以從下拉選單中選擇。舉例來說，如果您使用的是官方的 nRF52840 Dongle，您可以選擇 `nrf52840dongle_nrf52840`。
    *   確認 "SDK" 和 "Toolchain" 都已正確選擇 (應為 `nrfconnect` 和 `ncs v3.1.0`)。
    *   點擊 "Build Configuration" 按鈕。

4.  **開始編譯:**
    *   此時，在 "APPLICATIONS" 區塊下會出現您剛剛設定的建置目標。
    *   點擊目標旁邊的 "Build" 按鈕 (一個齒輪圖示)。
    *   VS Code 會自動開始編譯。您可以在下方的 "OUTPUT" 視窗中看到詳細的編譯過程和訊息。

5.  **尋找編譯好的韌體:**
    *   如果編譯成功，您可以在專案資料夾下的 `build/zephyr` 目錄中找到編譯好的韌體檔案。
    *   最常用的檔案有兩種：
        *   `.hex`: 適用於透過 J-Link 等除錯器燒錄。
        *   `.uf2`: 適用於具有 UF2 Bootloader 的開發板，可以像隨身碟一樣拖曳檔案來更新韌體。

## 3. 如何加入新的開發版

如果您自己設計了一塊基於 nRF52840 的開發板，並希望在這個專案中使用，您需要為它建立一組「開發板支援檔案 (Board Support Files)」。這能讓 Zephyr 編譯系統知道您的硬體配置。

最簡單的方法是複製一個現有的開發板設定，然後根據您的設計進行修改。我們以 `nrf52840dongle_nrf52840` 為例。

### 步驟

1.  **複製現有開發板資料夾:**
    *   在 `boards` 資料夾中，您可以看到許多現有的開發板設定。
    *   將 `nrf52840dongle_nrf52840.conf` 和 `nrf52840dongle_nrf52840.overlay` 這兩個檔案複製一份，並重新命名。例如，如果您的開發板叫做 `my_board`，您可以將它們命名為 `my_board_nrf52840.conf` 和 `my_board_nrf52840.overlay`。

2.  **理解並修改關鍵檔案:**

    *   **`.conf` 檔案 (例如 `my_board_nrf52840.conf`):**
        *   **功能:** 這個檔案用來設定 Zephyr 核心和各個模組的功能開關。它基於 Kconfig 系統。
        *   **說明:** 檔案中的每一行 `CONFIG_XXX=y` 或 `CONFIG_YYY=1024` 都代表啟用某個功能或設定某個參數。例如，`CONFIG_USB_DEVICE_STACK=y` 表示啟用 USB 功能。
        *   **修改:** 通常，您可以先保持與 `nrf52840dongle_nrf52840.conf` 一致的設定。除非您的硬體有特殊需求 (例如，使用不同的序列埠或啟用特定的感測器介面)，否則不需要大改。

    *   **`.overlay` 檔案 (例如 `my_board_nrf52840.overlay`):**
        *   **功能:** 這是 Device Tree (設備樹) 的疊加檔案，用來描述您的硬體細節，例如哪個 GPIO 接了 LED、哪個 I2C 控制器接了感測器等。這是您最需要修改的檔案。
        *   **說明:** Zephyr 使用設備樹來告訴作業系統硬體的連接方式。`.overlay` 檔案允許您在不修改 Zephyr 內建設備樹檔案的情況下，客製化您的硬體設定。
        *   **修改範例:**
            ```dts
            // 覆寫預設的 LED 設定
            / {
                aliases {
                    led0 = &led_0;
                };
            };

            // 定義一個新的 LED，連接到 P1.09
            &gpiote {
                status = "okay";
            };

            leds {
                compatible = "gpio-leds";
                led_0: led_0 {
                    gpios = <&gpio1 9 GPIO_ACTIVE_HIGH>; // 將 led_0 指向 GPIO Port 1 的第 9 支腳
                    label = "Green LED 0";
                };
            };
            ```
            您需要根據您自己電路板的設計，修改 GPIO 的腳位、I2C/SPI 的設定，或是其他周邊裝置的定義。

3.  **在 VS Code 中選擇新的開發板設定:**
    *   完成修改後，回到 VS Code。
    *   在建立 Build Configuration 時，您可能無法直接在下拉選單中看到您的新板子。
    *   但您可以在 `prj.conf` 檔案中直接指定您的 `.overlay` 和 `.conf` 檔案，或是在 `west build` 指令中透過參數傳遞。
    *   一個更符合 Zephyr 框架的做法是將您的板子建立成一個完整的自訂開發板 (Custom Board)，這需要建立更完整的資料夾結構 (包含 `board.yml`, `Kconfig` 等)，您可以參考 `boards/etee/etee_dongle_uf2` 的結構來建立。

對於初學者，建議先從修改現有的 `.conf` 和 `.overlay` 檔案開始，這能最快地讓您的自訂硬體跑起來。

## 4. 每個檔案和每個資料夾是什麼功能

以下是這個專案主要檔案和資料夾的功能概覽：

*   **`CMakeLists.txt`:**
    *   整個專案的 CMake 建置腳本。它會告訴建置系統如何編譯和連結原始碼，並引入 Zephyr 的建置框架。

*   **`Kconfig`:**
    *   定義了專案層級的可配置選項。您可以在這裡新增自己的 `CONFIG_` 選項，並在 `prj.conf` 中進行設定。

*   **`prj.conf`:**
    *   這是專案的主要設定檔，作用於所有開發板。它會設定預設的系統功能、通訊協定、日誌等級等。針對特定開發板的設定，則會寫在 `boards/` 下對應的 `.conf` 檔案中。

*   **`west.yml`:**
    *   West (Zephyr's meta-tool) 的設定檔。它定義了這個專案所依賴的 nRF Connect SDK 和其他外部 Zephyr 模組。當您執行 `west update` 時，West 會根據這個檔案來下載和更新相依套件。

*   **`boards/`:**
    *   存放所有「特定開發板」的設定檔。每個 `.conf` 和 `.overlay` 檔都對應到一款硬體。這讓同一套程式碼可以輕鬆地應用於不同的硬體上。

*   **`src/`:**
    *   所有核心應用程式邏輯的原始碼都存放在這裡。
    *   `main.c`: 程式的進入點 (entry point)。包含了主要的初始化流程和主迴圈。
    *   `hid.c`/`hid.h`: 處理 USB HID (Human Interface Device) 通訊的相關邏輯。接收器透過 HID 與電腦上的 SlimeVR Server 溝通。
    *   `connection/`: 處理與 SlimeVR Tracker 之間無線通訊的邏輯，主要是基於 Nordic 的 ESB (Enhanced ShockBurst) 協定。
    *   `system/`: 包含系統層級的功能，例如 LED 狀態指示、系統狀態管理等。
    *   `globals.h`/`globals.c`: 定義和實作全域變數和狀態。
    *   `retained.c`/`retained.h`: 處理需要在重啟後仍然保留的資料 (例如配對資訊)，通常會儲存在 Flash 或特定的 RAM 區塊。

*   **`pm_static_*.yml`:**
    *   Partition Manager 的靜態設定檔。用於定義 Flash 記憶體的分割區塊，例如 Bootloader、應用程式、儲存區等的大小和位置。
