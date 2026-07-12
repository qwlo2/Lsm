# TingyWeb 面试准备材料

## 0. 证据范围与口径

本材料基于 `/home/qiu/Tinyweberever` 当前源码、构建脚本和用户提供的 wrk 数据整理。当前目录未发现 README；未发现独立测试目录；性能数据按用户提供记录引用。

主要扫描证据：

| 方向 | 文件 | 类/函数 | 调用关系 |
|---|---|---|---|
| Server 主循环 | `/home/qiu/Tinyweberever/server/webserver.h`、`/home/qiu/Tinyweberever/src/webserver.cpp` | `WebServer`、`Start`、`InitSocket_`、`InitEventMode_`、`DealListen_`、`DealRead_`、`DealWrite_`、`OnRead_`、`OnWrite_`、`OnProcess` | main -> WebServer 构造 -> socket/bind/listen/epoll -> accept -> 事件分发 -> 线程池处理 |
| epoll 封装 | `/home/qiu/Tinyweberever/server/epoll.h`、`/home/qiu/Tinyweberever/src/epoll.cpp` | `Epoller::AddFd/ModFd/DleFd/Wait` | WebServer 调用 Epoller 封装 `epoll_ctl`/`epoll_wait` |
| 连接对象 | `/home/qiu/Tinyweberever/http/httpconn.h`、`/home/qiu/Tinyweberever/src/httpconn.cpp` | `HttpConn::init/read/write/process/Close` | WebServer 中 `users_[fd]` 保存连接；工作线程读写；process 生成响应 |
| Buffer | `/home/qiu/Tinyweberever/buffer/buffer.h`、`/home/qiu/Tinyweberever/src/buffer.cpp` | `Buffer::ReadFd/WriteFd/Append/Retrieve` | `HttpConn::read` -> `Buffer::ReadFd` 使用 readv；响应头写入 writeBuff |
| HTTP 请求解析 | `/home/qiu/Tinyweberever/http/httprequest.h`、`/home/qiu/Tinyweberever/src/httprequest.cpp` | `HttpRequest::parse`、`ParseRequestLine_`、`ParseHeader_`、`ParseBody_`、`ParseFromUrlencoded_`、`UserVerify` | Buffer -> HTTP 状态机 -> 路由 -> POST 登录注册 -> MySQL |
| HTTP 响应 | `/home/qiu/Tinyweberever/http/httpresponse.h`、`/home/qiu/Tinyweberever/src/httpresponse.cpp` | `HttpResponse::MakeResponse`、`AddContent_`、`UnmapFile` | 静态文件 stat/open/mmap -> 响应头 + mmap 文件 |
| 定时器 | `/home/qiu/Tinyweberever/timer/heaptimer.h`、`/home/qiu/Tinyweberever/src/heaptimer.cpp` | `HeapTimer::add/adjust/tick/GetNextTick` | WebServer 为连接注册超时回调；epoll_wait timeout 由堆顶决定 |
| 线程池 | `/home/qiu/Tinyweberever/Pool/threadpool.h` | `ThreadPool::AddTask` | 主线程把 `OnRead_`/`OnWrite_` 任务提交到队列，工作线程消费 |
| MySQL 连接池 | `/home/qiu/Tinyweberever/Pool/sqlconnpool.h`、`/home/qiu/Tinyweberever/src/sqlconnpool.cpp` | `SqlConnPool::Init/GetConn/FreeConn/ClosePool` | HTTP POST 登录注册 -> `HttpRequest::UserVerify` -> `SqlConnPool::GetConn` |
| 日志 | `/home/qiu/Tinyweberever/log/log.h`、`/home/qiu/Tinyweberever/src/log.cpp`、`/home/qiu/Tinyweberever/log/blockqueue.h` | `Log::init/write/AsyncWrite_` | WebServer 构造可选开启；当前 `main.cpp` 传 `openLog=false` |
| 构建 | `/home/qiu/Tinyweberever/CMakeLists.txt` | CMake target `Tinywebserver` | C++14、`-O2`、链接 `pthread` 和 `mysqlclient` |
| 启动参数 | `/home/qiu/Tinyweberever/src/main.cpp` | `main` | `WebServer(1316, 1, 60000, false, ..., 12, 6, false, ...)` |

当前应保守表达的边界：

- 这是单 Reactor 风格实现：一个主 epoll 负责监听和连接事件，工作线程执行读写和业务处理；没有多个从 Reactor，也没有 Channel 类。
- HTTP 解析支持 GET、POST、URL-encoded 表单和静态资源，不能说完整 HTTP/1.1 协议栈。
- 日志支持同步/异步结构，但 `main.cpp` 当前 `openLog=false`，不能把异步日志作为默认运行路径亮点。
- 线程池是简化实现，`Pool::Isclose` 未显式初始化，析构中设置为 false 也不是真正关闭语义；不能夸成完善线程池生命周期。
- MySQL 登录注册使用字符串拼接 SQL，没有看到预编译/转义，存在 SQL 注入风险，面试里要主动承认。
- wrk 数据来自用户提供记录，当前项目内未找到 benchmark 文件。

## 1. 项目介绍

### 30 秒版本

TingyWeb 是一个 C++14 实现的轻量级 HTTP Web Server，核心是 epoll + 非阻塞 socket + 单 Reactor 事件循环。主线程负责 accept 和 epoll 事件分发，读写事件通过线程池处理；HTTP 层用 Buffer 解析请求，支持 GET 静态资源、POST 表单登录注册、Keep-Alive；响应侧用 mmap 映射静态文件，再通过 writev 一次发送响应头和文件内容，同时用小根堆定时器清理空闲连接。用户 wrk 记录显示，在 8 vCPU、4GB Ubuntu 虚拟机、同机压测、2 线程、10000 并发连接下，平均吞吐约 1.46 万 QPS。

### 1 分钟版本

这个项目解决的是“用 C++ 从 socket 到 HTTP 响应实现一个可压测的静态 Web Server”。启动时 `main.cpp` 构造 `WebServer`，`WebServer::InitSocket_` 完成 socket、bind、listen、SO_REUSEADDR 和非阻塞设置，`Epoller::AddFd` 把监听 fd 加到 epoll。`WebServer::Start` 是主事件循环，`epoller_->Wait` 返回事件后，监听 fd 走 `DealListen_` accept 新连接；连接读写事件分别走 `DealRead_` 和 `DealWrite_`，再通过 `ThreadPool::AddTask` 分发到工作线程执行 `OnRead_` 或 `OnWrite_`。

请求进入 `HttpConn::read`，底层 `Buffer::ReadFd` 使用 readv，把数据读到内部 buffer 和额外 64KB 临时缓冲区；`HttpRequest::parse` 用状态机解析请求行、请求头和 body。响应由 `HttpResponse::MakeResponse` 生成，静态文件通过 `mmap` 映射，`HttpConn::process` 把响应头和文件分别放进两个 `iovec`，`HttpConn::write` 使用 writev 发送。连接如果设置 Keep-Alive，会继续处理；超时连接由 `HeapTimer` 小根堆在 `GetNextTick` 和 `tick` 中清理。

### 3 分钟版本

背景上，TingyWeb 是一个面向网络编程学习和面试展示的 HTTP 服务器，重点不在业务复杂度，而在高并发连接管理、事件驱动、线程池、HTTP 请求解析和静态文件响应优化。

架构上分为五层：第一层是 `WebServer`，负责 socket 初始化、epoll 主循环、连接生命周期和任务分发；第二层是 `Epoller`，封装 `epoll_create`、`epoll_ctl`、`epoll_wait`；第三层是 `ThreadPool`，主线程遇到读写事件后将任务提交到队列，工作线程从队列取任务执行；第四层是 `HttpConn`，每个 fd 对应一个连接对象，持有读写 Buffer、请求对象和响应对象；第五层是 HTTP 业务层，`HttpRequest` 解析请求，`HttpResponse` 生成响应，登录注册通过 MySQL 连接池访问数据库。

完整请求流程是：客户端连到监听端口后，`DealListen_` 在 LT 或 ET 模式下 accept，`AddClient_` 初始化 `HttpConn`，设置非阻塞，把连接 fd 注册到 epoll，并通过 `HeapTimer::add` 加入超时管理。读事件到来时，主线程通过 `DealRead_` 把 `OnRead_` 提交给线程池；`OnRead_` 调 `HttpConn::read`，如果是 ET 模式会循环 readv 直到 EAGAIN；随后 `OnProcess` 调 `HttpConn::process`。如果解析成功，`HttpResponse::MakeResponse` 会 stat 文件、处理 404/403/200，并在 `AddContent_` 中用 mmap 映射文件。最后 `HttpConn::write` 使用 writev 发送响应头和文件两个缓冲区，写完后如果 Keep-Alive 则继续处理，否则关闭连接。

我重点能讲的是：第一，单 Reactor 模式下主线程和工作线程如何分工，以及为什么用 EPOLLONESHOT 避免一个连接被多个线程同时处理；第二，Buffer 使用 readv 的额外缓冲区来减少扩容和多次 read，响应侧 mmap + writev 减少静态文件复制和发送次数；第三，小根堆定时器如何用 fd->heap index 的 ref map 支持 add/adjust/tick，配合 epoll_wait timeout 清理空闲连接；第四，MySQL 连接池用队列、mutex 和 semaphore 复用连接，配合 `shared_ptr` 自定义 deleter 自动归还。

性能上，用户记录是在 8 vCPU、4GB Ubuntu 虚拟机上，wrk 与服务器同机，2 个 wrk 线程、10000 并发连接，平均吞吐约 14.6K QPS、平均延迟约 540ms、P99 约 1.02s、Socket errors 为 0。这个数据说明服务器能在高并发连接压力下维持响应且没有连接级错误，但同机压测会争抢 CPU，10000 并发也不等于 10000 个请求同时在 CPU 执行，540ms 不能描述成低延迟。

## 2. STAR 版本

**Situation：** 需要实现一个从网络 I/O 到 HTTP 静态资源响应的 C++ Web Server，展示 Linux 网络编程、epoll、线程池、HTTP 解析和资源管理能力。

**Task：** 搭建非阻塞 socket + epoll 事件循环，支持静态文件访问、Keep-Alive、POST 表单登录注册，并通过线程池和 I/O 优化提升并发连接处理能力。

**Action：**

- 在 `WebServer::InitSocket_` 中完成 socket/bind/listen、SO_REUSEADDR、非阻塞和 epoll 注册。
- 在 `WebServer::Start` 中实现主事件循环，监听 fd 由 `DealListen_` accept，连接读写事件通过 `DealRead_`/`DealWrite_` 投递到线程池。
- 用 `EPOLLONESHOT` 标记连接事件，工作线程处理完后通过 `Epoller::ModFd` 重新注册读/写，避免同一 fd 被多线程并发处理。
- 在 `Buffer::ReadFd` 中用 readv 同时读入内部缓冲区和额外栈缓冲区；在 `HttpResponse::AddContent_` 中 mmap 静态文件，`HttpConn::write` 用 writev 发送响应头和文件。
- 使用 `HeapTimer` 小根堆维护连接超时，`GetNextTick` 返回 epoll_wait timeout，超时后执行关闭回调。

**Result：** 用户 wrk 记录显示，8 vCPU、4GB Ubuntu VM、同机压测、2 线程、10000 并发连接下，平均吞吐约 1.46 万 QPS，平均延迟约 540ms，P99 约 1.02s，Socket errors 为 0。

**Reflection：** 项目能体现后端网络基础和性能意识，但当前仍是学习型服务器：没有多 Reactor、HTTP 解析覆盖有限、线程池生命周期和 SQL 安全性还需要完善。

## 3. 一张纸复习版

### 架构图

```text
Client
  -> listen fd
  -> WebServer::DealListen_ accept
  -> AddClient_: HttpConn init + nonblock + epoll add + timer add
  -> Epoller::Wait
  -> EPOLLIN: DealRead_ -> ThreadPool::AddTask -> OnRead_
       -> HttpConn::read -> Buffer::ReadFd(readv)
       -> HttpRequest::parse
       -> WebServer::OnProcess
       -> HttpResponse::MakeResponse -> mmap static file
       -> epoll MOD EPOLLOUT
  -> EPOLLOUT: DealWrite_ -> ThreadPool::AddTask -> OnWrite_
       -> HttpConn::write(writev)
       -> keep-alive: OnProcess / close: CloseConn_
  -> HeapTimer::GetNextTick/tick closes idle fd
```

### 5 个核心亮点

1. 单 Reactor + 线程池：主线程 epoll/accept/分发，工作线程处理连接读写和 HTTP 业务。
2. 非阻塞 I/O 与 EPOLLONESHOT：连接 fd 非阻塞，工作线程处理期间避免同一 fd 被重复调度。
3. Buffer/readv：内部缓冲区不足时，readv 直接读到额外栈缓冲区，减少多次系统调用和扩容。
4. mmap/writev 静态响应：响应头在 Buffer，文件通过 mmap，两个 iovec 由 writev 发送。
5. 小根堆定时器：fd->index ref map 支持 O(logN) add/adjust/pop，配合 epoll_wait timeout 清理空闲连接。

### 10 个最高频问题

1. Reactor 模型是什么？
2. 项目是单 Reactor 还是多 Reactor？
3. 主线程和工作线程分别做什么？
4. 为什么用 epoll 而不是 select/poll？
5. LT 和 ET 有什么区别？
6. EPOLLONESHOT 解决什么问题？
7. 半包/粘包如何处理？
8. readv、writev、mmap 分别优化什么？
9. Keep-Alive 如何实现？
10. wrk 的 10000 并发连接和 540ms 延迟怎么解释？

### 关键性能数字

- 环境：8 vCPU、4GB Ubuntu 虚拟机。
- 压测：wrk 与服务器同机，2 线程，10000 并发连接。
- 结果：平均吞吐约 1.46 万 QPS，平均延迟约 540ms，P99 约 1.02s，Socket errors 为 0。

### 3 个项目不足

- 单 Reactor，不是主从 Reactor；主线程仍可能成为 accept/epoll 分发瓶颈。
- HTTP 解析和请求完整性处理较简单，不是完整 HTTP/1.1 协议栈。
- 线程池关闭语义和 MySQL SQL 拼接安全性需要完善。

### 3 个未来改进点

- 引入多 Reactor：主 Reactor accept，从 Reactor 管理连接 I/O。
- 完善 HTTP parser：支持流式解析、Content-Length 完整性、pipeline、更多方法和错误处理。
- 优化线程池和数据库访问：安全关闭、任务背压、SQL 预处理、连接池超时。

## 4. 完整请求流程

### 4.1 建连与事件注册

```text
main
  -> WebServer::WebServer
  -> SqlConnPool::Init
  -> WebServer::InitEventMode_
       connEvent_ = EPOLLONESHOT | EPOLLRDHUP | optional EPOLLET
       listenEvent_ = EPOLLRDHUP | optional EPOLLET
  -> WebServer::InitSocket_
       socket -> setsockopt(SO_LINGER/SO_REUSEADDR)
       bind -> listen
       Epoller::AddFd(listenFd_, listenEvent_ | EPOLLIN)
       SetFdNonblock(listenFd_)
  -> WebServer::Start
       epoller_->Wait(timer_->GetNextTick())
```

源码证据：

- `src/main.cpp` 构造 `WebServer(1316, 1, 60000, false, ..., 12, 6, false, ...)`。
- `InitEventMode_(1)` 使连接事件启用 ET，监听 fd 不启用 ET。
- `InitSocket_` 使用 `socket`、`setsockopt`、`bind`、`listen`、`epoller_->AddFd`、`SetFdNonblock`。

### 4.2 accept 新连接

```text
WebServer::Start
  -> fd == listenFd_
  -> DealListen_
       accept
       AddClient_
          users_[fd].init(fd, addr)
          timer_->add(fd, timeout, CloseConn_)
          SetFdNonblock(fd)
          epoller_->AddFd(fd, EPOLLIN | connEvent_)
```

关键点：

- `users_` 是 `unordered_map<int, HttpConn>`，fd 到连接对象。
- `HttpConn::userCount` 是 atomic 计数。
- 超时回调捕获 fd，触发 `CloseConn_(&users_[fd])`。

### 4.3 读事件处理

```text
epoll returns EPOLLIN
  -> WebServer::DealRead_(HttpConn* client)
       ExtentTime_
       threadpool_->AddTask([this, client] { OnRead_(client); })
  -> WebServer::OnRead_
       client->read(&readErrno)
       if ret <= 0 && errno != EAGAIN: CloseConn_
       OnProcess(client)
  -> HttpConn::read
       do Buffer::ReadFd(fd, saveErrno) while isET
  -> Buffer::ReadFd
       readv(fd, {internal writable area, extra stack buffer[65535]})
```

为什么 readv 配额外缓冲区：

- 如果内部 buffer 剩余空间够，数据直接进入内部 buffer。
- 如果一次读超过内部可写空间，超出部分进入栈上的额外 buffer，再 append 到 vector。
- 相比循环多次 read 或先 resize 大 buffer，可以减少系统调用和不必要扩容。

### 4.4 HTTP 解析与业务处理

```text
WebServer::OnProcess
  -> HttpConn::process
       request_.Init
       request_.parse(readBuff_)
       response_.Init(srcDir, path, keepAlive, code)
       response_.MakeResponse(writeBuff_)
       iov_[0] = response header Buffer
       iov_[1] = mmap file if exists
       epoller_->ModFd(fd, EPOLLOUT or EPOLLIN)
```

HTTP parser：

- `HttpRequest::parse` 用 `PARSE_STATE` 在 REQUEST_LINE、HEADERS、BODY、FINISH 间推进。
- `ParseRequestLine_` 用 regex 解析 `METHOD path HTTP/version`。
- `ParseHeader_` 解析 `Header: value`。
- `ParseBody_` 支持 `application/x-www-form-urlencoded`，调用 `ParseFromUrlencoded_`。
- `ParsePost_` 对 `/register.html`、`/login.html` 调 `UserVerify`，成功跳转 `/welcome.html`，失败 `/error.html`。

边界：

- 当前 parser 对半包/粘包的处理依赖 Buffer 和 CRLF 搜索，但没有完整 HTTP parser 那样的 Content-Length 状态机和 pipeline 处理。

### 4.5 静态响应与发送

```text
HttpResponse::MakeResponse
  -> stat(srcDir + path)
  -> 404/403/200
  -> AddStateLine_
  -> AddHeader_
  -> AddContent_
       open file
       mmap file
       append Content-Length

HttpConn::write
  -> writev(fd, iov_, iovCnt_)
  -> 根据已写字节调整 iov_[0]/iov_[1]
  -> EAGAIN: epoller_->ModFd(fd, EPOLLOUT)
  -> 写完:
       keep-alive: OnProcess(client)
       else: CloseConn_
```

`mmap + writev` 的准确说法：

- mmap 减少用户态手动 read 文件内容的拷贝。
- writev 把响应头和文件映射区聚合发送，减少发送次数。
- 这不等于严格意义的全链路零拷贝；内核发送到网卡仍有协议栈处理。

### 4.6 Keep-Alive 与连接关闭

```text
HttpRequest::IsKeepAlive
  -> header["Connection"] == "keep-alive" && version == "1.1"

HttpResponse::AddHeader_
  -> Connection: keep-alive / close

WebServer::OnWrite_
  -> client->ToWriteBytes() == 0
  -> if client->IsKeepAlive(): OnProcess(client)
  -> else CloseConn_
```

注意：

- 当前 Keep-Alive 逻辑能复用连接，但 `OnWrite_` 写完后直接 `OnProcess`，如果没有新数据，`HttpConn::process` 返回 false 后重新监听 EPOLLIN。

### 4.7 超时清理

```text
WebServer::Start
  -> timeMS = timer_->GetNextTick()
  -> epoller_->Wait(timeMS)

HeapTimer::GetNextTick
  -> tick()
       while heap top expired:
           cb()
           pop()
  -> return time until next expires

读写事件
  -> WebServer::ExtentTime_
  -> HeapTimer::adjust(fd, timeoutMS_)
```

为什么用小根堆：

- 最近过期连接在堆顶，取下一次 epoll_wait timeout 很方便。
- add/adjust/pop 是 O(logN)。
- `ref_` 保存 fd 到 heap index，支持按 fd 更新超时时间。

## 5. 高频问题分级

### A 级：必须熟练回答

#### A1. Reactor 模型是什么？

**30 秒简答：** Reactor 是事件驱动模型：主线程等待 I/O 事件，事件到来后分发给对应 handler 处理。

**详细回答：** 在 TingyWeb 中，`WebServer::Start` 调 `Epoller::Wait` 等待事件；监听 fd 事件调用 `DealListen_`，连接读写事件调用 `DealRead_` 或 `DealWrite_`。handler 最终执行 `OnRead_`、`OnWrite_` 和 `OnProcess`。这就是事件检测和事件处理分离。

**追问：** Proactor 有什么不同？答：Proactor 是异步 I/O 完成后通知应用，Reactor 是 I/O 就绪后应用自己读写。

#### A2. 项目属于单 Reactor 还是多 Reactor？

**30 秒简答：** 单 Reactor。只有一个 `Epoller` 在 `WebServer` 中，主线程负责 accept 和所有连接 fd 的事件等待；工作线程只处理任务。

**详细回答：** `WebServer` 持有一个 `std::unique_ptr<Epoller> epoller_`。`Start` 中所有 fd 都从这个 epoll_wait 返回。没有 Channel 类，也没有多个 EventLoop 或 sub-reactor 分配连接。

**追问：** 多 Reactor 怎么改？答：主 Reactor 只 accept，把连接 fd 分发给多个从 Reactor，每个从 Reactor 有自己的 epoll 和线程。

#### A3. 主线程和工作线程分别做什么？

**30 秒简答：** 主线程负责 epoll_wait、accept、事件分发和 fd 注册修改；工作线程执行连接读写、HTTP 解析和响应构造。

**详细回答：** `DealRead_`/`DealWrite_` 不直接读写，而是 `threadpool_->AddTask([this, client]{ OnRead_(client); })` 或 `OnWrite_`。`OnRead_` 调 `HttpConn::read` 和 `OnProcess`；`OnWrite_` 调 `HttpConn::write`，处理 EAGAIN、Keep-Alive 和关闭。

**追问：** 主线程是否完全不做业务？答：基本不做 HTTP 解析和文件响应，但仍做 accept、timer 和 epoll_ctl。

#### A4. 为什么使用 epoll？

**30 秒简答：** epoll 适合大量连接，避免 select 的 fd 数量限制和每次线性扫描全部 fd。

**详细回答：** `Epoller` 封装 `epoll_ctl` 和 `epoll_wait`，内核只返回活跃事件数组，`WebServer::Start` 遍历活跃事件。相比 select/poll，在高并发长连接但活跃连接较少时更高效。

**追问：** epoll 一定更快吗？答：不是，少量 fd 下差异不明显；优势在大连接数和事件稀疏场景。

#### A5. LT 与 ET 有什么区别？

**30 秒简答：** LT 只要 fd 还有数据就反复通知；ET 只在状态变化时通知一次，需要非阻塞并循环读到 EAGAIN。

**详细回答：** `InitEventMode_` 根据 `trigMode` 给监听或连接事件加 EPOLLET。`HttpConn::read` 中 `do ReadFd while(isET)`，说明 ET 模式下需要一次性把数据读干。`DealListen_` 也在监听 ET 时循环 accept。

**追问：** 当前 main 用什么？答：`trigMode=1`，连接 fd ET，监听 fd 默认 LT。

#### A6. 为什么使用非阻塞 socket？

**30 秒简答：** 防止一个 fd 的 read/write/accept 阻塞整个事件循环或工作线程。

**详细回答：** `InitSocket_` 和 `AddClient_` 都调用 `SetFdNonblock`。ET 模式尤其要求非阻塞，否则循环读写可能卡死。写响应时如果 `writev` 返回 EAGAIN，会重新注册 EPOLLOUT 等待下次可写。

**追问：** 非阻塞读不到数据返回什么？答：返回 -1，errno 为 EAGAIN/EWOULDBLOCK。

#### A7. EPOLLONESHOT 解决什么问题？

**30 秒简答：** 防止同一个连接 fd 在多线程环境下被多个工作线程同时处理。

**详细回答：** `InitEventMode_` 中 `connEvent_ = EPOLLONESHOT | EPOLLRDHUP`。读事件触发后 fd 需要在处理完成后 `Epoller::ModFd` 重新注册 EPOLLIN 或 EPOLLOUT。这样同一连接不会在处理期间重复触发并被多个线程并发读写。

**追问：** 忘记 ModFd 会怎样？答：连接不会再收到后续事件。

#### A8. HTTP 请求如何判断完整？

**30 秒简答：** 当前通过 Buffer 中的 CRLF 分行，按状态机解析请求行、头、body；实现较简单，非完整 HTTP parser。

**详细回答：** `HttpRequest::parse` 使用 `std::search` 找 `\r\n`，状态从 REQUEST_LINE 到 HEADERS，到空行后进入 BODY/FINISH。POST body 处理依赖 `Content-Type`，没有严格按 Content-Length 流式等待完整 body。

**追问：** 半包怎么办？答：Buffer 会保留未取出的数据，但当前完整性判断仍偏简化；工业实现要按状态和 Content-Length 累积。

#### A9. Keep-Alive 如何实现？

**30 秒简答：** 请求头中 `Connection: keep-alive` 且 HTTP/1.1 时保持连接，响应头写 keep-alive，写完后重新监听读事件。

**详细回答：** `HttpRequest::IsKeepAlive` 检查 header 和 version；`HttpResponse::AddHeader_` 写 `Connection:keep-alive` 和 `Keep-alive:max=6,timeout=120`；`OnWrite_` 写完后如果 `client->IsKeepAlive()`，调用 `OnProcess` 继续处理，否则关闭。

**追问：** 空闲 Keep-Alive 怎么关？答：HeapTimer 超时回调关闭连接。

#### A10. readv 为什么需要额外缓冲区？

**30 秒简答：** 内部 Buffer 空间不够时，一次 readv 可把溢出部分读到额外缓冲区，减少多次 read 和扩容。

**详细回答：** `Buffer::ReadFd` 准备两个 iovec：第一个指向 `BeginWrite()`，第二个指向 `char buff[65535]`。如果读取长度超过内部可写空间，先把内部写满，再把额外 buffer append 进去。

**追问：** 为什么不一开始就把 Buffer 设很大？答：大量连接会浪费内存；按需扩容更节省。

#### A11. writev 有什么作用？

**30 秒简答：** 把多个不连续缓冲区一次写出，减少发送系统调用和拷贝整理。

**详细回答：** `HttpConn::process` 把响应头 `writeBuff_` 放到 `iov_[0]`，mmap 文件放到 `iov_[1]`。`HttpConn::write` 调 `writev(fd, iov_, iovCnt_)`，并根据已写字节调整 iovec。

**追问：** writev 能保证一次写完吗？答：不能，所以代码要处理部分写和 EAGAIN。

#### A12. mmap 为什么适合静态文件响应？

**30 秒简答：** 静态文件可直接映射到进程地址空间，避免先 read 到用户缓冲区再发送。

**详细回答：** `HttpResponse::AddContent_` 用 `open` 和 `mmap` 映射文件，`HttpConn::process` 将 `mmFile_` 放入 iovec。发送完成或连接关闭时 `HttpResponse::UnmapFile` 释放映射。

**追问：** mmap + writev 是否等于零拷贝？答：不完全等于，只是减少用户态文件读取拷贝和聚合发送。

#### A13. 定时器为什么使用小根堆？

**30 秒简答：** 最近超时连接总在堆顶，便于计算 epoll_wait timeout 和批量清理到期连接。

**详细回答：** `HeapTimer::GetNextTick` 先 `tick` 清理过期节点，再返回堆顶剩余毫秒数。`add/adjust/pop` 都通过堆调整，`ref_` 把 fd 映射到堆下标。

**追问：** 时间轮有什么优势？答：大量定时器、超时时间离散时，时间轮可接近 O(1)，但实现复杂。

#### A14. MySQL 连接池如何避免频繁建连？

**30 秒简答：** 启动时预创建连接放入队列，请求时取出，用完通过自定义 deleter 归还。

**详细回答：** `SqlConnPool::Init` 循环 `mysql_init/mysql_real_connect` 并 push 到 `connQue_`；`GetConn` 返回 `shared_ptr<MYSQL>`，deleter 里重新 push 回队列并 `sem_post`。`HttpRequest::UserVerify` 获取连接执行登录/注册 SQL。

**追问：** 当前 SQL 安全问题？答：字符串拼接 SQL，没有转义或 prepared statement，存在注入风险。

#### A15. 10000 并发连接意味着什么？

**30 秒简答：** 表示 wrk 同时维持大量连接，不等于 10000 个请求同时在 CPU 上运行。

**详细回答：** epoll 的价值就是管理大量 fd，活跃事件才被处理。wrk 的 10000 connections 给服务器连接管理、内存、fd、timer 和 epoll 分发施压，但真正 CPU 并行度受 wrk 线程、服务器线程、CPU 核数和请求处理时间限制。

**追问：** 为什么平均延迟 540ms 不能说低延迟？答：540ms 对 Web 服务已经明显偏高，只能说在高并发连接下无 socket 错误且吞吐达到约 1.46 万 QPS。

### B 级：源码深入问题

#### B1. `WebServer::Start` 的事件处理顺序有没有问题？

**30 秒简答：** 代码先判断 listen fd，再判断 EPOLLIN/EPOLLOUT，最后才处理 ERR/HUP；一般应优先处理错误事件更稳妥。

**详细回答：** 当前 `Start` 中 `else if (event_ & EPOLLIN)` 在 `EPOLLERR|EPOLLRDHUP|EPOLLHUP` 之前。如果一个 fd 同时有读和关闭事件，可能先进入读处理。更稳的写法是对非 listen fd 先判断错误/挂起。

**追问：** 这一定是 bug 吗？答：不一定，但属于鲁棒性风险。

#### B2. `HttpConn::read` 为什么在 ET 下循环？

**30 秒简答：** ET 只通知一次，如果不读到 EAGAIN，剩余数据可能不再触发事件。

**详细回答：** `do { len = readBuff_.ReadFd(...) } while(isET);`，在非阻塞 fd 上读到 EAGAIN 会返回负数并退出。LT 下读一次即可，因为没读完还会继续通知。

**追问：** 代码如何判断 EAGAIN？答：上层 `OnRead_` 看到 ret<=0 且 readErrno==EAGAIN 不关闭。

#### B3. `HttpConn::write` 如何处理部分写？

**30 秒简答：** 根据 writev 返回长度调整 `iov_[0]` 和 `iov_[1]` 的 base/len，没写完且 EAGAIN 就重新注册 EPOLLOUT。

**详细回答：** 如果写出长度超过响应头长度，说明文件区也写了一部分，代码清空 writeBuff 并移动 `iov_[1].iov_base`；否则只移动 `iov_[0]` 和 retrieve header buffer。

**追问：** 为什么需要 `ToWriteBytes()`？答：判断是否还有待发送字节，决定继续写、注册 EPOLLOUT 或关闭。

#### B4. `HttpRequest::parse` 的状态机怎么走？

**30 秒简答：** REQUEST_LINE 解析方法路径版本，HEADERS 解析请求头，BODY 解析表单，FINISH 表示完成。

**详细回答：** `ParseRequestLine_` 正则匹配后设置 `state_=HEADERS`；`ParseHeader_` 匹配 header，遇到非 header 行切到 BODY；`ParseBody_` 保存 body，必要时 URL decode，再调用 `ParsePost_` 做登录注册路由。

**追问：** GET 请求什么时候 FINISH？答：headers 读完且可读字节小于等于 4 时设 FINISH。

#### B5. URL-encoded 表单如何解析？

**30 秒简答：** `ParseFromUrlencoded_` 遍历 body，把 `+` 转空格，按 `=` 和 `&` 切 key/value，并处理 `%xx` 十六进制转义。

**详细回答：** 函数维护 key、value、j 下标，遇到 `=` 记录 key，遇到 `&` 插入 `post_`，遇到 `%` 调 `ConverHex` 并 erase 后两个字符。

**追问：** 有什么边界？答：缺少非法 `%` 编码、重复字段、超长 body 的完整错误处理。

#### B6. `HttpResponse::MakeResponse` 如何决定状态码？

**30 秒简答：** stat 文件失败或目录返回 404，无读权限返回 403，否则 200。

**详细回答：** `MakeResponse` 对 `srcDir_ + path_` 调 `stat`，失败或 `S_ISDIR` 设 404；无 `S_IRUSR` 设 403；否则 200。`ErrorHtml_` 会把 400/403/404 映射到对应错误页面。

**追问：** 找不到错误页面怎么办？答：当前代码没有很完整的 fallback，可能继续走 AddContent_ 的 File NotFound。

#### B7. 连接生命周期如何管理？

**30 秒简答：** fd 对应 `users_[fd]` 中的 `HttpConn`，关闭时从 epoll 删除、unmap 文件、关闭 fd、userCount--。

**详细回答：** `AddClient_` 初始化连接并注册 timer/epoll；`CloseConn_` 调 `epoller_->DleFd` 和 `client->Close`；`HttpConn::Close` 调 `response_.UnmapFile` 和 `::close(fd_)`。

**追问：** fd 复用有什么风险？答：timer 回调捕获 fd，如果 fd 被关闭并复用，要确保旧 timer 被正确删除或覆盖，否则可能关闭新连接。当前 `HeapTimer::add` 对已存在 id 会更新，但关闭时没有显式从 timer 删除，存在可讨论风险。

#### B8. 线程池任务队列如何同步？

**30 秒简答：** `AddTask` 用 mutex push 任务，再 `notify_one`；工作线程拿 unique_lock 等待 cv，队列非空后 pop 并执行。

**详细回答：** `ThreadPool` 内部 `Pool` 包含 mutex、condition_variable 和 queue。构造时启动线程并 detach。任务执行在锁外，避免长时间持锁。

**追问：** 当前线程池有什么缺陷？答：`Isclose` 未初始化，析构设置 false 而不是 true，线程 detach，生命周期不严谨。

#### B9. `HeapTimer::GetNextTick` 和 epoll_wait 怎么配合？

**30 秒简答：** 每轮 epoll_wait 前先清理过期定时器，然后返回距离下一次过期的毫秒数作为 epoll_wait timeout。

**详细回答：** `WebServer::Start` 如果 `timeoutMS_>0` 就 `timeMS=timer_->GetNextTick()`；`GetNextTick` 调 `tick`，若堆非空计算 `heap_[0].expires - now`。这样即使没有 I/O 事件，超时也会唤醒清理连接。

**追问：** 没有 timer 怎么办？答：返回 -1，epoll_wait 可一直阻塞。

#### B10. 为什么 `EPOLLONESHOT` 处理后要 `ModFd`？

**30 秒简答：** ONESHOT 事件触发一次后自动失效，必须重新 arm。

**详细回答：** `OnProcess` 处理成功后 `ModFd(fd, connEvent_|EPOLLOUT)`，失败则 `ModFd(fd, connEvent_|EPOLLIN)`；`OnWrite_` 遇到 EAGAIN 也 `ModFd(... EPOLLOUT)`。这就是重新关注下一阶段事件。

**追问：** 如果两个线程同时 ModFd？答：ONESHOT 降低这个风险，但连接生命周期仍需要谨慎管理。

#### B11. `mmap` 后什么时候释放？

**30 秒简答：** `HttpResponse::UnmapFile` 在响应初始化、析构和连接关闭时释放。

**详细回答：** `HttpResponse::Init` 如果 `mmFile_` 存在会先 `UnmapFile`；`HttpConn::Close` 调 `response_.UnmapFile`；`HttpResponse::~HttpResponse` 也调用 `UnmapFile`。

**追问：** 大量大文件 mmap 有什么风险？答：虚拟地址空间、页缓存压力、缺页开销，需要限流或 sendfile 等替代。

#### B12. MySQL 连接池的 semaphore 用得是否完美？

**30 秒简答：** 它能表达连接数限制，但当前 `GetConn` 在持 mutex 时调用 `sem_wait`，实现比较粗糙。

**详细回答：** `GetConn` 先 lock，队列空则返回 nullptr，否则 `sem_wait`，pop 连接。由于空队列已判断，一般不会阻塞；但更典型写法是先 `sem_wait` 再加锁取连接，避免锁与信号量组合复杂化。

**追问：** 自定义 deleter 的好处？答：业务拿 `shared_ptr<MYSQL>`，离开作用域自动归还连接。

### C 级：压力追问

#### C1. 高并发下最大的竞态在哪里？

**30 秒简答：** fd 生命周期、timer 回调、工作线程中的 client 指针和 epoll 事件重注册。

**详细回答：** `DealRead_` 捕获 `HttpConn* client` 投递到线程池，如果连接同时超时关闭，指针仍可能被任务使用。EPOLLONESHOT 减少同 fd 并发处理，但 timer 与任务生命周期仍需要更严谨的引用管理。

**追问：** 怎么改？答：用 shared_ptr 管理连接对象，timer callback 持 weak_ptr，关闭时取消 timer。

#### C2. 为什么不是多 Reactor？

**30 秒简答：** 当前实现更简单，适合学习；但多核和高连接数下，单 epoll 主线程可能成为瓶颈。

**详细回答：** 单 Reactor 所有 fd 事件都从一个 epoll_wait 返回，主线程还要做 epoll_ctl 和 timer。多 Reactor 可以把连接分散到多个 epoll，每个线程处理自己的 fd，减少主线程压力。

**追问：** 改成多 Reactor 的核心改动？答：新增 EventLoop/Channel，把 accept 后的 fd 分配给 sub-loop，每个 loop 自己 epoll_wait。

#### C3. 半包粘包现在是否完全解决？

**30 秒简答：** TCP 层用 Buffer 累积数据，但 HTTP parser 完整性处理不够工业化。

**详细回答：** Buffer 不会因一次 read 不完整就丢数据；但 `HttpRequest::parse` 按 CRLF 行解析，POST body 没有严格依据 Content-Length 等待完整 body，对 pipeline 支持也不足。

**追问：** 如何完善？答：明确解析状态、已读 body 长度、Content-Length、chunked、pipeline 队列。

#### C4. `mmap + writev` 与 `sendfile` 怎么比较？

**30 秒简答：** mmap+writev 能聚合 header 和文件，但仍需要页映射和 write；sendfile 更接近内核态文件到 socket 的零拷贝。

**详细回答：** 当前选择 mmap 方便把文件作为 iovec，与响应头一起 writev。进一步优化可用 `sendfile` 或 `splice`，或者 `sendfile` 发送文件、writev 发送 header。

**追问：** 为什么不一定马上改？答：当前代码结构简单，mmap+writev 已能展示 I/O 优化，sendfile 需要更细致处理 header 和部分发送。

#### C5. 为什么平均延迟 540ms？

**30 秒简答：** 10000 并发连接压力很高，同机压测竞争 CPU，单 Reactor、线程池队列、静态文件 I/O、timer 和上下文切换都可能拉高排队延迟。

**详细回答：** wrk 同机意味着客户端和服务器共享 CPU。6 个工作线程处理 10000 连接，任务队列排队会增长；单 epoll 主线程要处理大量事件；如果静态文件未完全热缓存，mmap 缺页也会增加延迟。

**追问：** 如何降低 P99？答：多 Reactor、减少锁、使用 sendfile、预热静态文件、减少日志、调大 fd/backlog、连接分片、优化线程池队列。

#### C6. Socket errors 为 0 说明什么？

**30 秒简答：** 在这次压测条件下，连接建立/读写没有出现 wrk 统计的连接级错误。

**详细回答：** 它说明服务端没有大量 reset、timeout、read/write error，但不能证明协议完全正确、没有内存问题、没有尾延迟问题，也不能证明在跨机或更长时间下稳定。

**追问：** 还需要哪些指标？答：CPU、内存、fd 数、上下文切换、p99/p999、错误码分布、长稳测试。

#### C7. 项目和 Nginx 的差距是什么？

**30 秒简答：** Nginx 是成熟多进程/多事件循环服务器，协议、配置、模块、缓存、sendfile、TLS、稳定性都远超该项目。

**详细回答：** TingyWeb 展示网络基础机制，但没有完整 HTTP、TLS、反向代理、成熟内存池、负载均衡、多 worker 进程和大量边界处理。不能说超过 Nginx。

**追问：** 面试里怎么表达？答：说“参考高性能服务器常见机制做了学习型实现”。

#### C8. 如果 MySQL 连接池满了怎么办？

**30 秒简答：** 当前 `GetConn` 如果队列空会返回 nullptr，业务 assert(sql)，鲁棒性不足。

**详细回答：** `HttpRequest::UserVerify` 调 `GetConn` 后 `assert(sql)`，连接池满或连接失败会导致断言。更稳妥做法是等待带超时、返回错误页面或降级。

**追问：** 连接池还要什么能力？答：健康检查、重连、超时、最大等待队列、指标。

#### C9. 线程池为什么 detach 有风险？

**30 秒简答：** detach 后主对象析构时无法 join 控制线程退出，可能访问已销毁资源。

**详细回答：** 当前 `ThreadPool` 构造中线程 detach，析构只 notify_all，但 `Isclose` 没有正确置 true。工业实现应该保存 thread 对象，析构时设置关闭标志、notify_all、join。

**追问：** 面试能主动说吗？答：可以，说明你知道项目边界和改进方向。

#### C10. 如何继续提升吞吐？

**30 秒简答：** 多 Reactor 分摊 epoll，优化线程池，减少锁和内存拷贝，使用 sendfile，预热静态资源，完善连接生命周期。

**详细回答：** 当前单 Reactor 下所有 fd 由一个 epoll 管理；可以引入 sub-reactor，accept 后 round-robin 分配连接。响应可从 mmap+writev 进一步到 sendfile。线程池可改有界队列和 work stealing。HTTP parser 可以减少 regex 使用。

**追问：** 最优先改哪个？答：如果目标是高并发静态文件，优先多 Reactor + sendfile + 连接生命周期安全。

## 6. 性能结果解读

用户提供 wrk 数据：

- 环境：8 vCPU、4GB Ubuntu 虚拟机。
- wrk 与 server 同机。
- wrk 参数：2 线程，10000 并发连接。
- 平均吞吐：约 14.6K QPS。
- 平均延迟：约 540 ms。
- P99：约 1.02 s。
- Socket errors：0。

解释口径：

- 同机压测需要注明，因为 wrk 客户端和 server 争抢同一台 VM 的 CPU、内存和网络栈资源。
- 10000 并发连接表示同时维持大量 TCP 连接，不等于 10000 个请求同时在 CPU 上执行。
- 540ms 不能描述为低延迟，只能说在高连接压力下维持了约 1.46 万 QPS。
- Socket errors 为 0 说明本次测试下 wrk 未观察到连接级错误；不能证明长时间稳定、协议完整或无内存问题。
- 可能瓶颈：单 Reactor 分发、线程池队列排队、regex 解析、mmap 缺页、同机压测 CPU 竞争、MySQL 或日志如果开启。

## 7. 模拟面试：连续追问版

**Q1：介绍一下 TingyWeb。**  
我的回答：这是一个 C++14 HTTP Server，使用非阻塞 socket、epoll、单 Reactor 和线程池。主线程负责 accept 和 epoll 分发，工作线程处理读写和 HTTP 解析；静态文件通过 mmap 映射，writev 发送响应头和文件；小根堆定时器清理空闲连接。

更好的回答：补充它是学习型 Web Server，不是完整 HTTP/生产服务器，并给出 wrk 条件和数据。

易踩坑：说成多 Reactor 或超过 Nginx。

**Q2：请求从连接到响应怎么走？**  
我的回答：`InitSocket_` 监听端口，`Start` epoll_wait；监听 fd 事件 `DealListen_` accept，新连接 `AddClient_` 注册 epoll 和 timer；读事件进线程池 `OnRead_`，解析 HTTP，生成响应；写事件 `OnWrite_` 用 writev 发送。

追问：哪个类保存连接状态？  
更好的回答：`HttpConn` 保存 fd、addr、readBuff、writeBuff、HttpRequest、HttpResponse 和 iovec。

易踩坑：把 Buffer 和 HttpConn 职责混淆。

**Q3：为什么是单 Reactor？**  
我的回答：只有一个 `WebServer::epoller_`，所有 fd 都在一个 epoll 中等待；工作线程只处理任务，不拥有自己的 epoll。

追问：多 Reactor 怎么做？  
更好的回答：主 Reactor accept 后把 fd 分配给多个 sub Reactor，每个 sub Reactor 一个 epoll 和事件循环。

易踩坑：把线程池等同于多 Reactor。

**Q4：EPOLLONESHOT 为什么必要？**  
我的回答：读写事件交给线程池后，如果 fd 继续在 epoll 中可触发，可能多个线程同时处理一个连接。ONESHOT 触发一次后失效，处理完再 ModFd。

追问：忘记 ModFd 呢？  
更好的回答：连接后续读写事件不会再触发。

易踩坑：只说“提高性能”，不说并发安全。

**Q5：ET 模式为什么必须非阻塞？**  
我的回答：ET 只通知一次，所以要循环读写到 EAGAIN；如果 fd 是阻塞的，循环可能卡住线程。

追问：代码哪里体现？  
更好的回答：`HttpConn::read` 的 `do ReadFd while(isET)`，`write` 中 EAGAIN 重新注册 EPOLLOUT。

易踩坑：说 ET 一定比 LT 快，不讲使用条件。

**Q6：readv 优化了什么？**  
我的回答：一次系统调用把数据读到内部 buffer 和额外栈 buffer，避免内部空间不足时多次 read 或频繁扩容。

追问：额外 buffer 多大？  
更好的回答：源码里 `char buff[65535]`，约 64KB。

易踩坑：说 readv 是零拷贝。

**Q7：mmap + writev 怎么协作？**  
我的回答：响应头写进 `writeBuff_`，文件由 mmap 映射，两个地址放到 `iov_[0]` 和 `iov_[1]`，writev 聚合发送。

追问：是不是零拷贝？  
更好的回答：不是严格零拷贝，只是避免手动 read 文件到用户缓冲区并减少 write 次数。

易踩坑：把 mmap+writev 吹成全链路零拷贝。

**Q8：Keep-Alive 怎么实现？**  
我的回答：解析请求头 `Connection: keep-alive` 和 HTTP/1.1，响应头写 keep-alive，写完后不关闭连接，而是继续监听下一次请求。

追问：空闲连接怎么回收？  
更好的回答：`HeapTimer` 小根堆定时器，读写时 adjust，超时 tick 执行关闭回调。

易踩坑：忘记 timer。

**Q9：wrk 的 10000 并发和 540ms 怎么解释？**  
我的回答：10000 是连接并发，不是 CPU 同时跑 10000 请求；同机压测会竞争 CPU。540ms 说明尾部和排队已经明显，不应该说低延迟。

追问：Socket errors 为 0 说明什么？  
更好的回答：说明这次压测下没有连接级错误，但不能证明长稳和完整协议正确性。

易踩坑：只报 QPS，不提条件。

**Q10：你会优先优化哪里？**  
我的回答：先做多 Reactor 分摊 epoll 和连接事件，再改线程池生命周期和连接对象 shared_ptr/weak_ptr 管理，静态文件可考虑 sendfile，HTTP parser 改为流式状态机。

追问：为什么不是先优化 MySQL？  
更好的回答：静态文件压测路径通常不经过 MySQL；如果业务压测是登录注册，再优化 SQL 和连接池。

易踩坑：不区分压测路径。

