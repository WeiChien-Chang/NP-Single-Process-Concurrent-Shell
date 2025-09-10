#include<iostream>
#include<cstring>
#include<sstream>
#include<string>
#include<vector>
#include<cctype>
#include<map>
#include<pwd.h>
#include<errno.h>
#include<fcntl.h>
#include<signal.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/wait.h>
#include <arpa/inet.h>

#define MAX_COUNT 1001
#define	MAXLINE	15000

#define cmdTime first
#define processState second

int server_fd;

using namespace std;
struct processState{
	int numberedPipe[2] = {-1, -1};
	vector<pid_t> pids;
};
map<int, processState> numberedPipes;
int state, counter;
int NPOUT = STDOUT_FILENO;
int NPERR = STDERR_FILENO;

void signalHandler(int signo)
{
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

void ParseInput(vector<vector<string>> &dst, const string &input)
{
    /* 第一階段：依照 管線符號 | 以及 輸出重導 > 把一整行拆成數個片段 */
    // ls -al | grep cpp > out.txt -> ["ls -al", "grep cpp", "> out.txt"]
    vector<string> segments;
    int begin = 0;
    for(int i = 0; i < (int)input.size(); ++i)
    {
        if(input[i] == '|')
        {
            // 遇到 | -> 把 begin 到 cur 之間的子字串存進 segments
            segments.push_back(input.substr(begin, i - begin));
            begin = i + 1;
        }
        else if(input[i] == '>')
        {
            // push 目前指令
            segments.push_back(input.substr(begin, i - begin));
            // 將 > 與後面的字串視為一個 segment
            segments.push_back(">" + input.substr(i + 1));
            begin = input.size();
            break;
        }
    }
    // 剩餘的字串，加入 segments
    if(begin < (int)input.size())
        segments.push_back(input.substr(begin));

    /* 第二階段：把每個 segment 切成 argument */
    dst.clear();
    dst.resize(segments.size());

    // 迴圈處理每一段 segment
    // "ls -al" -> {"ls", "-al"}
    // ">output.txt" -> {">", "output.txt"}
    for (size_t index = 0; index < segments.size(); ++index) 
    {
        const string &seg = segments[index];
        vector<string> &args = dst[index];
        string arg;
        bool inToken = false;
        size_t startPos = 0;

        if (!seg.empty() && seg[0] == '>') 
        {
            args.push_back(">");
            startPos = 1;
        }

        for (size_t i = startPos; i < seg.size(); ++i) 
        {
            char c = seg[i];
            if (isspace(c))     // 用空白字元來切分 token
            {
                if (inToken) 
                {
                    args.push_back(arg);
                    arg.clear();
                    inToken = false;
                }
            }
            else 
            {
                inToken = true;
                arg.push_back(c);
            }
        }

        if (!arg.empty())
            args.push_back(arg);
    }
}

// execvp( const char *file, char *const argv[] ) 時，第二個參數必須是以 NULL 結尾的 char* 陣列
// C++ 的 vector<string> 與 string 物件並不符合這種 C 介面的要求，所以必須轉成裸指標陣列
char **string_to_char(vector<string> &input)    //vector<string> to char[][]
{
	int argc = input.size();
	char **argv = new char*[argc+1]; 
	for(int i = 0; i < argc; i++){
		argv[i] = new char[input[i].size() + 1];    // +1 放 '\0'
		strcpy(argv[i], input[i].c_str());
	}
	argv[argc] = nullptr;
	return argv;
}

bool buildInCommand(vector<vector<string>> &cmd)
{
    if (cmd[0][0] == "printenv")
    {
        // printenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        // getenv() 接收的是 const char*，而 cmd[0][1] 是 string，透過c_str() 轉換，將 string 轉成 C-style 字串，方便給 C 函數使用
        if (cmd[0].size() < 2 || getenv(cmd[0][1].c_str()) == NULL);
            // do nothing
        else 
            cout << getenv(cmd[0][1].c_str()) << '\n';
        return true;
    }
    // int setenv(const char *name, const char *value, int overwrite);
    // name: 環境變數名稱 value: 環境變數的值 overwrite: 是否覆蓋原有的環境變數(1,0)
    else if (cmd[0][0] == "setenv")
    {
        // setenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else if (cmd[0].size() < 3)  
            setenv(cmd[0][1].c_str(), "", 1);
        else
            setenv(cmd[0][1].c_str(), cmd[0][2].c_str(), 1);
        return true;
    }
    else if (cmd[0][0] == "exit")
    {
        // You should handle the forked processes properly, or there might be zombie processes.
        // 根據spec敘述，因此在接收到exit指令時，就直接結束所有的子行程
        // -1：表示等所有子程序（不限哪一個）NULL：不需要知道結束狀態 WNOHANG：非阻塞模式，如果沒有子程序結束就立刻回傳 0
        while (waitpid(-1, nullptr, WNOHANG) > 0);  // 回收所有子程序
        exit(0);
    }

    else if (cmd[0][0] == "unsetenv")
    {
        // unsetenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else 
            unsetenv(cmd[0][1].c_str());
        return true;
    }

    return false;
}

void childProcess(int i, int numOrdPipe, int ordinaryPipe[], bool fileRedir, int &fd, vector<vector<string>> &cmd)
{
    /* 1. stdin */
    // 第一條指令 若沒有編號 pipe -> 保持預設 STDIN
    if (i != 0) 
        dup2( ordinaryPipe[(i - 1) * 2], STDIN_FILENO );    // 從上一條一般 pipe 的 讀端 接進來
    else if ( numberedPipes.find(counter) != numberedPipes.end() )
        dup2( numberedPipes[counter].numberedPipe[0], STDIN_FILENO );

    /* 2. stdout / stderr */
    if (i != numOrdPipe)    // 中間指令
    {
        dup2( ordinaryPipe[i * 2 + 1], STDOUT_FILENO );
    }
    else if (fileRedir)     // 最後一條且 >
    {
        fd = open( cmd.back()[1].c_str(), O_TRUNC | O_RDWR | O_CREAT, 0666 );
        dup2( fd, STDOUT_FILENO );
    }
    else    // 最後一條無redirection
    {
        dup2( NPOUT, STDOUT_FILENO );
        dup2( NPERR, STDERR_FILENO );
    }

    /* 3. 關閉不需要的 FD */
    for (int j = 0; j < (i + 1) * 2 && j < numOrdPipe * 2; ++j)
        close( ordinaryPipe[j] );
    for (auto &p : numberedPipes) 
    {
        close( p.second.numberedPipe[0] );
        close( p.second.numberedPipe[1] );
    }

    /* 4. execvp */
    // 透過輔助函式 string_to_char(cmd[i]) 產生 argv。
    char **argv = string_to_char( cmd[i] );
    if ( execvp(argv[0], argv) == -1 ) {
        cerr << "Unknown command: [" << argv[0] << "].\n";
        exit(1);
    }
    /* execvp 成功不會回到這；失敗已 exit */
}

void parentProcess( int i, int numOrdPipe, int ordinaryPipe[])
{
    /* 回收Zombie process */
    while ( waitpid(-1, &state, WNOHANG) > 0 );

    /* 關掉上一條一般 pipe */
    if (i != 0) {
        close( ordinaryPipe[(i - 1) * 2] );
        close( ordinaryPipe[(i - 1) * 2 + 1] );
    }

    /* Number pipe 到期處理 */
    if ( numberedPipes.find(counter) != numberedPipes.end() ) {
        close( numberedPipes[counter].numberedPipe[0] );
        close( numberedPipes[counter].numberedPipe[1] );
        // 逐一 waitpid 該 pipe 內記錄的子行程 PID，確保完全回收
        for (auto childpid : numberedPipes[counter].pids)
            waitpid(childpid, &state, 0);
        numberedPipes.erase(counter);
    }
}

void ExecuteCommand(vector<vector<string>> &cmd, int pipeTime)
{
    int numOrdinaryPipe = cmd.size() - 1, fd = -1;

    /* 檢查最後一段是否是 > */
    bool fileRedirection = cmd.back()[0] == ">";
    if (fileRedirection)
        numOrdinaryPipe--;

    /* 建立一般 pipe 陣列 */
    int ordinaryPipe[numOrdinaryPipe * 2];
    memset( ordinaryPipe, -1, sizeof(ordinaryPipe) );

    pid_t childpid;

    for (int i = 0; i < numOrdinaryPipe + 1; ++i) 
    {

        if (i < numOrdinaryPipe)          // 中間指令先開 pipe
            pipe( ordinaryPipe + i * 2 );

        if ( cmd[i].empty() ) {
            cerr << "Unknown command: [].\n";
            continue;
        }

        while ( waitpid(-1, &state, WNOHANG) > 0 ); // 清zombie process

        childpid = fork();

        if (childpid >= 0) /* fork succeeded */
        { 
            if (childpid == 0) /* fork() returns 0 to the child process */
            { 
                childProcess(i, numOrdinaryPipe, ordinaryPipe, fileRedirection, fd, cmd);
            } 
            else /* fork() returns new childpid to the parent process */
            { 
                parentProcess(i, numOrdinaryPipe, ordinaryPipe);
            }
        } 
        else    // fork failed
        { 
            --i;
            usleep(1000);
            continue;
        }
    }

    if (pipeTime == -1)
        waitpid(childpid, &state, 0);
    else
        numberedPipes[pipeTime].pids.push_back(childpid);

    while ( waitpid(-1, &state, WNOHANG) > 0 );

    if (fileRedirection)
        close(fd);

    /* 重設預設輸出 */
    NPOUT = STDOUT_FILENO;
    NPERR = STDERR_FILENO;
}

void handleNumberedPipes(string &line)
{
	int begin = 0, num = 0, pipeTime, cur ,newCur;
	
    for(cur = 0; cur < line.size(); cur++)
    {
        // 尋找 | 或 !
		if(line[cur] == '|' || line[cur] == '!')
        {
            // 連續讀取數字字元，把它們轉成整數 num，Ex: |123 -> num = 123
			for(newCur = cur + 1; newCur < line.size() && isdigit(line[newCur]); newCur++)
            {
				num *= 10;
				num += (int) line[newCur] - '0';
			}
			if(num == 0)
                continue;
			
            counter = (counter + 1) % MAX_COUNT;    // counter 每讀完一行就加 1
			pipeTime = ((counter + num) % MAX_COUNT);   // 未來第 num 行的位置

			if(numberedPipes.find(pipeTime) == numberedPipes.end())
				pipe(numberedPipes[pipeTime].numberedPipe);     // 建立真正的 pipe FD
		
			NPOUT = numberedPipes[pipeTime].numberedPipe[1];    // stdout 導向 pipe 寫端
			
            if(line[cur] == '!')
                NPERR = numberedPipes[pipeTime].numberedPipe[1];   // 若是 !N 連 stderr 也導向
			
            // 取出「目前找到 |N/!N 前」的子字串去執行
            vector<vector<string>> cmd;
			string tline = line.substr(begin, cur - begin);
			ParseInput(cmd, tline);
			ExecuteCommand(cmd, pipeTime);
			begin = newCur;
			cur = newCur - 1;
			num = 0;
		}
	}
	line = line.substr(begin);
}

void serviceClient(int client_fd)
{
    /* 1. 把 socket 綁到標準 I/O */
    dup2(client_fd, STDIN_FILENO);
    dup2(client_fd, STDOUT_FILENO);
    dup2(client_fd, STDERR_FILENO);
    close(client_fd);

    /* 2. 每個 client 都要有自己的一份狀態 */
	clearenv();
	setenv("PATH", "bin:.", 1);
	counter = 0;
	numberedPipes.clear();

    char buf[MAXLINE];
    string in;
    vector<vector<string>> cmd;
    int n;

    cout << "% " << flush;
    while ( (n = read(STDIN_FILENO, buf, MAXLINE)) > 0 )
    {
        for (int i = 0; i < n; ++i) {
            if (buf[i] == '\r')
                continue;
            if (buf[i] != '\n')
            { 
                in.push_back(buf[i]);
                continue; 
            }

            string line = in;
            in.clear();

            handleNumberedPipes(line); 
            ParseInput(cmd, line); 

            if (cmd.empty())
            { 
                cout << "% " << flush;
                continue;
            }

            counter = (counter + 1) % MAX_COUNT;
            int r = buildInCommand(cmd);
            if (r ==  1) 
            {
                cout << "% " << flush;
                continue;
            }
            if (r == -1)
                goto quit;      // exit

            ExecuteCommand(cmd, -1);
            cout << "% " << flush;
        }
    }
quit:
    exit(0);                             // child 結束
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // socket
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(atoi(argv[1]));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    // socket

	signal(SIGCHLD, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    // cmd = {{"printenv", "PATH"},{"|"},{"sort"}}
	string line;
    vector<vector<string>> cmd;

    while (true)
    {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        
        pid_t pid = fork();
        if (pid < 0) {             // fork 失敗
            perror("fork");
            close(client_fd);
            continue;
        }
        else if (pid == 0) {
            close(server_fd);      // Child process不需要 listening socket
            serviceClient(client_fd);
        }
        else {
            close(client_fd);      // 交給 child 處理後立即關閉
        }
    }

    return 0;
}