# 工作目标

分析 tiny-lsm 项目的真实源码和测试，并基于分析结果修改
resume/resume-zh_CN.tex，生成面向 C++ 后端、数据库内核和存储引擎实习的一页中文简历。

# 目录说明

- tiny-lsm/：项目源码，包含 LSM、WAL、事务、Redis 兼容层等实现。
- resume/：LaTeX 简历模板。
- resume/resume-zh_CN.tex：唯一需要主要修改的简历正文。

# 真实性要求

- 必须先阅读源码、测试、README、构建文件和 Git 修改记录。
- 只写能够从源码或测试中验证的已完成功能。
- 未实现的设计不能写成“已实现”，只能标记为“设计”或不写。
- 不得虚构性能、并发能力、事务正确性、测试结果或个人经历。
- 性能数字只能采用用户明确提供的数据，并注明测试条件。
- 不得宣称实现了完整 Redis，只能表述为兼容部分 RESP 命令和数据结构。

# 项目分析重点

重点检查：

- src/lsm、include/lsm
- src/wal、include/wal
- RedisWrapper、RESP Server
- Hash、Set、List、ZSet 相关代码
- 缓存实现
- WAL、MemTable、SSTable、Bloom Filter、BlockCache、Compaction
- 事务和隔离级别
- benchmark 或测试代码

# 简历要求

- 面向 C++ 后端、数据库内核、存储引擎实习。
- 保留原模板的类文件、宏和整体排版。
- 简历控制在一页。
- 每个项目要点使用“动作 + 技术方案 + 结果”的表达。
- 避免空泛表述和技术名词堆砌。
- LaTeX 特殊字符必须正确转义，例如 `_`、`%`、`&`。
- 修改完成后使用 XeLaTeX 编译并修复错误。
- 除非明确要求，不修改 tiny-lsm 源码。
