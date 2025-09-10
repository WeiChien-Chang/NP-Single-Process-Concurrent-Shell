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

using namespace std;
struct processState 
{
    int numberedPipe[2] = {-1, -1};
    vector<pid_t> pids;
};

enum class ParseState 
{
    Normal,
    AfterYell,
    AfterTellCmd,
    AfterTellTarget
};

map<pair<int,int>,processState> userPipes;

struct usersInfo {
    char ip4[INET_ADDRSTRLEN];
    int port;
    string name = "(no name)";
    string cmd;
    map<int, processState> numberedPipes;
    map<string, string> env;
    int counter;
};

map<int, processState> numberedPipes;

ParseState parseState = ParseState::Normal;
int current_stdin = STDIN_FILENO;
int current_stdout = STDOUT_FILENO;
int current_stderr = STDERR_FILENO;
constexpr int BACKUP_STDIN  = 4;
constexpr int BACKUP_STDOUT = 5;
constexpr int BACKUP_STDERR = 6;

int client[FD_SETSIZE];
int waitStatus = 0;
int counter = 0;
int numUsers = 0;

map<int, usersInfo> usersData;

void signalHandler(int signo)
{
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

void who(int userId) 
{
    cout << "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
    for (const auto& [id, info] : usersData) 
    {
        cout << id << '\t' << info.name << '\t' << info.ip4 << ':' << info.port;
        if (id == userId)
            cout << "\t<-me";
        cout << '\n';
    }
}

void tell(int senderId, int receiverId, string msg)
{
    if (client[receiverId] == -1) 
    {
        cerr << "*** Error: user #" << receiverId << " does not exist yet. ***\n";
        return;
    }
    dup2(client[receiverId], STDIN_FILENO);
    dup2(client[receiverId], STDOUT_FILENO);
    dup2(client[receiverId], STDERR_FILENO);
    cout << "*** " << usersData[senderId].name << " told you ***: " << msg << '\n';
    dup2(client[senderId], STDIN_FILENO);
    dup2(client[senderId], STDOUT_FILENO);
    dup2(client[senderId], STDERR_FILENO);
}

void yell(int userId, string msg) 
{
    for (const auto& [id, info] : usersData) 
    {
        dup2(client[id], STDIN_FILENO);
        dup2(client[id], STDOUT_FILENO);
        dup2(client[id], STDERR_FILENO);
        cout << "*** " << usersData[userId].name << " yelled ***: " << msg << '\n';
    }
    dup2(client[userId], STDIN_FILENO);
    dup2(client[userId], STDOUT_FILENO);
    dup2(client[userId], STDERR_FILENO);
}

void name(int userId, string newName) 
{
    for (const auto& [id, info] : usersData) 
    {
        if (info.name == newName) 
        {
            cerr << "*** User '" << newName << "' already exists. ***\n";
            return;
        }
    }

    usersData[userId].name = newName;
    for (const auto& [id, info] : usersData) 
    {
        dup2(client[id], STDIN_FILENO);
        dup2(client[id], STDOUT_FILENO);
        dup2(client[id], STDERR_FILENO);
        cout << "*** User from " << usersData[userId].ip4 << ":" << usersData[userId].port << " is named '" << newName << "'. ***\n";
    }
    dup2(client[userId], STDIN_FILENO);
    dup2(client[userId], STDOUT_FILENO);
    dup2(client[userId], STDERR_FILENO);
}

void ParseInput(std::vector<std::vector<std::string>>& dst, const std::string& input)
{
    /* 0. 設計的三個BuildInFunction：yell / tell / name 指令有別於一般輸入，要進行特殊處理 */
    auto starts_with = [](const std::string& line, const std::string& word)->bool
    {
        return line.rfind(word + " ", 0) == 0;      // 
    };

    if (starts_with(input, "yell") || starts_with(input, "tell") || starts_with(input, "name"))
    {
        // 直接把整行當成唯一segment ─ 不處理pipe/redirection
        dst.clear();
        dst.resize(1);
        std::vector<std::string>& args = dst[0];

        std::string arg;
        bool inTok = false;
        for (char c : input) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (inTok) {
                    args.push_back(arg);
                    arg.clear();
                    inTok = false;
                }
            } else {
                inTok = true;
                arg.push_back(c);
            }
        }
        if (!arg.empty()) args.push_back(arg);

        // 把 yell / tell 的剩餘字串合併成一個參數
        if (!args.empty() && (args[0] == "tell" || args[0] == "yell")) {
            std::size_t msg_start = (args[0] == "tell") ? 2 : 1;
            if (args.size() > msg_start) {
                std::string msg;
                for (std::size_t j = msg_start; j < args.size(); ++j) {
                    if (j != msg_start) msg += " ";
                    msg += args[j];
                }
                args.resize(msg_start);
                args.push_back(msg);
            }
        }
        return;
    }

    /* 1.1 一般輸入：依 | / > 先切 segment */
    std::vector<std::string> segments;
    int begin = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '|') {
            segments.push_back(input.substr(begin, i - begin));
            begin = i + 1;
        }
        else if (input[i] == '>') 
        {
            if (i + 1 < (int)input.size() && std::isdigit(input[i + 1]))
                continue;
            segments.push_back(input.substr(begin, i - begin));
            segments.push_back(">" + input.substr(i + 1));
            begin = input.size();
            break;
        }
    }
    if (begin < static_cast<int>(input.size()))
        segments.push_back(input.substr(begin));

    /* 1.2 把每段切成 token */
    dst.clear();
    dst.resize(segments.size());

    for (std::size_t idx = 0; idx < segments.size(); ++idx) {
        const std::string& seg = segments[idx];
        std::vector<std::string>& args = dst[idx];

        std::string tok;
        bool inTok = false;
        std::size_t startPos = 0;

        if (!seg.empty() && seg[0] == '>') {
            args.push_back(">");
            startPos = 1;
        }

        for (std::size_t i = startPos; i < seg.size(); ++i) {
            char c = seg[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (inTok) {
                    args.push_back(tok);
                    tok.clear();
                    inTok = false;
                }
            } else {
                inTok = true;
                tok.push_back(c);
            }
        }
        if (!tok.empty()) args.push_back(tok);
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

int buildInCommand(vector<vector<string>> &cmd, int userId)
{
    if (cmd.empty() || cmd[0].empty())
        return 1;
    if (cmd[0][0] == "printenv")
    {
        // printenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        // getenv() 接收的是 const char*，而 cmd[0][1] 是 string，透過c_str() 轉換，將 string 轉成 C-style 字串，方便給 C 函數使用
        if (cmd[0].size() < 2 || getenv(cmd[0][1].c_str()) == NULL);
            // do nothing
        else 
            cout << getenv(cmd[0][1].c_str()) << '\n';
        return 1;
    }
    // int setenv(const char *name, const char *value, int overwrite);
    // name: 環境變數名稱 value: 環境變數的值 overwrite: 是否覆蓋原有的環境變數(1,0)
    else if (cmd[0][0] == "setenv")
    {
        // setenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else if (cmd[0].size() < 3)  
        {
            usersData[userId].env[cmd[0][1]] = "";
            setenv(cmd[0][1].c_str(), "", 1);
        }
        else
        {
            usersData[userId].env[cmd[0][1]] = cmd[0][2];
            setenv(cmd[0][1].c_str(), cmd[0][2].c_str(), 1);
        }
        return 1;
    }
    else if (cmd[0][0] == "exit")
    {
        return -1;
    }

    else if (cmd[0][0] == "unsetenv")
    {
        // unsetenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else
        {
            usersData[userId].env.erase(cmd[0][1]);
            unsetenv(cmd[0][1].c_str());
        }
        return 1;
    }

    else if (cmd[0][0] == "who")
    {
        who(userId);
        return 1;
    }

    else if (cmd[0][0] == "tell")
    {
        int receiverId = 0;
        if (cmd[0].size() < 2)
        {
            cerr << "tell [id] [msg]\n";
            return 1;
        }
        sscanf(cmd[0][1].c_str(), "%d", &receiverId);
        if (cmd[0].size() < 3)
        {
            cerr << "tell [id] [msg]\n";
            return 1;
        }
        tell(userId, receiverId, cmd[0][2]);
        return 1;
    }

    else if (cmd[0][0] == "yell")
    {
        if (cmd[0].size() < 2)
        {
            cerr << "yell [msg]\n";
            return 1;
        }
        yell(userId, cmd[0][1]);
        return 1;
    }

    else if (cmd[0][0] == "name")
    {
        if (cmd[0].size() < 2)
        {
            cerr << "name [new name]\n";
            return 1;
        }
        name(userId, cmd[0][1]);
        return 1;
    }

    return 0;
}

inline void safeClose(int fd) 
{
    if (fd >= 0)
        close(fd);
}

void closeAllUnusedFDs(int *ordinaryPipe, int pipeCount, int currentCmdIndex) 
{
    for (int j = 0; j < (currentCmdIndex + 1) * 2 && j < pipeCount * 2; ++j)
        safeClose(ordinaryPipe[j]);

    for (auto &user : usersData)
    {
        safeClose(client[user.first]);
        for (auto &proc : user.second.numberedPipes) 
        {
            safeClose(proc.second.numberedPipe[0]);
            safeClose(proc.second.numberedPipe[1]);
        }
    }

    for (auto &pipe : userPipes) 
    {
        safeClose(pipe.second.numberedPipe[0]);
        safeClose(pipe.second.numberedPipe[1]);
    }

    safeClose(BACKUP_STDIN);
    safeClose(BACKUP_STDOUT);
    safeClose(BACKUP_STDERR);

    for (int k=0; k<FD_SETSIZE; ++k)
        safeClose(client[k]);
}

void childProcess(int i,
                int numOrdinaryPipe,
                int ordinaryPipe[],
                bool fileRedir,
                int &fd,
                int userId,
                vector<vector<string>> &cmd,
                bool hasUserPipeInput, bool hasUserPipeOutput,
                int UserPipeSrcId, int UserPipeDstId,
                bool userPipeExists)
{
    bool usedUserPipeIn = false, usedUserPipeOut = false;

    if(hasUserPipeInput && UserPipeSrcId != 0)
    {
        if(client[UserPipeSrcId] == -1)
        {
            dup2(open("/dev/null", O_RDONLY, 0), STDIN_FILENO);     // 錯誤處理 -> 開個空stdin避免阻塞
        }
        else if(userPipes.find({UserPipeSrcId, userId}) == userPipes.end())
        {
            dup2(open("/dev/null", O_RDONLY, 0), STDIN_FILENO);     // 無效的 user pipe，導向 null
        }
        else
        {
            usedUserPipeIn = true;
            dup2(userPipes[{UserPipeSrcId, userId}].numberedPipe[0], STDIN_FILENO);
        }
    }
    if(hasUserPipeOutput && UserPipeDstId != 0)
    {
        if(client[UserPipeDstId] == -1)
        {
            dup2(open("/dev/null", O_RDWR, 0), STDOUT_FILENO);
        }
        else if(userPipeExists)
        {
            dup2(open("/dev/null", O_RDWR, 0), STDOUT_FILENO);
        }
        else
        {
            usedUserPipeOut = true;
            dup2(userPipes[{userId, UserPipeDstId}].numberedPipe[1], STDOUT_FILENO);
        }
    }
    if (!usedUserPipeIn)
    {
        // 第一條指令 若沒有Number pipe -> 保持預設 STDIN
        if (i != 0) 
            dup2( ordinaryPipe[(i - 1) * 2], STDIN_FILENO );    // 從上一條Ordinary pipe 的 讀端 接進來
        else if ( numberedPipes.find(counter) != numberedPipes.end() )
            dup2( numberedPipes[counter].numberedPipe[0], STDIN_FILENO );
    }
    if (!usedUserPipeOut)
    {
        if (i != numOrdinaryPipe)    // 中間指令
        {
            dup2( ordinaryPipe[i * 2 + 1], STDOUT_FILENO );
        }
        else if (fileRedir)     // 最後一條且 >
        {
            fd = open( cmd.back()[1].c_str(), O_TRUNC | O_RDWR | O_CREAT, 0777);
            dup2( fd, STDOUT_FILENO );
        }
        else    // 最後一條無redirection
        {
            dup2( current_stdout, STDOUT_FILENO );
            dup2( current_stderr, STDERR_FILENO );
        }
    }

    closeAllUnusedFDs(ordinaryPipe, numOrdinaryPipe, i);

    // 透過輔助函式 string_to_char(cmd[i]) 產生 argv。
    char **argv = string_to_char(cmd[i]);
    if ( execvp(argv[0], argv) == -1 ) {
        cerr << "Unknown command: [" << argv[0] << "].\n";
        exit(1);
    }
    exit(0);
}

void parentProcess(int i,
                int numOrdPipe,
                int ordinaryPipe[],
                bool shouldRecvUP,
                int upSrc,
                int userId)
{
    /* 回收Zombie process */
    while ( waitpid(-1, &waitStatus, WNOHANG) > 0 );

    /* 關掉上一條一般 pipe */
    if (i != 0) {
        close( ordinaryPipe[(i - 1) * 2] );
        close( ordinaryPipe[(i - 1) * 2 + 1] );
    }

    /* Number pipe 到期處理 */
    if ( numberedPipes.find(counter) != numberedPipes.end() ) 
    {
        close( numberedPipes[counter].numberedPipe[0] );
        close( numberedPipes[counter].numberedPipe[1] );
        // 逐一 waitpid 該 pipe 內記錄的子行程 PID，確保完全回收
        for (auto childpid : numberedPipes[counter].pids)
            waitpid(childpid, &waitStatus, 0);
        numberedPipes.erase(counter);
    }
    if(shouldRecvUP)
    {
        close(userPipes[{upSrc, userId}].numberedPipe[0]);
        close(userPipes[{upSrc, userId}].numberedPipe[1]);

        for(auto pid : userPipes[{upSrc, userId}].pids)
        {
            waitpid(pid, &waitStatus, 0);
        }
        userPipes.erase({upSrc, userId});
    }
}

void ExecuteCommand(vector<vector<string>> &cmd, int pipeTime, string &line, int userId)
{
    int numOrdinaryPipe = cmd.size() - 1, fd = -1;
    int UserPipeSrcId = 0, UserPipeDstId = 0;
    bool shouldReceiveUserPipe = false, shouldSendUserPipe = false;
    /* 檢查最後一段是否是 > */
    bool fileRedirection = cmd.back()[0] == ">";
    if (fileRedirection)
        numOrdinaryPipe--;

    /* 建立Ordinary pipe 陣列 */
    int ordinaryPipe[numOrdinaryPipe * 2];
    memset( ordinaryPipe, -1, sizeof(ordinaryPipe));

    pid_t childpid;

    for (int i = 0; i < numOrdinaryPipe + 1; ++i) 
    {
        // UserPipeHandle
        bool hasUserPipeInput = false, hasUserPipeOutput = false, userPipeExists = false;
        int argc = cmd[i].size();
        
        if (argc > 1)
        {
            // 如果這一段指令的最後一個字串的第一個字元是'>' Ex: >2
            if (cmd[i][argc - 1][0] == '>')
            {
                // >3 -> 3
                sscanf(cmd[i][argc - 1].substr(1).c_str(), "%d", &UserPipeDstId);
                hasUserPipeOutput = true;
            }
            else if (cmd[i][argc - 1][0] == '<')
            {
                // <3 -> 3
                sscanf(cmd[i][argc - 1].substr(1).c_str(), "%d", &UserPipeSrcId);
                hasUserPipeInput = true;
            }
            // 處理 <2 >3 的情況
            if (argc > 2)
            {
                if (cmd[i][argc - 2][0] == '>')
                {
                    sscanf(cmd[i][argc - 2].substr(1).c_str(), "%d", &UserPipeDstId);
                    hasUserPipeOutput = true;
                }
                else if (cmd[i][argc - 2][0] == '<')
                {
                    sscanf(cmd[i][argc - 2].substr(1).c_str(), "%d", &UserPipeSrcId);
                    hasUserPipeInput = true;
                }
            }
        }

        if (hasUserPipeInput && UserPipeSrcId != 0)
        {
            cmd[i].pop_back();    // 移除 >3
            if (client[UserPipeSrcId] == -1)
            {
                cerr << "*** Error: user #" << UserPipeSrcId << " does not exist yet. ***" << '\n';
            }
            // 處理 <2 cat 但user #2 並沒有對使用過 >N的情況 Ex: *** Error: the pipe #<sender_id>->#<receiver_id> does not exist yet. ***
            else if (userPipes.find({UserPipeSrcId, userId}) == userPipes.end())
            {
                cerr << "*** Error: the pipe #" << UserPipeSrcId << "->#" << userId << " does not exist yet. ***" << '\n';
            }
            else
            {
                for (const auto& [uid, info] : usersData)
                {
                    dup2(client[uid], STDIN_FILENO);
                    dup2(client[uid], STDOUT_FILENO);
                    dup2(client[uid], STDERR_FILENO);
                    cout << "*** " << usersData[userId].name << " (#" << userId << ") just received from " << usersData[UserPipeSrcId].name << " (#" << UserPipeSrcId << ") by '" << line << "' ***\n";
                }
                dup2(client[userId], STDIN_FILENO);
                dup2(client[userId], STDOUT_FILENO);
                dup2(client[userId], STDERR_FILENO);
                shouldReceiveUserPipe = true;
            }
        }

        if (hasUserPipeOutput && UserPipeDstId != 0)
        {
            cmd[i].pop_back();    // 移除 <3
            if (client[UserPipeDstId] == -1)
            {
                cerr << "*** Error: user #" << UserPipeDstId << " does not exist yet. ***" << '\n';
            }
            else if (userPipes.find({userId, UserPipeDstId}) != userPipes.end())
            {
                cerr << "*** Error: the pipe #" << userId << "->#" << UserPipeDstId << " already exists. ***" << '\n';
                userPipeExists = true;
            }
            else
            {
                for (const auto& [uid, info] : usersData)
                {
                    dup2(client[uid], STDIN_FILENO);
                    dup2(client[uid], STDOUT_FILENO);
                    dup2(client[uid], STDERR_FILENO);
                    cout << "*** " << usersData[userId].name << " (#" << userId << ") just piped '" << line << "' to " << usersData[UserPipeDstId].name << " (#" << UserPipeDstId << ") ***" << '\n';
                }
                dup2(client[userId], STDIN_FILENO);
                dup2(client[userId], STDOUT_FILENO);
                dup2(client[userId], STDERR_FILENO);
                pipe(userPipes[{userId, UserPipeDstId}].numberedPipe);
                shouldSendUserPipe = true;
            }
        }

        // Handle fork may failed
        while (true)
        {
            if(i < numOrdinaryPipe)
                pipe(ordinaryPipe + i * 2);

            while(waitpid(-1, &waitStatus, WNOHANG) > 0);

            childpid = fork();
            if(childpid >= 0)
                break; // fork成功跳出
            usleep(1000);       // fork失敗就稍等再試
        }

        if (childpid == 0)
        { 
            childProcess(i, numOrdinaryPipe, ordinaryPipe, fileRedirection, fd, userId, cmd, hasUserPipeInput, hasUserPipeOutput, UserPipeSrcId, UserPipeDstId, userPipeExists);
        } 
        else
        { 
            parentProcess(i, numOrdinaryPipe, ordinaryPipe, shouldReceiveUserPipe, UserPipeSrcId, userId);
        }

    }

    // 最後一條指令的處理
    if (shouldSendUserPipe)
        userPipes[{userId, UserPipeDstId}].pids.push_back(childpid);
    else if (pipeTime == -1)
        waitpid(childpid, &waitStatus, 0);
    else
        numberedPipes[pipeTime].pids.push_back(childpid);

    while ( waitpid(-1, &waitStatus, WNOHANG) > 0 );

    if (fileRedirection)
        close(fd);

    /* 重設預設輸出 */
    current_stdout = STDOUT_FILENO;
    current_stderr = STDERR_FILENO;
}

void handleNumberedPipes(string &line, int userId)
{
    int begin = 0, num = 0, pipeTime, cur ,newCur;
    
    for(cur = 0; cur < line.size(); cur++)
    {
        // 尋找 | 或 !（代表Ordinary Number管線」或「同時導出 stderr 的Number管線」）
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
        
            current_stdout = numberedPipes[pipeTime].numberedPipe[1];    // stdout 導向 pipe 寫端
            
            if(line[cur] == '!')
                current_stderr = numberedPipes[pipeTime].numberedPipe[1];   // 若是 !N 連 stderr 也導向
            
            // 取出目前找到 |N/!N 前的子字串去執行
            vector<vector<string>> cmd;
            string tline = line.substr(begin, cur - begin);
            ParseInput(cmd, tline);
            ExecuteCommand(cmd, pipeTime, line, userId);
            begin = newCur;
            cur = newCur - 1;
            num = 0;
        }
    }
    line = line.substr(begin);
}

void serviceClient(int server_fd)
{
    int i, n;
    vector<vector<string>> cmd;
    
    /* 1. 把 socket 綁到標準 I/O */
    dup2(STDIN_FILENO , BACKUP_STDIN);
    dup2(STDOUT_FILENO, BACKUP_STDOUT);
    dup2(STDERR_FILENO, BACKUP_STDERR);

    fd_set allset, rset;
    FD_ZERO(&allset);
    FD_SET(server_fd, &allset);

    for (i = 0; i < FD_SETSIZE; ++i)
        client[i] = -1;

    char buf[MAXLINE];
    int maxfd     = 30;
    int maxi      = -1;
    // int onlineCnt = 0;           // ★ 同時在線人數
    int nready, connfd, sockfd;
    socklen_t clientLen;
    struct sockaddr_in serverAddress, clientAddress;

    while (true)
    {
        rset = allset;
        nready = select(maxfd + 1, &rset, nullptr, nullptr, nullptr);
        if (nready < 0) 
        {
            if (errno == EINTR)          // 被 SIGCHLD 打斷 -> 重新 select
                continue;
            perror("select");
            continue;
        }
        if (FD_ISSET(server_fd, &rset))
        {
            clientLen = sizeof(clientAddress);
            connfd = accept(server_fd, (struct sockaddr *) &clientAddress, &clientLen);
            
            for(i = 1; i < FD_SETSIZE; i++)
            {
                if(client[i] < 0)
                {
                    client[i] = connfd;
                    break;
                }
            }
            if(i == FD_SETSIZE)
            {
                exit(1);
            }

            //user count
            numUsers++;

            inet_ntop(AF_INET, &(clientAddress.sin_addr), usersData[i].ip4, INET_ADDRSTRLEN);
            usersData[i].port = ntohs(clientAddress.sin_port);
            usersData[i].env["PATH"] = "bin:.";
            usersData[i].counter = 0;
            usersData[i].numberedPipes.clear();

            clearenv();
            for(auto env : usersData[i].env)
            {
                setenv(env.first.c_str(), env.second.c_str(), 1);
            }
            counter = usersData[i].counter;
            numberedPipes = usersData[i].numberedPipes;

            /* 歡迎訊息 + 廣播 */
            dup2(client[i], 0);
            dup2(client[i], 1);
            dup2(client[i], 2);
            cout << "****************************************\n";
            cout << "** Welcome to the information server. **\n";
            cout << "****************************************\n";

            for(auto  user: usersData)
            {
                dup2(client[user.first], 0);
                dup2(client[user.first], 1);
                dup2(client[user.first], 2);
                cout << "*** User '" << usersData[i].name << "' entered from " << usersData[i].ip4 << ":" << usersData[i].port << ". ***" << '\n';
            }
            dup2(client[i], 0);
            dup2(client[i], 1);
            dup2(client[i], 2);
            cout << "% ";
            dup2(BACKUP_STDIN , STDIN_FILENO );
            dup2(BACKUP_STDOUT, STDOUT_FILENO);
            dup2(BACKUP_STDERR, STDERR_FILENO);
            
            FD_SET(connfd, &allset);
            if(connfd > maxfd)
                maxfd = connfd;
            if(i > maxi)
                maxi = i;
            if(--nready <= 0)
                continue;
        }

        for (i = 1; i <= maxi; ++i)
        {
            sockfd = client[i];
            if (sockfd < 0)
                continue;
            if (FD_ISSET(sockfd, &rset))
            {
                clearenv();
                for(auto env : usersData[i].env)
                {
                    setenv(env.first.c_str(), env.second.c_str(), 1);
                }
                counter = usersData[i].counter;
                numberedPipes = usersData[i].numberedPipes;

                dup2(client[i], 0);
                dup2(client[i], 1);
                dup2(client[i], 2);

                while (true) 
                {
                    n = read(STDIN_FILENO, buf, MAXLINE);
                    if (n == -1 && errno == EINTR)
                        continue;
                    break;
                }

                if (n == 0)
                {
                    dup2(BACKUP_STDIN , STDIN_FILENO );
                    dup2(BACKUP_STDOUT, STDOUT_FILENO);
                    dup2(BACKUP_STDERR, STDERR_FILENO);
                    close(client[i]);
                    FD_CLR(client[i], &allset);
                    client[i] = -1;
                    for(auto user : usersData)
                    {
                        if(client[i] == -1)
                            continue;
                        userPipes.erase({user.first, i});
                        userPipes.erase({i, user.first});
                        dup2(client[user.first], 0);
                        dup2(client[user.first], 1);
                        dup2(client[user.first], 2);						
                        cout << "*** User '" << usersData[i].name << "' left. ***" << '\n';
                    }
                    usersData.erase(i);
                    numUsers--;
                }
                else
                {
                    int execFlag = 0;
                    for(int j = 0; j < n; j++)
                    {
                        if(buf[j] == '\r')
                            continue;
                        if(buf[j] != '\n')
                        {
                            usersData[i].cmd.push_back(buf[j]);
                            continue;
                        }
                        string line = usersData[i].cmd;
                        usersData[i].cmd.clear();

                        handleNumberedPipes(line, i);
                        if (line.empty())
                        {
                            cout << "% ";
                            continue;
                        }
                        ParseInput(cmd, line);
                        execFlag = buildInCommand(cmd, i);

                        if(execFlag == 1)
                        {
                            cout << "% ";
                            continue;
                        }
                        else if(execFlag == -1)
                        {
                            break;
                        }
                        if(cmd.size() <= 0)
                        {
                            cout << "% ";
                            continue;
                        }
                        counter = (counter + 1) % MAX_COUNT;
                        ExecuteCommand(cmd, -1, line, i);
                        cout << "% ";
                    }

                    usersData[i].numberedPipes = numberedPipes;
                    usersData[i].counter = counter;
                    if(execFlag == -1)
                    {
                        dup2(BACKUP_STDIN , STDIN_FILENO );
                        dup2(BACKUP_STDOUT, STDOUT_FILENO);
                        dup2(BACKUP_STDERR, STDERR_FILENO);
                        close(client[i]);
                        FD_CLR(client[i], &allset);
                        client[i] = -1;
                        for(auto user : usersData)
                        {
                            if(client[user.first] == -1)
                                continue;
                            userPipes.erase({user.first, i});
                            userPipes.erase({i, user.first});
                            dup2(client[user.first], 0);
                            dup2(client[user.first], 1);
                            dup2(client[user.first], 2);	
                            cout << "*** User '" << usersData[i].name << "' left. ***" << '\n';
                        }
                        usersData.erase(i);
                        numUsers--;
                    }
                }
                dup2(BACKUP_STDIN , STDIN_FILENO );
                dup2(BACKUP_STDOUT, STDOUT_FILENO);
                dup2(BACKUP_STDERR, STDERR_FILENO);

                if(--nready <= 0)
                    break;
            }
        }
    }
}

int main(int argc,char* argv[])
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    
    if (argc != 2) 
    {
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

    usersData.clear();
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    // socket
    
    signal(SIGCHLD, signalHandler);
    
    serviceClient(server_fd);
    
    return 0;
}