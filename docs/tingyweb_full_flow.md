# 从零理解 TingyWeb 完整运行流程

本文基于 `/home/qiu/Tinyweberever` 当前源码生成，只描述源码中能确认的行为。项目不是多 Reactor，也不是 one loop per thread；当前是**单 Reactor 主线程 + worker 线程池处理读写与业务**的教学型 WebServer。

## 1. 项目整体定位

TingyWeb 是一个 C++14 单机 HTTP 静态资源服务器，带登录/注册示例、MySQL 连接池、小根堆定时器、线程池、Buffer、epoll 封装、同步/异步日志。它能处理静态文件 GET 和简单表单 POST，但不能称为完整 HTTP/1.1 服务器：源码未完整处理 `Content-Length`、chunked、pipeline、TLS、限流、背压和安全 SQL。

关键入口：

- 入口：`/home/qiu/Tinyweberever/src/main.cpp` `main`
- 核心服务器：`server/webserver.h` `WebServer`，`src/webserver.cpp`
- 事件封装：`server/epoll.h` `Epoller`，`src/epoll.cpp`
- 连接对象：`http/httpconn.h` `HttpConn`，`src/httpconn.cpp`
- 请求解析：`http/httprequest.h` `HttpRequest`，`src/httprequest.cpp`
- 响应生成：`http/httpresponse.h` `HttpResponse`，`src/httpresponse.cpp`
- 缓冲区：`buffer/buffer.h` `Buffer`，`src/buffer.cpp`
- 定时器：`timer/heaptimer.h` `HeapTimer`，`src/heaptimer.cpp`
- 线程池：`Pool/threadpool.h` `ThreadPool`
- 数据库连接池：`Pool/sqlconnpool.h` `SqlConnPool`，`src/sqlconnpool.cpp`
- 日志：`log/log.h` `Log`，`log/blockqueue.h` `BlockDeque`，`src/log.cpp`
- 构建：`CMakeLists.txt`
- 静态资源：`resources/index.html`、`login.html`、`register.html`、`welcome.html`、`400/403/404.html`、CSS/JS/图片/视频

## 2. 项目目录与模块

| 模块 | 负责什么 | 谁创建 | 谁调用 | 核心资源 | 线程 | 销毁 |
| --- | --- | --- | --- | --- | --- | --- |
| `WebServer` | 管理监听 fd、epoll、连接表、timer、threadpool | `main` 栈对象 | `main -> Start` | `listenFd_`、`users_`、`timer_`、`epoller_` | 主线程创建；worker 会回调其成员函数 | `~WebServer` close listen、free srcDir、ClosePool |
| `Epoller` | 封装 epoll fd 和事件数组 | `WebServer` 构造 | `WebServer::Start/AddClient/CloseConn` | `epollfd`、`events_` | 主线程主要调用；worker 也会调用 `ModFd` | 析构 close epoll fd |
| `HttpConn` | 单连接读写、请求解析、响应生成 | `users_[fd].init` | 主线程拿指针投递给 worker | fd、地址、Buffer、Request、Response、iovec | worker 读写；主线程/定时器可能关闭 | `Close` close fd、munmap、userCount-- |
| `HttpRequest` | HTTP 请求行、头、body 和登录注册解析 | `HttpConn` 成员 | `HttpConn::process` | method/path/version/header/body/post | worker | 随 `HttpConn` |
| `HttpResponse` | stat、mmap 文件、构造响应头和 body | `HttpConn` 成员 | `HttpConn::process/write/Close` | `mmFile_`、`mmFileStat_` | worker/Close | `UnmapFile` |
| `Buffer` | 管理读写缓冲，封装 readv/write | `HttpConn` 成员、`Log` 成员 | `HttpConn::read/process` | `vector<char>`、`readPos_`、`writePos_` | 所属连接的 worker；没有外部锁 | 随对象 |
| `ThreadPool` | 保存任务队列并创建 detached worker | `WebServer` 构造 | `DealRead_`、`DealWrite_` | queue、mutex、cv、detached threads | 多 worker | 析构不 join，存在缺陷 |
| `HeapTimer` | 连接超时管理 | `WebServer` 构造 | `AddClient_`、`ExtentTime_`、`Start` | `heap_`、`ref_` | 主线程为主；timer 回调会关闭连接 | `clear` |
| `SqlConnPool` | MySQL 连接复用 | 单例 | `WebServer` 构造、`HttpRequest::UserVerify` | queue、mutex、sem、MYSQL* | worker 访问 | `ClosePool` |
| `Log` | 同步/异步日志 | 单例，只有 openLog 时 init | LOG 宏 | FILE*、Buffer、BlockDeque、日志线程 | 多线程 | 单例析构 join 日志线程 |

## 3. 线程模型

通俗解释：Reactor 是“等 fd 就绪，再调用对应处理逻辑”的模型。epoll 告诉你“这个 fd 可以读/可以写”，应用再自己调用 `readv/writev`。Proactor 则是“异步 I/O 完成后通知你结果”。当前项目使用 epoll 就绪事件，所以是 Reactor。

当前线程关系：

```text
主线程
├── WebServer::Start
├── epoll_wait
├── accept 新连接
├── timer_->GetNextTick/tick
├── 向 ThreadPool 投递读写任务
└── 部分 epoll_ctl：AddFd、DelFd

工作线程
├── OnRead_ -> HttpConn::read -> Buffer::ReadFd(readv)
├── HttpConn::process -> HttpRequest::parse -> HttpResponse::MakeResponse
├── UserVerify -> SqlConnPool -> mysql_query
├── OnWrite_ -> HttpConn::write(writev)
└── ModFd 重新激活 ONESHOT 事件
```

为什么不是多 Reactor：源码只有一个 `WebServer::epoller_`，由 `std::make_unique<Epoller>()` 创建；没有每线程一个 epoll，也没有 fd 分发到 sub reactor。

## 4. 程序启动

`src/main.cpp`：

```cpp
WebServer server(
    1316, 1, 60000, false,
    3306, "root", "123456", "tinywebserver",
    12, 6, false, 1, 1024);
server.Start();
```

当前实际配置：

- 端口：`1316`
- `trigMode=1`：按 `InitEventMode_` 逻辑，监听 fd LT，连接 fd ET
- 超时：`60000 ms`
- `OptLinger=false`：不开启 linger 延迟关闭
- MySQL：端口 `3306`，用户 `root`，密码 `123456`，库 `tinywebserver`
- 注意：`WebServer` 构造中实际 host 写死为 `"192.168.1.6"`，不是 main 参数传入
- 数据库连接数：`12`
- 线程池线程数：`6`
- 日志开关：`false`，因此默认运行不初始化日志系统
- 日志等级：`1`
- 异步日志队列容量：`1024`

真实启动链：

```text
main
-> WebServer::WebServer
   -> 构造成员 timer_/threadpool_/epoller_
   -> 设置 srcDir_ = "/home/qiu/Tinyweberever/resources"
   -> 设置 HttpConn::userCount/srcDir
   -> 如果 openLog 才 Log::init
   -> SqlConnPool::Init("192.168.1.6", ...)
   -> InitEventMode_(trigMode)
   -> InitSocket_()
-> WebServer::Start()
```

初始化失败会导致 `isClose_ = true`：主要是 `InitSocket_` 内 socket、setsockopt、bind、listen、epoll add 失败。`SqlConnPool::Init` 中 MySQL 连接失败只打日志，没有让构造函数失败退出。

`Start()` 长期阻塞的原因：循环条件是 `while(!isClose_)`，而源码没有信号处理或优雅关闭入口；正常情况下主线程一直在 `epoller_->Wait(timeMS)` 等事件。

## 5. Socket 初始化

函数：`src/webserver.cpp` `WebServer::InitSocket_`。

真实顺序：

```text
填 sockaddr_in
-> 检查端口
-> socket(AF_INET, SOCK_STREAM, 0)
-> setsockopt SO_LINGER
-> setsockopt SO_REUSEADDR
-> bind
-> listen(..., 1024)
-> epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN)
-> SetFdNonblock(listenFd_)
```

### socket

- 地址族：`AF_INET`，IPv4
- 类型：`SOCK_STREAM`，TCP 字节流
- 返回值：监听 socket fd，保存在 `listenFd_`
- 失败：返回 `<0`，记录日志、返回 false

### SO_REUSEADDR

源码设置：`setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))`。它帮助服务端重启时绑定处于 TIME_WAIT 相关状态的地址，但不能让多个进程都正常接收同端口连接；那是 `SO_REUSEPORT` 的语义。

### SO_LINGER

源码总是调用 `setsockopt(SO_LINGER)`。当 `openLinger_ > 0` 时设置 `l_onoff=1,l_linger=1`；当前 main 传 `false`，所以 `linger lin={0}`，即关闭 linger。实际 close 时通常立即返回，内核继续处理 TCP 关闭，不等待 1 秒发送残留数据。

### bind

`sockaddr_in`：

- `sin_family = AF_INET`
- `sin_port = htons(port_)`：主机字节序转网络字节序
- `sin_addr.s_addr = htonl(INADDR_ANY)`：绑定所有本机网卡地址

bind 失败常见原因：端口被占用、权限不足、地址不可用。源码失败后 close listen fd 并返回 false。

### listen

`listen(listenFd_, 1024)`。backlog 是内核维护的连接队列大小提示，不等于最大并发连接数；最大连接数还受 fd 限制、`MAX_FD`、内存、系统参数影响。

### epoll 注册与非阻塞

监听 fd 注册事件：`listenEvent_ | EPOLLIN`。当前 `trigMode=1` 时 `listenEvent_ = EPOLLRDHUP`，因此监听 fd 注册为 `EPOLLRDHUP | EPOLLIN`，没有 `EPOLLET`，即 LT。源码先 epoll add，再 `SetFdNonblock(listenFd_)`。返回值未检查，这是一个缺陷。

连接 fd 在 `AddClient_` 里 `SetFdNonblock(fd)`，再 `epoller_->AddFd(fd, EPOLLIN | connEvent_)`。ET 必须配合非阻塞，否则一次读不完后可能阻塞或丢事件。

## 6. 事件模式

函数：`WebServer::InitEventMode_(int trigMode)`。

初始：

- `connEvent_ = EPOLLONESHOT | EPOLLRDHUP`
- `listenEvent_ = EPOLLRDHUP`

各模式：

| trigMode | 监听 fd | 连接 fd |
| --- | --- | --- |
| 0 | LT | LT |
| 1 | LT | ET |
| 2 | ET | LT |
| 3 | ET | ET |
| default | ET | ET |

当前 main 用 `trigMode=1`：监听 LT，连接 ET。

重要 flag：

- `EPOLLRDHUP`：对端关闭或半关闭连接时触发
- `EPOLLONESHOT`：同一个 fd 触发一次后被禁用，必须 `ModFd` 重新激活
- `EPOLLET`：边缘触发，状态变化时通知，要求非阻塞并读/写到 EAGAIN

为什么连接 fd 要 ONESHOT：当前读写任务在 worker 线程执行。如果没有 ONESHOT，线程 A 正在处理 fd，fd 又触发事件，主线程可能把同 fd 再投递给线程 B，两个线程同时操作同一个 `HttpConn`、Buffer、Response，发生数据竞争。ONESHOT 触发后禁用该 fd，worker 处理完再 `ModFd` 注册下一次 `EPOLLIN/EPOLLOUT`。

源码缺陷：`HttpConn::isET=(connEvent_|EPOLLET);` 使用了位或，不是判断；结果基本恒为 true。正确写法应类似 `(connEvent_ & EPOLLET)`。因此即使 trigMode 选择连接 LT，`HttpConn::read/write` 仍可能按 ET 循环行为执行。

## 7. Epoller 封装

文件：`server/epoll.h`，`src/epoll.cpp`。

- 构造：`epollfd(epoll_create(5))`，`events_(maxEvent)`；返回的是 epoll 实例 fd
- 析构：`close(epollfd)`
- `AddFd(fd,event)`：`epoll_ctl(EPOLL_CTL_ADD)`
- `ModFd(fd,event)`：`epoll_ctl(EPOLL_CTL_MOD)`
- `DleFd(fd)`：`epoll_ctl(EPOLL_CTL_DEL)`
- `Wait(timeoutms)`：`epoll_wait(epollfd, &events_[0], events_.size(), timeoutms)`
- `GetEventFd(i)`：取第 i 个就绪事件中的 fd
- `GetEvent(i)`：取事件 flags

`epoll_wait` 返回：

- `>0`：就绪事件数量
- `=0`：超时，没有 fd 就绪
- `<0`：错误；当前 `Start()` 没有特殊处理，for 循环不会执行

epoll 返回的是“就绪事件”，不是“完成事件”，因此项目属于 Reactor。

## 8. 主事件循环

函数：`WebServer::Start()`。

真实伪代码：

```cpp
while (!isClose_) {
    if (timeoutMS_ > 0) {
        timeMS = timer_->GetNextTick();
    }
    int nums = epoller_->Wait(timeMS);
    for (int i = 0; i < nums; i++) {
        fd = epoller_->GetEventFd(i);
        event = epoller_->GetEvent(i);
        if (fd == listenFd_) {
            DealListen_();
        } else if (event & EPOLLIN) {
            DealRead_(&users_[fd]);
        } else if (event & EPOLLOUT) {
            DealWrite_(&users_[fd]);
        } else if (event & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
            CloseConn_(&users_[fd]);
        } else {
            LOG_ERROR("Unexpected event");
        }
    }
}
```

`GetNextTick()` 会先 `tick()` 清理过期连接，再返回堆顶距离现在的毫秒数，作为 `epoll_wait` timeout。所以定时器会影响主线程最多睡多久。

事件判断顺序缺陷：源码先判断 `EPOLLIN/EPOLLOUT`，最后才判断 `EPOLLERR/EPOLLRDHUP/EPOLLHUP`。如果一个 fd 同时有可读和 RDHUP，当前会优先走读任务，而不是立刻关闭。

退出：只有 `isClose_` 变 true。源码没有信号处理和优雅关闭机制；析构时才设置 `isClose_=true`。

## 9. 新连接建立

调用链：

```text
epoll_wait 返回 listenFd_
-> WebServer::DealListen_
-> accept
-> WebServer::AddClient_
-> users_[fd].init
-> timer_->add
-> SetFdNonblock(fd)
-> epoller_->AddFd(fd, EPOLLIN | connEvent_)
```

`accept` 返回新连接 fd。监听 fd 只负责接收连接，新 fd 才代表一个客户端 TCP 连接。客户端地址写入局部 `sockaddr_in addr`，随后传给 `HttpConn::init`。

LT/ET 下 accept 循环：`DealListen_` 使用 `do...while(listenEvent_ & EPOLLET)`。当前监听 fd 是 LT，所以每次只 accept 一个；如果监听 ET，会循环 accept 到失败。源码没有检查 `errno == EAGAIN`，fd<=0 直接返回。

`users_`：

- 类型：`std::unordered_map<int, HttpConn>`
- key：连接 fd
- value：连接对象
- `users_[fd]` 可能插入新元素，也可能在 fd 复用时复用已有对象
- 当前无锁保护；主线程、定时器回调、worker 线程都可能间接访问 `HttpConn*`，存在竞态和生命周期风险

`HttpConn::init`：

- `userCount++`
- 保存 fd 和地址
- 清空读写 Buffer
- `isClose_=false`
- 不重置 `request_`/`response_` 的所有外部状态；`process()` 会 `request_.Init()`，`response_.Init()` 会 unmap

timer：`timer_->add(fd, timeoutMS_, [this, fd](){ CloseConn_(&users_[fd]); })`。回调捕获 `this` 和 fd。风险：fd 复用时，旧 timer 回调可能作用到新连接；当前没有 generation/version。

## 10. 读事件

调用链：

```text
EPOLLIN
-> DealRead_(HttpConn*)
-> ExtentTime_
-> ThreadPool::AddTask(lambda)
-> worker: OnRead_
-> HttpConn::read
-> Buffer::ReadFd
-> readv
-> OnProcess
-> HttpConn::process
```

`DealRead_` 先延长超时，再把 `[this, client] { OnRead_(client); }` 放入线程池。主线程投递后继续回到事件循环。

风险：lambda 捕获裸 `HttpConn*`。如果主线程/定时器在任务执行前关闭连接，或者 fd 被复用，worker 可能操作已关闭或已重新初始化的连接对象。

`OnRead_`：

- `client->read(&readErrno)`
- `ret <= 0 && readErrno != EAGAIN` 时关闭
- 否则进入 `OnProcess`

`HttpConn::read`：

```cpp
do {
    len = readBuff_.ReadFd(fd_, saveErrno);
    if (len < 0) break;
} while(isET);
```

严重边界：ET 下如果 `readv` 返回 0，表示对端关闭；源码没有 break，`while(isET)` 可能造成循环读 0 的空转。

## 11. Buffer/readv

`Buffer` 内部：

- `std::vector<char> buffer_`
- `readPos_`：可读数据起点
- `writePos_`：可写位置
- 可读字节：`writePos_ - readPos_`
- 可写字节：`buffer_.size() - writePos_`
- prependable：`readPos_`

核心函数：

- `EnsureWriteable(len)`：空间不足就 `MakeSpace_`
- `MakeSpace_(len)`：如果可写+前置空间仍不够，则 resize；否则把可读数据搬到开头
- `Append`：确保空间后 copy
- `Retrieve`：移动 readPos
- `RetrieveUntil`：消费到指定指针
- `RetrieveAll`：清空并 bzero
- `Peek`：返回可读起点
- `BeginWrite`：返回可写起点

`Buffer::ReadFd` 使用两个 iovec：

```text
iov[0] -> Buffer 当前可写区域
iov[1] -> 栈上 65535 字节临时缓冲区
readv(fd, iov, 2)
```

三种情况：

1. `len <= WritableBytes`：只写进 buffer，`HasWritten(len)`
2. `len == WritableBytes`：同样只更新 writePos 到末尾
3. `len > WritableBytes`：buffer 写满，`writePos_=buffer_.size()`，额外部分从栈缓冲 `Append` 到 buffer

这个设计减少了读前预扩容，但不是零拷贝：数据仍从内核复制到用户态 buffer/栈缓冲，超出部分还要 append 到 vector。

当前没有最大请求大小限制；恶意大请求可能导致 vector 扩容占用内存。

## 12. HTTP 请求解析

入口：`HttpConn::process -> request_.parse(readBuff_)`。

状态机：

```text
REQUEST_LINE -> HEADERS -> BODY -> FINISH
```

解析循环用 `std::search` 找 `\r\n`。每轮构造 `line(buff.Peek(), lineend)`，解析后 `RetrieveUntil(lineend + 2)` 消费行和 CRLF。

### 请求行

`ParseRequestLine_` 使用正则：

```regex
^([^ ]*) ([^ ]*) HTTP/([^ ]*)$
```

保存 method、path、version，然后进入 HEADERS。源码没有限制 method 集合；只是在后续 POST 逻辑中特判 POST。

`ParsePath_`：

- `/` -> `/index.html`
- `/index`、`/login`、`/register` 等如果拼上 `.html` 后在默认集合里，就转成对应 html
- 其他路径保持原样，用于静态文件

### 请求头

`ParseHeader_` 使用正则：

```regex
^([^ ]*): ?(.*)$
```

匹配成功后 `header_.emplace(key,value)`。Header 大小写不归一化；重复 Header 因为 `emplace` 不覆盖，保留第一次。空行不匹配，进入 BODY。

`Connection` 判断：`IsKeepAlive` 只在存在 Header `"Connection"`，且值严格等于 `"keep-alive"`，且版本为 `"1.1"` 时返回 true。它没有实现 HTTP/1.1 默认 keep-alive。

### Body

`ParseBody_` 把当前 line 当作整个 body。若 `Content-Type == application/x-www-form-urlencoded`，调用 `ParseFromUrlencoded_`，支持 `+` 转空格和 `%xx` 解码，再进入登录/注册。

关键缺陷：源码不使用 `Content-Length` 来判断 body 是否完整。POST body 半包时可能把当前已到达的部分误判为完整 body；多个请求粘在同一个 Buffer 中也没有完整 pipeline 语义。

## 13. 路由与登录注册

`ParsePost_`：

- 只处理 `method_ == "POST"` 且 `Content-Type == "application/x-www-form-urlencoded"`
- `DEFAULT_HTML_TAG`：`/register.html -> 0`，`/login.html -> 1`
- 登录/注册都调用 `UserVerify(post_["username"], post_["password"], isLogin)`
- 成功：`path_="/welcome.html"`
- 失败：`path_="/error.html"`

`UserVerify`：

- 从 `SqlConnPool::Instance()->GetConn()` 获取 `shared_ptr<MYSQL>`
- 登录 SQL：`SELECT username,password FROM user WHERE username='%s' LIMIT 1`
- 注册 SQL：`INSERT INTO user(username,password) VALUES('%s','%s')`

SQL 注入风险：用户名和密码直接拼入 SQL 字符串，没有 prepared statement、参数绑定、转义、密码哈希。注册也没有先查重，只依赖插入失败。

无法从源码确认：数据库表结构、唯一索引、真实部署数据库账号权限。

## 14. MySQL 连接池

`SqlConnPool` 是函数内静态单例：`Instance()` 返回 `static SqlConnPool connsql`。

`Init`：

- 循环 `connSize` 次
- `mysql_init`
- `mysql_real_connect(host,user,pwd,dbName,port)`
- 成功或失败后都可能 `connQue_.push(sql)`；如果 connect 失败，`sql` 可能为空，这是风险
- `sem_init(&semId_, 0, MAX_CONN_)`

`GetConn`：

- 加 `mutex_`
- 如果队列空，直接返回 `nullptr`
- 否则 `sem_wait(&semId_)`
- pop 一个 `MYSQL*`
- 包装成 `shared_ptr<MYSQL>`，自定义 deleter 把连接 push 回队列并 `sem_post`

注意：它不是“连接耗尽时阻塞等待”的常见实现，因为队列空时直接返回 nullptr。`sem_wait` 在锁内执行，当前通常不会阻塞，但设计不够干净。没有超时获取。

归还路径：`shared_ptr` 离开作用域时 deleter 自动归还，所以这里用了 RAII 思路；`sqlconnRAII.h` 中的 RAII 类整段注释掉，未实际使用。

关闭风险：`ClosePool` 只关闭队列里的空闲连接；如果 worker 正持有 shared_ptr，连接池关闭后 deleter 仍可能访问池对象。

## 15. HTTP 响应

调用链：

```text
HttpConn::process
-> HttpResponse::Init
-> HttpResponse::MakeResponse
-> stat
-> ErrorHtml_
-> AddStateLine_
-> AddHeader_
-> AddContent_
-> mmap
```

状态码：

- 200：文件存在、不是目录、有 owner read 权限
- 400：请求解析失败时 `HttpConn::process` 初始化 response code=400；未知 code 也回退到 400
- 403：`!(st_mode & S_IRUSR)`
- 404：`stat` 失败或目标是目录

`stat` 检查文件是否存在、是否目录、权限和大小。

响应头：

- 状态行：`HTTP/1.1 code reason`
- `Connection:keep-alive` 或 `Connection:close`
- keep-alive 时额外 `Keep-alive:max=6,timeout=120`
- `Content-type` 根据后缀映射 MIME
- `Content-Length` 在 `AddContent_` 中追加

`mmap`：

- `open(srcDir_ + path_, O_RDONLY)`
- `mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcfd, 0)`
- 地址保存到 `mmFile_`
- 成功后 close 文件 fd
- `UnmapFile` 或析构时 munmap

边界缺陷：`mmap` 失败时没有 close `srcfd`；空文件 `st_size=0` 时 mmap 可能失败。mmap 避免应用先 read 文件到普通用户缓冲区，但网络发送仍通过 `writev`，不能说全程严格零拷贝。

## 16. mmap/writev 写事件

调用链：

```text
EPOLLOUT
-> DealWrite_
-> ThreadPool::AddTask
-> OnWrite_
-> HttpConn::write
-> writev
```

`HttpConn::process` 设置：

- `iov_[0]`：响应头所在的 `writeBuff_`
- `iov_[1]`：mmap 文件区域
- `iovCnt_=1`：只有响应头
- `iovCnt_=2`：响应头 + 文件

`writev` 返回写入字节数。部分写调整：

- 如果 `len > iov_[0].iov_len`：响应头写完，文件写了一部分；移动 `iov_[1].iov_base`，减少 `iov_[1].iov_len`，清空 writeBuff
- 否则：只写了响应头的一部分；移动 `iov_[0].iov_base`，减少 `iov_[0].iov_len`，`writeBuff_.Retrieve`

示例：响应头 200 字节，文件 1000 字节，第一次 writev 写 600 字节：

- 头 200 全部写完
- 文件写 400
- `iov_[0].iov_len=0`
- `iov_[1].iov_base += 400`
- `iov_[1].iov_len = 600`

EAGAIN：`OnWrite_` 调 `ModFd(fd, connEvent_ | EPOLLOUT)`，等待下次可写。写完后，如果 keep-alive，则 `OnProcess(client)`；否则关闭。

缺陷：`ToWriteBytes()` 总是返回 `iov_[0].iov_len + iov_[1].iov_len`，即使 `iovCnt_ == 1`。如果 `iov_[1]` 留有旧长度，可能误判仍有待写数据。

## 17. Keep-Alive

链路：

```text
HttpRequest::IsKeepAlive
-> HttpResponse::AddHeader_
-> HttpConn::IsKeepAlive
-> OnWrite_ 写完
-> OnProcess 或 ModFd EPOLLIN
```

当前实现：只有 Header 中存在精确键 `"Connection"`，值精确等于 `"keep-alive"`，且 `version_=="1.1"` 才 keep-alive。HTTP/1.1 默认 keep-alive 没有实现；`Connection: close` 会返回 false。

连接复用前：

- `HttpConn::process` 开头 `request_.Init()`
- `response_.Init` 会 unmap 旧文件
- Buffer 中若已有下一请求数据，写完后 `OnProcess` 会尝试继续处理；如果没有可读数据，`OnProcess` 会注册 EPOLLIN

长连接资源：fd、HttpConn 对象、Buffer、timer 节点。空闲连接通过 `HeapTimer` 超时关闭；有读写事件时 `ExtentTime_` 调 `timer_->adjust` 续期。

## 18. 小根堆定时器

`HeapTimer` 成员：

- `heap_`：`vector<TimerNode>`，按最早过期时间组织小根堆
- `ref_`：fd -> heap 下标，支持 O(1) 找节点
- `TimerNode`：id、expires、callback

复杂度：

- add：O(logN)
- adjust：O(logN)
- delete/pop：O(logN)
- 查堆顶：O(1)

流程：

```text
新连接 -> timer.add(fd)
读写事件 -> timer.adjust(fd)
主循环 -> GetNextTick -> tick -> 过期 cb -> CloseConn_
```

风险：

- timer 无 mutex，当前主要主线程访问，但回调关闭连接，worker 也可能同时处理同连接
- `CloseConn_` 不主动从 timer 删除，超时节点可能之后再次回调
- fd 复用没有 generation/version，旧回调理论上可能误关新连接

## 19. 线程池

`Pool/threadpool.h`：

- 构造函数创建 `threadcout` 个 `std::thread(...).detach()`
- 任务队列：`std::queue<std::function<void()>> tasks`
- 锁：`mutex_`
- 条件变量：`cv`
- `AddTask` 加锁 push，然后 `notify_one`

工作线程：

```cpp
while(true) {
    if(pool->Isclose) break;
    wait until !tasks.empty();
    pop task;
    task();
}
```

缺陷：

- `Pool::Isclose` 未初始化
- 析构中把 `Isclose=false`，不是 true
- worker detach，不 join
- `wait` 谓词只等 `!tasks.empty()`，关闭时无法唤醒退出
- 队列无上限，无背压和拒绝策略
- task 抛异常会导致线程终止
- 程序退出时 detached worker 可能访问已析构的 WebServer/HttpConn/SqlConnPool

## 20. 日志

当前 main 传 `openLog=false`，默认运行不启用日志。

源码支持：

- `Log::init` 设置日志级别、目录、后缀
- `maxQueueCapacity > 0` 时启用异步日志：创建 `BlockDeque<std::string>` 和日志线程
- `write` 构造日志行，异步时队列未满则 push，否则同步 `fputs`
- `flush` 异步时唤醒消费者并 `fflush`
- 析构时关闭队列并 join 日志线程

风险：

- `Log::write` 调 `AppendLogLevelTitle_(level_)`，使用的是全局 level_，不是本次日志 level 参数
- 队列满时会回退同步写；不能说日志完全无阻塞
- `BlockDeque::push_front` 有明显错误写法，但当前未发现被调用

## 21. 连接关闭

关闭入口：

- `EPOLLERR/EPOLLRDHUP/EPOLLHUP`
- `OnRead_` 中读失败或客户端关闭
- `OnWrite_` 写完且非 keep-alive
- timer 超时回调
- 最大连接数时发送 busy 后 close
- `WebServer` 析构 close listen fd

`CloseConn_`：

```text
epoller_->DleFd(fd)
-> client->Close()
```

`HttpConn::Close`：

- 如果未关闭：`response_.UnmapFile()`
- `isClose_=true`
- `userCount--`
- `::close(fd_)`

缺陷：

- `CloseConn_` 不从 timer 删除
- worker 持有 `HttpConn*` 时，主线程/timer 可能关闭同连接
- fd 被 OS 重用后，旧任务可能操作新连接对象
- `users_` 无锁

## 22. GET 完整案例

请求：

```http
GET /index.html HTTP/1.1
Host: localhost
Connection: keep-alive
```

流程：

```text
TCP 建连
-> listen fd 就绪
-> WebServer::DealListen_
-> accept 得到 client fd
-> WebServer::AddClient_
-> HttpConn::init
-> timer.add
-> SetFdNonblock
-> epoll AddFd EPOLLIN|connEvent_
-> EPOLLIN
-> DealRead_ 投递任务
-> OnRead_
-> HttpConn::read
-> Buffer::ReadFd(readv)
-> OnProcess
-> HttpConn::process
-> HttpRequest::parse
-> ParseRequestLine_/ParsePath_/ParseHeader_
-> HttpResponse::Init(srcDir, /index.html, keepAlive, 200)
-> MakeResponse: stat + mmap + 响应头
-> epoller ModFd EPOLLOUT
-> EPOLLOUT
-> DealWrite_ 投递任务
-> OnWrite_
-> HttpConn::write(writev)
-> 写完且 Keep-Alive
-> OnProcess；若无下一请求则重新 ModFd EPOLLIN
```

## 23. POST 登录完整案例

请求：

```http
POST /login.html HTTP/1.1
Content-Type: application/x-www-form-urlencoded
Content-Length: ...

username=xxx&password=xxx
```

流程：

```text
readv -> Buffer
-> HttpRequest::parse
-> ParseBody_
-> ParseFromUrlencoded_
-> ParsePost_
-> UserVerify(name,pwd,true)
-> SqlConnPool::GetConn
-> mysql_query SELECT ...
-> mysql_store_result/mysql_fetch_row
-> shared_ptr deleter 自动归还连接
-> 成功 path=/welcome.html，失败 path=/error.html
-> HttpResponse 按静态文件响应
```

SQL 风险：直接 `snprintf` 拼接 username/password，存在注入风险；应改为 prepared statement 和参数绑定，并对密码做哈希。

## 24. 异常流程

| 场景 | 当前处理 |
| --- | --- |
| socket 失败 | 已处理：close/return false |
| bind 失败 | 已处理：close/return false |
| listen 失败 | 已处理：close/return false |
| epoll_ctl add listen 失败 | 已处理：close/return false |
| epoll_ctl mod/del 失败 | 部分处理：返回 bool，但调用方大多不检查 |
| accept EAGAIN | 部分处理：fd<=0 直接 return，不区分 errno |
| accept EMFILE/ENFILE | 未专门处理 |
| read EAGAIN | 部分处理：OnRead 不关闭 |
| read 返回 0 | 有意图关闭，但 ET 下 `HttpConn::read` 可能空转 |
| read EINTR | 未专门处理 |
| write EAGAIN | 已处理：重新 EPOLLOUT |
| write EINTR | 未专门处理 |
| writev 部分写 | 部分处理：调整 iovec，但 `ToWriteBytes` 可能受 iov_[1] 旧值影响 |
| mmap 失败 | 部分处理：ErrorContent，但 fd 未 close |
| stat 失败 | 已处理为 404 |
| SQL 连接耗尽 | 部分处理：GetConn 返回 nullptr，UserVerify assert 可能终止 |
| SQL 执行失败 | 部分处理：返回 false |
| 线程池任务异常 | 未处理 |
| HTTP 请求过大 | 未处理 |
| 慢速请求 | 部分处理：timer 超时 |
| 半包 body | 未正确处理 Content-Length |
| fd 重用 | 未使用 generation 防护 |
| 定时器和 I/O 同时触发 | 存在竞争风险 |
| 服务器退出仍有任务 | 存在风险：detached worker |

## 25. 并发与生命周期风险

共享资源：

- `users_`：主线程插入/关闭，worker 持有内部对象指针；无锁
- `epoller_`：主线程 wait/add/del，worker mod；epoll_ctl 线程安全但对象生命周期无统一协调
- `timer_`：主线程操作为主；回调关闭连接
- `SqlConnPool`：mutex 保护队列，但关闭与 borrowed connection 存在生命周期风险
- `ThreadPool::tasks`：mutex/cv 保护，但无界、无优雅退出
- `Log`：mutex 保护 FILE 和 Buffer；默认未启用

最核心风险：任务 lambda 捕获裸 `HttpConn*`，而 `CloseConn_` 可能在任务执行前或执行中关闭 fd；fd 复用后，旧任务可能误操作新连接。

## 26. 性能分析

用户提供压测条件：

- 8 vCPU
- 4 GB Ubuntu 虚拟机
- wrk 与服务器同机
- 2 个 wrk 线程
- 10000 并发连接
- 平均约 14.6K QPS
- 平均延迟约 540 ms
- P99 约 1.02 s
- Socket 错误为 0

解释：

- 10000 并发连接不等于 10000 个线程；epoll 让一个主线程监控大量 fd，就绪后投递任务
- 也不等于 CPU 同时执行 10000 个请求；真正执行的是主线程和 6 个 worker
- QPS 是每秒完成请求数
- 平均延迟是整体平均；P99 是 99% 请求低于该延迟，更能反映尾延迟
- Little's Law 可粗略看：并发量约等于吞吐 * 平均响应时间；同机压测会让 wrk 和 server 竞争 CPU、内存、网络栈
- Socket 错误为 0 说明该压测条件下没有明显连接层错误；不能证明没有竞态、没有协议 bug、没有 SQL 注入

可能瓶颈：单 Reactor、任务队列、线程切换、正则解析、mmap 缺页、文件 I/O、MySQL、锁竞争、同机 wrk 抢资源。

建议定位：`top/htop`、`pidstat -t`、`perf record/report`、火焰图、`strace -c -p PID`、观察 `epoll_wait/readv/writev/futex/mmap/mysql_query` 占比。

## 27. 已知缺陷

1. `ThreadPool::Pool::Isclose` 未初始化，析构还设置 false，线程 detach，不支持优雅关闭。
2. `HttpConn::read` 在 ET 下遇到 `readv` 返回 0 可能死循环。
3. `HttpConn*` 被 worker lambda 裸指针捕获，连接关闭和 fd 复用时有生命周期风险。
4. `HttpRequest` 不根据 `Content-Length` 等待完整 body，半包 POST 可能被误解析。
5. 登录/注册 SQL 直接拼接字符串，存在 SQL 注入风险。
6. `InitEventMode_` 中 `HttpConn::isET=(connEvent_|EPOLLET)` 逻辑错误。
7. `Start` 先处理 IN/OUT，再处理 ERR/RDHUP/HUP，复合事件时可能先读写而不是关闭。
8. `mmap` 失败路径未 close 文件 fd。
9. `CloseConn_` 不删除 timer，fd 复用没有 generation 防护。
10. `users_` 无锁，主线程和 worker 对连接对象的访问缺少生命周期约束。

## 28. 改进方案

### 主从 Reactor

```text
Main Reactor:
    listen fd
    accept
    按轮询/负载把 client fd 分发给 Sub Reactor

Sub Reactor:
    每线程一个 epoll
    管理自己的连接 I/O 和 timer
```

需要 eventfd/pipe 做跨线程唤醒；连接对象归属某个 sub reactor，避免跨线程操作同一 epoll 和同一连接；timer 也放到连接所属线程。

### 优雅关闭线程池

- 初始化 `stop=false`
- 析构设置 `stop=true`
- 拒绝新任务
- `notify_all`
- worker 谓词 `stop || !tasks.empty`
- 选择处理完剩余任务或丢弃
- join 所有线程
- task 内捕获异常

### HTTP parser

- 用 Content-Length 判断 body 是否完整
- 支持 chunked 或明确返回 400/411
- 限制 header/body 最大长度
- 支持 pipeline 队列式解析
- Header key 大小写归一化
- 对非法请求生成错误响应

### 静态文件发送

- 当前：`mmap + writev`
- 可比较 `read + write`、`sendfile`、`splice`
- Linux 静态文件场景可考虑 `sendfile(fd, filefd, &off, len)` 减少用户态映射和页故障

### SQL 安全

- 使用 `mysql_stmt_prepare`
- 参数绑定 username/password
- 密码哈希加盐存储
- 连接获取支持超时
- 查询失败记录错误并返回明确页面

## 29. 一页总流程图

```text
main
-> WebServer 构造
   -> ThreadPool(detached workers)
   -> Epoller(epoll fd)
   -> HeapTimer
   -> SqlConnPool
   -> socket/bind/listen/epoll add
-> Start
   -> timer.GetNextTick
   -> epoll_wait
      -> listen fd: accept -> AddClient -> timer add -> epoll add EPOLLIN
      -> EPOLLIN: AddTask -> readv -> parse -> response/mmap -> ModFd EPOLLOUT
      -> EPOLLOUT: AddTask -> writev -> keepalive ? process next : close
      -> error/RDHUP/HUP: CloseConn
   -> loop forever
```

## 30. 源码阅读索引

建议顺序：

1. `src/main.cpp`
2. `server/webserver.h`
3. `src/webserver.cpp` 构造、`InitEventMode_`、`InitSocket_`、`Start`
4. `server/epoll.h`、`src/epoll.cpp`
5. `http/httpconn.h`、`src/httpconn.cpp`
6. `buffer/buffer.h`、`src/buffer.cpp`
7. `http/httprequest.h`、`src/httprequest.cpp`
8. `http/httpresponse.h`、`src/httpresponse.cpp`
9. `timer/heaptimer.h`、`src/heaptimer.cpp`
10. `Pool/threadpool.h`
11. `Pool/sqlconnpool.h`、`src/sqlconnpool.cpp`
12. `log/log.h`、`log/blockqueue.h`、`src/log.cpp`

## 31. 20 个理解检查题

1. 当前为什么是单 Reactor？
2. main 中 trigMode=1 对监听 fd 和连接 fd 分别意味着什么？
3. `EPOLLONESHOT` 解决了什么竞态？
4. 为什么 ET 要配合非阻塞？
5. `WebServer::Start` 中错误事件为什么可能被 IN/OUT 分支抢先处理？
6. `users_[fd]` 有什么生命周期风险？
7. `Buffer::ReadFd` 为什么用两个 iovec？
8. readv 是否等于零拷贝？
9. POST 半包为什么可能被误解析？
10. `HttpRequest::IsKeepAlive` 是否符合 HTTP/1.1 默认 keep-alive？
11. `mmap + writev` 避免了什么复制？不能避免什么？
12. `HttpConn::write` 如何处理响应头和文件的部分写？
13. timer 的 `ref_` 为什么需要？
14. fd 复用为什么会影响 timer 回调？
15. SQL 连接是如何归还的？
16. `GetConn` 连接耗尽时会阻塞吗？
17. 当前线程池为什么不能优雅关闭？
18. 默认运行是否启用日志？
19. 14.6K QPS 与 10000 并发连接是什么关系？
20. 如果要改成主从 Reactor，连接对象归属应该怎么设计？
