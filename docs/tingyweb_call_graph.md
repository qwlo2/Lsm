# TingyWeb 调用图

源码根目录：`/home/qiu/Tinyweberever`。本文图中的函数名均来自当前源码。

## 1. 程序启动图

```mermaid
flowchart TD
    A["main\nsrc/main.cpp"] --> B["WebServer::WebServer\nsrc/webserver.cpp"]
    B --> C["构造 HeapTimer\nunique_ptr<HeapTimer> timer_"]
    B --> D["构造 ThreadPool(6)\nunique_ptr<ThreadPool> threadpool_"]
    B --> E["构造 Epoller\nunique_ptr<Epoller> epoller_"]
    B --> F["设置 srcDir_\n/home/qiu/Tinyweberever/resources"]
    B --> G["SqlConnPool::Init\nhost=192.168.1.6 port=3306"]
    B --> H["WebServer::InitEventMode_(1)"]
    H --> H1["listenEvent_=EPOLLRDHUP\nconnEvent_=EPOLLONESHOT|EPOLLRDHUP|EPOLLET"]
    B --> I["WebServer::InitSocket_"]
    I --> I1["socket"]
    I1 --> I2["setsockopt SO_LINGER"]
    I2 --> I3["setsockopt SO_REUSEADDR"]
    I3 --> I4["bind"]
    I4 --> I5["listen backlog=1024"]
    I5 --> I6["Epoller::AddFd(listenFd)"]
    I6 --> I7["SetFdNonblock(listenFd)"]
    B --> J["server.Start()"]
    J --> K["WebServer::Start\n主事件循环"]
```

真实顺序补充：

- `Log::init` 只有 `openLog=true` 才调用；当前 main 传 `false`。
- `SqlConnPool::Init` 在 `InitEventMode_` 和 `InitSocket_` 之前。
- `InitSocket_` 失败会设置 `isClose_=true`。

## 2. 新连接建立图

```mermaid
flowchart TD
    A["epoll_wait 返回 listenFd"] --> B["WebServer::Start"]
    B --> C["WebServer::DealListen_"]
    C --> D["accept(listenFd)"]
    D --> E{"fd <= 0?"}
    E -- yes --> F["return"]
    E -- no --> G{"HttpConn::userCount >= MAX_FD?"}
    G -- yes --> H["SendError_\nclose fd"]
    G -- no --> I["WebServer::AddClient_(fd, addr)"]
    I --> J["users_[fd].init\nHttpConn::init"]
    J --> K["HttpConn::userCount++\n保存 fd/addr\n清空 Buffer"]
    I --> L["timer_->add(fd, timeout,\n[this,fd]{ CloseConn_(&users_[fd]); })"]
    I --> M["SetFdNonblock(fd)"]
    M --> N["Epoller::AddFd(fd,\nEPOLLIN | connEvent_)"]
```

关键成员变量：

- `WebServer::users_`：`unordered_map<int, HttpConn>`
- `WebServer::timer_`
- `WebServer::epoller_`
- `HttpConn::fd_`
- `HttpConn::readBuff_`、`writeBuff_`

风险：timer 回调捕获 fd，worker 任务捕获 `HttpConn*`，无 generation 防护。

## 3. GET 静态文件请求图

```mermaid
flowchart TD
    A["EPOLLIN(client fd)"] --> B["WebServer::DealRead_"]
    B --> C["ExtentTime_\ntimer_->adjust"]
    B --> D["ThreadPool::AddTask\nlambda: OnRead_(client)"]
    D --> E["worker: WebServer::OnRead_"]
    E --> F["HttpConn::read"]
    F --> G["Buffer::ReadFd"]
    G --> H["readv(fd, iov, 2)"]
    H --> I["WebServer::OnProcess"]
    I --> J["HttpConn::process"]
    J --> K["request_.Init"]
    K --> L["HttpRequest::parse"]
    L --> L1["ParseRequestLine_"]
    L1 --> L2["ParsePath_"]
    L2 --> L3["ParseHeader_"]
    L3 --> M["HttpResponse::Init\ncode=200"]
    M --> N["HttpResponse::MakeResponse"]
    N --> O["stat(srcDir + path)"]
    O --> P["AddStateLine_"]
    P --> Q["AddHeader_"]
    Q --> R["AddContent_"]
    R --> S["open + mmap"]
    S --> T["设置 iov_[0]=响应头\niov_[1]=文件映射"]
    T --> U["Epoller::ModFd\nEPOLLOUT | connEvent_"]
    U --> V["EPOLLOUT"]
    V --> W["DealWrite_ -> AddTask"]
    W --> X["worker: OnWrite_"]
    X --> Y["HttpConn::write"]
    Y --> Z["writev(fd, iov_, iovCnt_)"]
```

写完后：

```mermaid
flowchart TD
    A["HttpConn::write 完成"] --> B{"ToWriteBytes()==0?"}
    B -- no --> C{"ret < 0 && errno == EAGAIN?"}
    C -- yes --> D["ModFd EPOLLOUT"]
    C -- no --> E["CloseConn_"]
    B -- yes --> F{"IsKeepAlive()?"}
    F -- yes --> G["OnProcess(client)\n处理 Buffer 中可能已有的下一请求\n否则 ModFd EPOLLIN"]
    F -- no --> E
```

## 4. POST 登录图

```mermaid
flowchart TD
    A["EPOLLIN"] --> B["OnRead_"]
    B --> C["HttpConn::read/readv"]
    C --> D["HttpConn::process"]
    D --> E["HttpRequest::parse"]
    E --> F["ParseBody_"]
    F --> G["ParseFromUrlencoded_\nusername/password"]
    G --> H["ParsePost_"]
    H --> I["UserVerify(name,pwd,isLogin=true)"]
    I --> J["SqlConnPool::Instance()->GetConn"]
    J --> K["shared_ptr<MYSQL> with custom deleter"]
    K --> L["mysql_query SELECT ... username='%s'"]
    L --> M["mysql_store_result/mysql_fetch_row"]
    M --> N{"password match?"}
    N -- yes --> O["path_=/welcome.html"]
    N -- no --> P["path_=/error.html"]
    O --> Q["HttpResponse 静态文件响应"]
    P --> Q
```

注册路径：

```mermaid
flowchart TD
    A["POST /register.html"] --> B["ParsePost_ tag=0"]
    B --> C["UserVerify(..., isLogin=false)"]
    C --> D["mysql_query INSERT INTO user(username,password) VALUES(...)"]
    D --> E{"mysql_query 成功?"}
    E -- yes --> F["/welcome.html"]
    E -- no --> G["/error.html"]
```

安全边界：SQL 由 `snprintf` 拼接，当前没有 prepared statement。

## 5. 超时关闭图

```mermaid
flowchart TD
    A["AddClient_"] --> B["timer_->add(fd, timeout, cb)"]
    C["读/写事件"] --> D["ExtentTime_"]
    D --> E["timer_->adjust(fd, timeout)"]
    F["WebServer::Start 每轮循环"] --> G["timer_->GetNextTick"]
    G --> H["HeapTimer::tick"]
    H --> I{"heap_[0].expires <= now?"}
    I -- no --> J["返回下次 timeout 给 epoll_wait"]
    I -- yes --> K["执行 TimerNode.cb"]
    K --> L["WebServer::CloseConn_(&users_[fd])"]
    L --> M["Epoller::DleFd"]
    M --> N["HttpConn::Close"]
    N --> O["UnmapFile + close(fd) + userCount--"]
    K --> P["HeapTimer::pop"]
```

风险：`CloseConn_` 不主动从 timer 删除；fd 复用没有 generation。

## 6. Keep-Alive 状态图

```mermaid
stateDiagram-v2
    [*] --> Reading: EPOLLIN
    Reading --> Processing: OnRead_ 成功
    Processing --> Writing: process() 返回 true / ModFd EPOLLOUT
    Processing --> WaitingRead: process() 返回 false / ModFd EPOLLIN
    Writing --> WaitingWrite: writev EAGAIN / ModFd EPOLLOUT
    Writing --> Processing: 写完且 IsKeepAlive=true / OnProcess
    Writing --> Closed: 写完且 IsKeepAlive=false
    WaitingRead --> Reading: 下一次 EPOLLIN
    WaitingWrite --> Writing: 下一次 EPOLLOUT
    Closed --> [*]
```

Keep-Alive 判定源码：

```text
HttpRequest::IsKeepAlive:
header_ contains "Connection"
&& header_["Connection"] == "keep-alive"
&& version_ == "1.1"
```

因此不是完整 HTTP/1.1 默认 keep-alive 语义。

## 7. 线程关系图

```mermaid
flowchart LR
    Main["主线程\nWebServer::Start"] --> Epoll["Epoller\nepoll_wait"]
    Main --> Accept["DealListen_/accept"]
    Main --> Timer["HeapTimer\nGetNextTick/tick"]
    Main --> Dispatch["ThreadPool::AddTask"]

    Dispatch --> W1["worker thread"]
    Dispatch --> W2["worker thread"]
    Dispatch --> WN["worker thread"]

    W1 --> Read["HttpConn::read\nBuffer::ReadFd/readv"]
    W1 --> Parse["HttpRequest::parse"]
    W1 --> Resp["HttpResponse::MakeResponse\nstat/mmap"]
    W1 --> Write["HttpConn::write\nwritev"]
    W1 --> SQL["SqlConnPool/mysql_query"]
    W1 --> Mod["Epoller::ModFd"]

    LogT["日志线程\n仅 openLog=true"] --> LogQ["BlockDeque"]
```

共享资源审计：

| 资源 | 访问线程 | 锁 | 风险 |
| --- | --- | --- | --- |
| `users_` | 主线程、timer 回调、worker 间接使用指针 | 无 | 指针悬空、fd 复用、数据竞争 |
| `epoller_` | 主线程 wait/add/del，worker mod | 无对象锁；epoll_ctl 内核线程安全 | 生命周期和事件顺序风险 |
| `timer_` | 主线程为主 | 无 | 与 worker 处理同 fd 竞争 |
| `SqlConnPool` | worker | mutex + semaphore | close 时 borrowed connection 风险 |
| `ThreadPool::tasks` | 主线程/worker | mutex + cv | 无界队列，无优雅退出 |
| `Log` | 多线程 | mutex + 队列 | 默认未启用；队列满可能同步阻塞 |

## 8. 对象生命周期图

```mermaid
flowchart TD
    A["main 栈对象 WebServer server"] --> B["WebServer 成员"]
    B --> C["unique_ptr<HeapTimer> timer_"]
    B --> D["unique_ptr<ThreadPool> threadpool_"]
    B --> E["unique_ptr<Epoller> epoller_"]
    B --> F["unordered_map<int,HttpConn> users_"]
    F --> G["HttpConn"]
    G --> H["Buffer readBuff_/writeBuff_"]
    G --> I["HttpRequest request_"]
    G --> J["HttpResponse response_"]
    J --> K["mmap 文件区域 mmFile_"]
    B --> L["char* srcDir_ malloc/free"]
    M["SqlConnPool 单例"] --> N["MYSQL* 队列"]
    O["Log 单例"] --> P["FILE* + BlockDeque + 日志线程"]
```

析构路径：

```text
WebServer::~WebServer
-> isClose_=true
-> close(listenFd_)
-> free(srcDir_)
-> SqlConnPool::ClosePool()
-> unique_ptr 成员析构
```

不完整 RAII：

- `listenFd_` 手动 close
- `srcDir_` 手动 malloc/free
- `HttpConn` 手动 `Close`
- `ThreadPool` detached 线程不 join
- worker lambda 捕获裸指针

## 9. 一页 ASCII 总图

```text
main
 |
 v
WebServer 构造
 |-- ThreadPool(6 detached workers)
 |-- Epoller(epoll_create)
 |-- HeapTimer
 |-- SqlConnPool(12 MySQL connections)
 |-- InitEventMode(trigMode=1)
 '-- InitSocket(socket/bind/listen/add listen fd)
 |
 v
Start 主循环
 |-- timeout = timer.GetNextTick()
 |-- epoll_wait(timeout)
 |-- listen fd -> accept -> AddClient -> timer.add -> epoll add EPOLLIN
 |-- EPOLLIN   -> AddTask -> worker readv -> parse -> response/mmap -> ModFd EPOLLOUT
 |-- EPOLLOUT  -> AddTask -> worker writev -> keepalive ? process next : close
 '-- ERR/RDHUP/HUP -> CloseConn
```

## 10. 缺陷关系图

```mermaid
flowchart TD
    A["ThreadPool detach + no stop"] --> B["退出时 worker 可能访问已析构 WebServer"]
    C["worker lambda 捕获 HttpConn*"] --> D["CloseConn_/fd 复用导致旧任务操作新连接"]
    E["InitEventMode isET 位或错误"] --> F["连接侧总按 ET 循环"]
    F --> G["readv 返回 0 时 read 循环可能空转"]
    H["HTTP parser 不用 Content-Length"] --> I["POST 半包误判完整"]
    J["SQL 字符串拼接"] --> K["SQL 注入风险"]
    L["Start 先 IN/OUT 后 ERR"] --> M["复合事件可能先读写而非关闭"]
```
