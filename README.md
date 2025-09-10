# NP‑RWG (Remote Working Ground) Server

以 C/C++ 實作的多人遠端shell伺服器。

- **`np_simple.cpp`**：每個連線 `fork()` 出獨立 child（Concurrent connection-oriented、彼此完全隔離）。
- **`np_single_proc.cpp`**：Single-process concurrent + `select()` I/O 多工，支援 `who/tell/yell/name` 與跨使用者的 **User Pipe**（`>n` / `<n`）。

把shell搬到網路上後，除了要正確串接 `fork/exec/pipe/dup2`，還要兼顧：
- Concurrent 模型（`fork-per-client` vs `select` single-process）
- 每個使用者的獨立環境（PATH、numbered pipe 倒數、暫存訊息）
- 各種極端與錯誤案例（未知指令、離線、User Pipe 不存在/重複、系統呼叫失敗）

---

## 功能
- **Pipe**：
  - Ordinary pipe：`cmd1 | cmd2`
  - Numbered pipe：`cmd |N`、`cmd !N`（N 行後把 STDOUT／STDERR 接到當時的 STDIN）
  - 檔案重導向：`cmd > file`
- **環境變數**：`printenv / setenv / unsetenv`（每個使用者獨立 PATH；預設 `PATH=bin:.`）
- **連線管理**：`accept` 多 client、`SO_REUSEADDR`、`SIGCHLD` 回收 zombie
- **聊天室（`np_single_proc`）**：
  - `who`、`name <new>`、`tell <id> <msg>`、`yell <msg>`
  - **User Pipe**：`>n` 把我的 STDOUT 丟給使用者 n；`<n` 從使用者 n 接收到我的 STDIN
    - 成功會廣播完整指令列；錯誤（不存在/重複/使用者不在）只提示，不阻塞後續命令

---

## 架構
### 1) Concurrent 模型
- **`np_simple`：fork‑per‑client**
  - 新連線就 `fork()`：child 進入互動式 shell 迴圈；parent 繼續 `accept()`。
  - 優點：簡單直觀、連線天然隔離；缺點：程序成本較高(每次都要fork()新的process)。
- **`np_single_proc`：single process `select()`**
  - 用 `FD_SET` 追蹤所有 socket，迴圈中 `select()` 等待可讀 client，再「一次處理一整行」。
  - 優點：資源輕、共享狀態易控（user 列表、User Pipe 表、每人環境）；缺點：必須小心載入/回存每位使用者的上下文。

### 2) 指令解析與執行（兩階段解析）
1. 先按 `|` 與 `>` 切成 **segments**。
   - 例：`ls -al | grep cpp > out.txt` → `["ls -al", "grep cpp", "> out.txt"]`
2. 再把每個 segment 切成 **tokens**。
   - `"> out.txt"` → `[">", "out.txt"]`

**執行策略（stdin/stdout 佈線優先序）**
- **stdin 來源**：User Pipe ▶ 到期的 Numbered Pipe ▶ 前一段 ordinary pipe ▶ 預設 STDIN
- **stdout 去向**：User Pipe ▶ 後一段 ordinary pipe ▶ 檔案重導向 ▶ 預設（socket）
- `execvp()` 失敗 → 印 `Unknown command: [xxx]`，結束該 child。

---

## Numbered Pipe：用「環形倒數」記 N 行後的輸出

`|N` / `!N` 會把「本行的輸出」存到一個「在 N 行後要讀」的Pipe；到了那一行，系統自動把該管線接到該行指令的 **STDIN**。

**為什麼叫「環形倒數（ring indexing）」？**  
使用固定大小（例如 `MAX_COUNT = 1001`）的環形索引表：
- 每讀完一行就把 `counter = (counter + 1) % MAX_COUNT`。
- 若本行有 `|N`（或 `!N`），計算「到期行」：`pipeTime = (counter + N) % MAX_COUNT`。
- 把本行 **STDOUT**（`!N` 時連同 **STDERR**）導向 `pipes[pipeTime].write_end`。
- 當未來執行到「到期行」（也就是索引等於 `pipeTime`）時，自動把 `pipes[pipeTime].read_end` 接到該行的 **STDIN**，然後關閉並回收。

**範例**
```
L1: ls |3        # 輸出存到 pipe[(1+3)%MAX] → 等到 L4 讀
L2: echo A       # 正常
L3: echo B       # 正常
L4: cat          # 這行的 STDIN 自動接到 L1 的輸出
```

## User Pipe：跨使用者的 STDOUT/STDIN
資料結構：`map<pair<int,int>, processState>`，key 是 `(srcID, dstID)`。

**規則**
- 送出端 `>n`：
  - 若 `(me, n)` 已存在 → `*** Error: the pipe #me->#n already exists. ***`
  - 否則建立匿名 pipe，**廣播**：`*** <me> just piped '<原始指令>' to <n> ***`
- 接收端 `<n`：
  - 若 `(n, me)` 不存在 → `*** Error: the pipe #n->#me does not exist yet. ***`
  - 否則使用其讀端，**廣播**：`*** <me> just received from <n> by '<原始指令>' ***`
- 錯誤時**不阻塞**：
  - 無效輸入 → 把 STDIN 指到 `/dev/null`（等同 EOF）
  - 無效輸出 → 把 STDOUT 指到 `/dev/null`（直接丟棄）
- 使用者離線：關閉並清掉所有與其相關的 `(src,dst)`。

**範例**
```
#1: cat a.txt >2      # 建立 (#1->#2) 並廣播
#2: cat <1            # 讀取 (#1->#2) 並廣播，之後此 pipe 會被關閉與回收
```

---

## 每使用者的獨立上下文（`np_single_proc`）
- `usersData[id]`：`name / ip:port / env(PATH 等) / numberedPipes / counter`
- 在 `select` 迴圈：
  1) 開始處理某位使用者 → 載入他的 `env/counter/numberedPipes` 到全域工作區
  2) 執行完 → 把更新後的狀態**回存**到 `usersData[id]`
- 為了在廣播/私訊時切換 I/O，我另外備份了 `STDIN/STDOUT/STDERR`（`BACKUP_*`），發完訊息再還原。

---

## 錯誤處理與邊界案例
- 系統呼叫：`fork/pipe/select/read` 失敗 → 重試或明確回報
- `Unknown command: [xxx].`（`execvp` 失敗）
- User Pipe 錯誤訊息：不存在、重複、對離線使用者等
- Zombie 清理：`SIGCHLD` handler + 迴圈中 `waitpid(..., WNOHANG)`

---
