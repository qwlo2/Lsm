# TingyWeb 六天学习笔记

学习目标：按真实源码调用链理解 TingyWeb，而不是背类名。每一天都建议边读源码边画图，最后用自测题检查。

## 第一天：程序启动与 Socket

### 学习目标

- 看懂 `main` 如何创建 `WebServer`
- 理解构造函数中创建了哪些资源
- 掌握 socket、bind、listen、SO_REUSEADDR、SO_LINGER、非阻塞

### 必须读的文件

1. `/home/qiu/Tinyweberever/src/main.cpp`
2. `/home/qiu/Tinyweberever/server/webserver.h`
3. `/home/qiu/Tinyweberever/src/webserver.cpp`
4. `/home/qiu/Tinyweberever/CMakeLists.txt`

### 建议阅读函数顺序

```text
main
-> WebServer::WebServer
-> WebServer::InitEventMode_
-> WebServer::InitSocket_
-> WebServer::SetFdNonblock
-> WebServer::Start
```

### 需要画出的流程图

```text
main
-> 构造 WebServer
-> 初始化 ThreadPool/Epoller/HeapTimer
-> 初始化 MySQL 连接池
-> 初始化事件模式
-> socket/bind/listen
-> epoll add listen fd
-> Start
```

### 关键问题

- main 当前端口是多少？
- `trigMode=1` 对 listen fd 和 client fd 分别意味着什么？
- `SO_REUSEADDR` 和 `SO_REUSEPORT` 有什么区别？
- `SO_LINGER` 当前有没有启用？
- 为什么 listen backlog 不等于最大并发连接数？

### 自测题

1. 当前 `WebServer` 构造函数实际设置的静态资源目录是什么？
2. `WebServer` 构造阶段创建了哪三个 `unique_ptr` 成员？
3. `SqlConnPool::Init` 的 host 是 main 传入的吗？
4. `InitSocket_` 中非阻塞是在 epoll add 之前还是之后设置？
5. socket 初始化失败后程序如何避免进入正常服务状态？

### 自测答案

<details>
<summary>展开答案</summary>

1. `/home/qiu/Tinyweberever/resources`。
2. `timer_`、`threadpool_`、`epoller_`。
3. 不是，构造函数里写死为 `"192.168.1.6"`。
4. 监听 fd 是先 `epoller_->AddFd`，后 `SetFdNonblock`。
5. `InitSocket_` 返回 false，构造函数设置 `isClose_=true`，`Start` 循环不会正常运行。

</details>

## 第二天：epoll 与 Reactor

### 学习目标

- 理解 Epoller 封装
- 理解 Reactor 与 Proactor 区别
- 确认当前是单 Reactor，不是多 Reactor
- 理解 `EPOLLET`、`EPOLLONESHOT`、`EPOLLRDHUP`

### 必须读的文件

1. `/home/qiu/Tinyweberever/server/epoll.h`
2. `/home/qiu/Tinyweberever/src/epoll.cpp`
3. `/home/qiu/Tinyweberever/src/webserver.cpp`

### 建议阅读函数顺序

```text
Epoller::Epoller
-> Epoller::AddFd
-> Epoller::ModFd
-> Epoller::DleFd
-> Epoller::Wait
-> WebServer::InitEventMode_
-> WebServer::Start
```

### 需要画出的流程图

```text
主线程
-> epoll_wait
-> 判断 fd/event
-> listen: accept
-> EPOLLIN/EPOLLOUT: 投递线程池
-> error/RDHUP/HUP: 关闭
```

### 关键问题

- epoll 返回就绪事件还是完成事件？
- 为什么当前是 Reactor？
- 为什么只有一个 `Epoller` 就是单 Reactor？
- ONESHOT 如何避免两个 worker 同时处理同一 fd？
- 处理完为什么必须 `ModFd`？

### 自测题

1. `Epoller` 构造函数调用的是 `epoll_create` 还是 `epoll_create1`？
2. `Epoller::Wait` 的 timeout 从哪里来？
3. 当前 `Start` 对事件分支的判断顺序有什么问题？
4. `InitEventMode_` 中设置 `HttpConn::isET` 的代码有什么 bug？
5. worker 线程会不会调用 `epoller_->ModFd`？

### 自测答案

<details>
<summary>展开答案</summary>

1. `epoll_create(5)`。
2. `timer_->GetNextTick()`。
3. 先判断 `EPOLLIN/EPOLLOUT`，最后才判断 `EPOLLERR/EPOLLRDHUP/EPOLLHUP`，复合事件可能先读写。
4. 写成了 `HttpConn::isET=(connEvent_|EPOLLET)`，位或后几乎总为 true；应判断 `connEvent_ & EPOLLET`。
5. 会。`OnProcess`、`OnWrite_` 中可能调用 `epoller_->ModFd`，而它们运行在 worker。

</details>

## 第三天：连接、线程池和 Buffer

### 学习目标

- 看懂 accept 到 `HttpConn::init`
- 理解 `users_`、timer、epoll 注册的关系
- 理解线程池任务投递
- 理解 Buffer 和 readv

### 必须读的文件

1. `/home/qiu/Tinyweberever/src/webserver.cpp`
2. `/home/qiu/Tinyweberever/http/httpconn.h`
3. `/home/qiu/Tinyweberever/src/httpconn.cpp`
4. `/home/qiu/Tinyweberever/Pool/threadpool.h`
5. `/home/qiu/Tinyweberever/buffer/buffer.h`
6. `/home/qiu/Tinyweberever/src/buffer.cpp`

### 建议阅读函数顺序

```text
WebServer::DealListen_
-> accept
-> WebServer::AddClient_
-> HttpConn::init
-> ThreadPool::AddTask
-> WebServer::DealRead_
-> WebServer::OnRead_
-> HttpConn::read
-> Buffer::ReadFd
```

### 需要画出的流程图

```text
listen fd ready
-> accept client fd
-> users_[fd].init
-> timer.add
-> SetFdNonblock
-> epoll AddFd EPOLLIN
-> EPOLLIN
-> AddTask
-> worker readv
```

### 关键问题

- `users_` 为什么用 fd 做 key？
- `users_[fd]` 可能导致什么容器行为？
- worker lambda 捕获 `HttpConn*` 有什么风险？
- readv 的两个 iovec 分别指向哪里？
- 为什么 readv 不是零拷贝？

### 自测题

1. `HttpConn::init` 会不会增加 `userCount`？
2. 新连接什么时候加入 timer？
3. `ThreadPool` 的 worker 是 join 还是 detach？
4. `ThreadPool::Pool::Isclose` 有初始化吗？
5. `Buffer::ReadFd` 读取超过内部可写空间时如何处理？

### 自测答案

<details>
<summary>展开答案</summary>

1. 会，`userCount++`。
2. `AddClient_` 中 `users_[fd].init` 后，`timer_->add`。
3. detach。
4. 没有显式初始化，而且析构时还设成 false，这是缺陷。
5. 先把内部 buffer 写满，再把栈上临时缓冲区中的超出部分 `Append` 进 vector。

</details>

## 第四天：HTTP 解析与响应

### 学习目标

- 理解 `HttpRequest` 状态机
- 理解路径映射和登录/注册
- 理解 `HttpResponse` 如何 stat、mmap、构造响应
- 理解 writev 的 iovec 调整

### 必须读的文件

1. `/home/qiu/Tinyweberever/http/httprequest.h`
2. `/home/qiu/Tinyweberever/src/httprequest.cpp`
3. `/home/qiu/Tinyweberever/http/httpresponse.h`
4. `/home/qiu/Tinyweberever/src/httpresponse.cpp`
5. `/home/qiu/Tinyweberever/src/httpconn.cpp`

### 建议阅读函数顺序

```text
HttpConn::process
-> HttpRequest::parse
-> ParseRequestLine_
-> ParseHeader_
-> ParseBody_
-> ParsePost_
-> HttpResponse::Init
-> HttpResponse::MakeResponse
-> HttpConn::write
```

### 需要画出的流程图

```text
REQUEST_LINE
-> HEADERS
-> BODY
-> FINISH
-> MakeResponse
-> stat
-> mmap
-> iov[0]/iov[1]
-> writev
```

### 关键问题

- 请求行正则是什么？
- Header 是否大小写不敏感？
- `Content-Length` 是否用于等待完整 body？
- `/login` 如何变成 `/login.html`？
- mmap 能不能称为严格零拷贝？

### 自测题

1. `ParsePath_` 中 `/` 会映射到什么？
2. `IsKeepAlive` 对 Header 的判断是否区分大小写？
3. POST body 半包当前能正确等待吗？
4. `HttpResponse::MakeResponse` 什么时候返回 403？
5. `HttpConn::write` 中 `iov_[0]` 和 `iov_[1]` 分别保存什么？

### 自测答案

<details>
<summary>展开答案</summary>

1. `/index.html`。
2. 区分。只查精确 key `"Connection"` 和 value `"keep-alive"`。
3. 不能。源码没有根据 `Content-Length` 判断 body 完整性。
4. 文件存在但 `!(st_mode & S_IRUSR)`。
5. `iov_[0]` 是响应头 Buffer，`iov_[1]` 是 mmap 的静态文件内容。

</details>

## 第五天：定时器、数据库和资源管理

### 学习目标

- 理解小根堆 timer
- 理解 MySQL 连接池和 shared_ptr deleter
- 理解连接关闭完整路径
- 审计对象生命周期

### 必须读的文件

1. `/home/qiu/Tinyweberever/timer/heaptimer.h`
2. `/home/qiu/Tinyweberever/src/heaptimer.cpp`
3. `/home/qiu/Tinyweberever/Pool/sqlconnpool.h`
4. `/home/qiu/Tinyweberever/src/sqlconnpool.cpp`
5. `/home/qiu/Tinyweberever/src/webserver.cpp`
6. `/home/qiu/Tinyweberever/src/httpconn.cpp`

### 建议阅读函数顺序

```text
HeapTimer::add
-> HeapTimer::adjust
-> HeapTimer::GetNextTick
-> HeapTimer::tick
-> WebServer::CloseConn_
-> HttpConn::Close
-> SqlConnPool::Init
-> SqlConnPool::GetConn
```

### 需要画出的流程图

```text
新连接 -> timer.add
读写事件 -> timer.adjust
主循环 -> GetNextTick -> tick -> cb -> CloseConn_

UserVerify -> GetConn -> mysql_query -> shared_ptr 析构 -> 归还连接
```

### 关键问题

- `ref_` 为什么能让 timer 按 fd 调整？
- `CloseConn_` 有没有删除 timer？
- fd 复用可能导致什么问题？
- `GetConn` 队列空时会阻塞还是返回 nullptr？
- 连接池关闭时 borrowed connection 是否安全？

### 自测题

1. TimerNode 保存哪三个信息？
2. `HeapTimer::GetNextTick` 会先做什么？
3. `SqlConnPool::GetConn` 返回的 `shared_ptr` 自定义 deleter 做什么？
4. `CloseConn_` 是否先从 epoll 删除 fd？
5. `HttpConn::Close` 是否会 munmap 文件？

### 自测答案

<details>
<summary>展开答案</summary>

1. `id`、`expires`、`cb`。
2. 先调用 `tick()` 清理过期节点。
3. 把 MYSQL* push 回连接队列，并 `sem_post`。
4. 是，调用 `epoller_->DleFd(client->GetFd())`。
5. 会，调用 `response_.UnmapFile()`。

</details>

## 第六天：性能与缺陷

### 学习目标

- 正确解释 wrk 压测数据
- 知道哪些结论不能从压测推出
- 能列出当前项目的核心缺陷
- 能提出下一步改造方案

### 必须读的文件

1. `/home/qiu/Tinyweberever/src/webserver.cpp`
2. `/home/qiu/Tinyweberever/Pool/threadpool.h`
3. `/home/qiu/Tinyweberever/src/httpconn.cpp`
4. `/home/qiu/Tinyweberever/src/httprequest.cpp`
5. `/home/qiu/Tinyweberever/src/httpresponse.cpp`
6. `/home/qiu/Tinyweberever/src/sqlconnpool.cpp`
7. `docs/tingyweb_full_flow.md`
8. `docs/tingyweb_call_graph.md`

### 建议阅读函数顺序

```text
WebServer::Start
-> DealRead_/DealWrite_
-> ThreadPool::AddTask
-> HttpConn::read/write
-> HttpRequest::parse
-> UserVerify
-> CloseConn_
```

### 需要画出的流程图

```text
单 Reactor 性能路径：
epoll_wait -> task queue -> worker -> read/parse/write -> ModFd

缺陷路径：
timer close / worker task / fd reuse / users_ no lock
```

### 关键问题

- 10000 并发连接为什么不等于 10000 线程？
- 平均延迟和 P99 分别说明什么？
- Socket 错误为 0 能证明什么，不能证明什么？
- 当前最可能的性能瓶颈是什么？
- 如果要改成主从 Reactor，连接对象归属如何设计？

### 自测题

1. 当前模型为什么容易被单 Reactor 限制？
2. 同机 wrk 压测会争抢哪些资源？
3. 为什么不能说项目已经完整解决线程安全？
4. 当前 SQL 为什么有注入风险？
5. 你会优先修哪 5 个 bug？

### 自测答案

<details>
<summary>展开答案</summary>

1. 只有一个主线程负责所有 fd 的 epoll_wait、accept、分发和 timer，事件入口集中。
2. CPU、内存、网络协议栈、调度资源、cache。
3. `users_` 无锁，worker 捕获裸 `HttpConn*`，timer/worker/主线程可能同时处理同 fd，线程池无法优雅退出。
4. `UserVerify` 用 `snprintf` 把用户名和密码直接拼进 SQL，没有 prepared statement 和参数绑定。
5. 建议优先修：线程池 stop/join；`HttpConn::read` 0 返回空转；`HttpConn*` 生命周期；HTTP Content-Length；SQL prepared statement。也可以加 fd generation 和 timer 删除。

</details>

## 最终复盘清单

学完后你应该能不看笔记说清：

1. 从 `main` 到 `Start` 的真实初始化顺序。
2. listen fd 与 client fd 的 epoll 事件差异。
3. ONESHOT 为什么存在，以及在哪里重新激活。
4. `readv` 如何进入 Buffer。
5. HTTP parser 为什么不能完整支持半包 body。
6. `mmap + writev` 的真实收益与边界。
7. Keep-Alive 的状态转换。
8. timer 如何影响 `epoll_wait` timeout。
9. SQL 连接如何借出和归还。
10. 当前最重要的并发和生命周期缺陷。
