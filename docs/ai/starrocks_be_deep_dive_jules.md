# StarRocks Backend (BE) 深度剖析

**Jules @ 2025-09-08**

---

## 目录

### 第一章：引言

#### 1.1 研究背景与动机
StarRocks 作为一个高性能的分析型数据库，其卓越的查询性能和对实时场景的强大支持，越来越多地吸引了社区和企业的关注。其核心竞争力在于其 Backend（BE）模块的存储和执行引擎。理解 BE 的内部工作原理，对于深度使用、性能调优、问题排查以及二次开发都至关重要。然而，官方文档往往侧重于用户功能和高层架构，对于源码级别的实现细节和设计思想着墨不多。本文档旨在弥补这一差距，通过对 StarRocks BE 模块的源码进行一次全面、系统、由表及里的深度剖析，为开发者和架构师提供一份详实的内部参考。

#### 1.2 StarRocks 架构概览
StarRocks 采用经典的 MPP（大规模并行处理）架构，主要由 Frontend（FE）和 Backend（BE）两类进程组成。
- **Frontend (FE)**: 负责元数据管理、查询解析、规划、调度以及集群管理。它是集群的“大脑”，不存储实际的表数据。
- **Backend (BE)**: 负责数据的存储和计算。它是集群的“四肢”，负责执行 FE 下发的计划分片，并直接管理本地磁盘上的数据。

#### 1.3 BE 的角色与核心职责
BE 是 StarRocks 中最为复杂的模块，其核心职责包括：
- **分布式存储**: 以 Tablet 为单位，管理数据的多副本存储。
- **计算执行**: 执行 FE 下发的物理计划，完成数据的扫描、过滤、聚合、连接等计算任务。
- **数据导入**: 负责处理来自各种数据源的导入请求，并将数据持久化。
- **后台维护**: 执行 Compaction、垃圾回收、数据恢复等任务，保证系统的长期稳定。

#### 1.4 本文结构
本文将围绕 BE 的核心机制展开，结构安排如下：
- **第二章：数据写入流程与分布式一致性**: 剖析一次数据写入请求从进入 BE 到最终多副本确认的全过程。
- **第三章：存储引擎架构与核心数据结构**: 深入分析 Tablet、Rowset、Segment 等核心数据结构的设计与实现。
- **第四章：查询处理与一致性保证**: 阐述查询时如何通过 MVCC 实现快照隔离，以及数据的读取路径。
- **第五章：后台任务与系统维护**: 揭示 Compaction、数据恢复等后台任务的工作原理。
- **第六章：总结与展望**: 对 BE 的整体设计思想进行总结，并展望未来的优化方向。

### 第二章：数据写入流程与分布式一致性

StarRocks 的数据写入是一个精心设计的分布式过程，旨在确保数据在多副本间的高效、一致和可靠的持久化。本章将以最常用的 Stream Load 为例，深入剖析其从接收请求到最终提交的完整端到端流程，并揭示其背后三副本写入的分布式一致性原理。

#### 2.1 写入模型概览

StarRocks 支持多种数据导入方式，主要包括：
- **Stream Load**: 通过 HTTP 协议流式上传本地文件或数据流，适用于高频、实时的数据注入。本章主要分析此模型。
- **Broker Load**: 通过 Broker 进程读取外部存储系统（如 HDFS, S3）的数据进行批量导入，适用于大数据量的离线导入。
- **Insert**: 标准的 SQL `INSERT` 语句，通常用于小批量数据写入或 ETL 过程。

尽管入口不同，但它们在 BE 端的底层写入流程最终会收敛，核心都是将数据转化为内部格式，并通过多副本写入协议持久化。

#### 2.2 Stream Load 详解：一次写入的旅程

一次 Stream Load 请求从 HTTP 协议开始，到最终数据落盘，其在 BE 内部的旅程可以分为**发送端**和**接收端**两个视角。发送端指最初接收用户 HTTP 请求的那个 BE 节点（也称协调节点），接收端指最终负责写入数据的各个副本所在的 BE 节点。

##### 2.2.1 发送端 BE：请求的接收、规划与分发

当用户向一个 BE 节点发起 Stream Load 的 PUT 请求时，数据流转正式开始。

**1. HTTP 入口: `StreamLoadAction`**

- **文件**: `be/src/http/action/stream_load.h`, `be/src/http/action/stream_load.cpp`
- **职责**: 作为 HTTP 请求的直接处理者，它负责解析请求头、管理加载上下文，并启动整个写入流程。
- **核心逻辑**:
    - `on_header()`: 当请求头到达时，此方法被调用。它会解析出数据库、表、Label、认证信息以及各种加载参数（如列分隔符、文件格式等），并创建一个 `StreamLoadContext` 对象来跟踪整个加载任务的状态。
    - **开启事务**: `_on_header()` 内部会调用 `_exec_env->stream_load_executor()->begin_txn(ctx)`，这会向 FE 发起 RPC 请求，在元数据中开启一个新事务并获取全局唯一的 `txn_id`。这是写入流程在集群层面的正式开始。
    - **获取执行计划**: 接着，它调用 `_process_put()`，将所有加载参数打包成 `TStreamLoadPutRequest`，通过 RPC 向 FE 请求执行计划。FE 作为“大脑”，会进行权限校验、表结构检查等，并返回一个为此次写入量身定制的 `TPlanFragment`（计划分片）。
    - **启动执行**: 拿到计划后，调用 `_exec_env->stream_load_executor()->execute_plan_fragment(ctx)`，在 BE 本地建立一个数据处理管道，管道的“源头”是 `StreamLoadPipe`（一个内存队列），“终点”是 `OlapTableSink`。
    - `on_chunk_data()`: 随着 HTTP 的数据块不断到达，此方法被反复调用，它作为**生产者**将数据送入 `StreamLoadPipe`。

**2. 数据分发中枢: `OlapTableSink`**

- **文件**: `be/src/exec/tablet_sink.h`, `be/src/exec/tablet_sink.cpp`
- **职责**: 作为数据流管道的终点，`OlapTableSink` 是连接执行引擎和存储引擎的桥梁。它负责将上游传来的数据块（Chunk）根据规则路由到正确的副本节点。
- **核心逻辑**:
    - **初始化**: 在 `init()` 和 `prepare()` 方法中，它从 FE 返回的计划中解析出目标表的 Schema、分区、副本位置等所有元信息。
    - **路由计算**: `_send_chunk()` 是其核心方法。它接收数据块，并调用 `_vectorized_partition->find_tablets()` 为每一行数据计算其所属的 Partition 和 Bucket，最终确定它应该发往的 `tablet_id`。
    - **委托发送**: 计算完路由信息后，它并不直接进行网络发送，而是将数据块和路由信息一同交给其成员 `_tablet_sink_sender` 处理。

**3. 数据发送者: `TabletSinkSender` & `NodeChannel`**

- **文件**: `be/src/exec/tablet_sink_sender.h`, `be/src/exec/tablet_sink_index_channel.h`
- **职责**:
    - `TabletSinkSender`: 负责将一个数据块按目标 BE 节点进行分组。
    - `NodeChannel`: 负责与**单一**目标 BE 节点进行通信。它将发往该节点的数据攒批（Batching），并通过 brpc 异步地调用目标 BE 上的 `tablet_writer_add_chunk` RPC 接口。
- **核心逻辑**:
    - `TabletSinkSender::_send_chunk_by_node()` 遍历所有副本所在的 BE 节点，为每个节点筛选出需要发送给它的那些行。
    - `NodeChannel::add_chunk()` 接收到这些行后，将它们追加到内部的发送缓冲区 `_cur_chunk`。
    - 当缓冲区满或 flush 时，`NodeChannel` 将 `_cur_chunk` 序列化为 Protobuf 格式，封装成 `PTabletWriterAddChunkRequest`，并最终通过 `_stub` 发送出去。

至此，数据在协调节点（发送端 BE）的旅程结束，通过网络流向了存储副本的各个接收端 BE。

##### 2.2.2 接收端 BE：数据的接收、写入与持久化

当一个接收端 BE 收到来自协调节点的 `tablet_writer_add_chunk` RPC 请求时，真正的写入存储引擎的流程开始。

**1. RPC 服务入口: `BackendInternalServiceImpl`**

- **文件**: `be/src/service/service_be/internal_service.cpp`
- **职责**: 作为 RPC 服务的实现者，它接收来自其他 BE 的写入请求。
- **核心逻辑**: 它本身不处理复杂的逻辑，而是直接将请求转发给 `LoadChannelMgr`。
  ```cpp
  void BackendInternalServiceImpl::tablet_writer_add_chunk(...) {
      _exec_env->load_channel_mgr()->add_chunk(*request, response);
  }
  ```

**2. 写入通道管理: `LoadChannelMgr` & `LoadChannel`**

- **文件**: `be/src/runtime/load_channel_mgr.h`, `be/src/runtime/load_channel.h`
- **职责**:
    - `LoadChannelMgr`: 管理该 BE 上所有加载任务的通道。它持有一个从 Load ID 到 `LoadChannel` 的映射。
    - `LoadChannel`: 代表一个加载任务在当前 BE 上的上下文。它再进一步持有从 Index ID 到 `TabletsChannel` 的映射，以区分对主表和物化视图的写入。
- **核心逻辑**: `LoadChannelMgr` 根据请求中的 Load ID 找到对应的 `LoadChannel`，然后 `LoadChannel` 再根据 Index ID 找到对应的 `LocalTabletsChannel`，并将数据块传递下去。

**3. Tablet 通道: `LocalTabletsChannel`**

- **文件**: `be/src/runtime/local_tablets_channel.h`
- **职责**: 管理对**单个索引**下、发往**一组 Tablet** 的写入过程。这是数据写入 `DeltaWriter` 之前的最后一站。
- **核心逻辑**: `LocalTabletsChannel` 持有一个从 Tablet ID 到 `AsyncDeltaWriter` 的映射 (`_delta_writers`)。当数据块到达时，它根据 Tablet ID 找到对应的 `AsyncDeltaWriter` 并调用其 `write` 方法。

**4. 最终写入者: `AsyncDeltaWriter` & `DeltaWriter`**

- **文件**: `be/src/storage/async_delta_writer.h`, `be/src/storage/delta_writer.h`
- **职责**:
    - `AsyncDeltaWriter`: 这是一个异步封装器。它内部维护了一个 `bthread::ExecutionQueue` 任务队列，将 `write` 调用变成一个异步任务并放入队列，从而立即返回，避免阻塞上游。
    - `DeltaWriter`: **写入路径的终点**。它负责对单个 Tablet 的实际写入操作。
- **核心逻辑**:
    - `DeltaWriter::write()`: 将数据块写入内存中的 `_mem_table`。
    - **刷盘 (Flush)**: 当 `_mem_table` 写满时，`DeltaWriter` 会调用 `_flush_memtable()`，该方法通过 `_rowset_writer` 将 `MemTable` 中的数据写入一个新的 Segment 文件。
    - **提交 (Commit)**: 当一次写入（一个 Rowset）完成后，`DeltaWriter::commit()` 被调用，它通过 `_rowset_writer->build()` 完成 Rowset 的构建，并通过 `TxnManager::commit_txn` 将事务的元数据持久化，使写入最终生效。

#### 2.3 三副本写入与 Quorum 协议

StarRocks 通过三副本机制保证数据的高可用和容错。其写入协议是一种基于 Quorum 的类 Paxos 变种，确保了数据的一致性。

- **Pipeline 复制**: `OlapTableSink` 会为目标 Tablet 的三个副本分别建立一个 `NodeChannel`。当数据需要写入时，它会将同一份数据同时发送给这三个 `NodeChannel`。
- **Quorum 机制**: 在 `TabletSinkSender` 和 `NodeChannel` 层面，通过 `_write_quorum_type`（通常是 `MAJORITY`）和 `_has_intolerable_failure()` 等方法来判断写入是否成功。默认情况下，只要多数派（2个）副本写入成功，协调节点就会认为本次写入成功，并可以向客户端返回成功。
- **版本号 (Version)**:
    - 每次成功的写入都会生成一个**新的、单调递增的 Rowset 版本号**。例如，初始版本是 `[0-1]`，第一次写入成功后变为 `[2-2]`，第二次成功后变为 `[3-3]`。
    - 这个版本号由 FE 全局统一分配和管理，在 `commit_txn` 阶段返回给 BE。
    - BE 将这个版本号与新生成的 Rowset 绑定，并记录在 `Tablet` 的元数据中（`_rs_version_map`）。
- **“发布-应用”两阶段提交**:
    1.  **写入阶段**: 数据被写入各个副本的 `DeltaWriter`，生成新的 Rowset 文件，但此时这些文件对查询不可见。
    2.  **提交阶段 (`commit_txn`)**: 当协调节点收到多数派副本的成功响应后，会向 FE 发起 `commit_txn` 请求。
    3.  **发布阶段 (`publish_version`)**: FE 确认 `commit` 后，会向所有副本下发 `publish_version` 任务，通知它们将对应 `txn_id` 的 Rowset 版本号正式“发布”，使其对后续的查询可见。

通过这一系列机制，StarRocks 确保了即使在有副本失败的情况下，数据也能成功写入，并且所有副本之间的数据状态最终能够通过版本号达成一致。

#### 2.4 主键模型写入与部分更新

主键模型的写入与普通模型最大的不同在于，它需要支持 Upsert（更新/插入）和 Delete 操作，并且需要保证主键的唯一性。

- **主键索引 (`PrimaryIndex`)**: 每个主键表 Tablet 都会维护一个 `PrimaryIndex`（通常是基于 RocksDB 的持久化索引），用于存储主键到其数据位置（RowID，即 Rowset ID + 行号）的映射。
- **`TabletUpdates`**: 主键表的写入流程不直接使用 `DeltaWriter`，而是通过 `TabletUpdates` 类来管理。
    - **伪“WAL”机制**: 当主键表有写入时，`TabletUpdates::rowset_commit` 会被调用。它并不立刻更新主键索引，而是将这次写入（包含新的 Rowset）封装成一个 `EditVersionInfo`，并将其记录在一个持久化的日志中。这个日志起到了 Write-Ahead Log 的作用，确保了写入的持久性。
    - **异步 Apply**: 后台的 Apply 线程会消费这个 `EditVersionInfo` 日志，异步地将变更应用到 `PrimaryIndex` 中：对于新数据，更新索引；对于老数据，在 `DelVector` 中记录删除标记。
- **部分更新**: 主键模型还支持列模式的部分更新。此时，新写入的 Rowset 只包含部分更新的列，查询时需要将新旧 Rowset 的数据进行合并，才能得到完整的行。

通过引入 `PrimaryIndex` 和 `TabletUpdates` 的日志+异步 Apply 机制，StarRocks 在实现主键模型强大的 Upsert 功能的同时，也保证了写入的性能和数据一致性。

### 第三章：存储引擎架构与核心数据结构

StarRocks 的存储引擎是其高性能分析能力的核心。它围绕一系列精心设计的数据结构构建，实现了高效的列式存储、数据压缩、索引以及版本管理。本章将深入剖析这些核心概念及其在源码中的实现。

#### 3.1 核心概念：Tablet, Rowset, Segment

StarRocks 的物理数据组织可以概括为三层结构：Tablet -> Rowset -> Segment。

##### 3.1.1 Tablet: 逻辑数据管理单元

- **文件**: `be/src/storage/tablet.h`, `be/src/storage/tablet.cpp`
- **定义**: `Tablet` 是 StarRocks 中数据管理、副本和并发控制的基本逻辑单元。一张表根据其分区和分桶规则，在物理上被切分为多个 `Tablet`。每个 `Tablet` 的一个副本独立地存储在一个 BE 节点上。
- **核心职责**:
    - **元数据管理**: `Tablet` 类持有一个 `TabletMetaSharedPtr` (`_tablet_meta`)，其中包含了 Tablet 的所有持久化元数据，如 Schema、版本信息、所有 Rowset 的元数据列表等。这些元数据最终通过 `KVStore`（RocksDB 的封装）进行持久化。
    - **Rowset 集合管理**: `Tablet` 在内存中通过 `_rs_version_map` 维护了一个从 `Version` 到 `RowsetSharedPtr` 的映射。这是 Tablet 已发布数据的核心视图。当写入新数据或 Compaction 完成后，新的 Rowset 会被原子性地添加到这个 map 中。
    - **版本管理**: `Tablet` 通过 `_timestamped_version_tracker`（一个版本图）来管理版本历史。查询时，通过 `capture_consistent_rowsets()` 方法，可以根据指定的版本号，从版本图中获取一个一致性的 Rowset 列表，从而实现快照隔离。
    - **并发控制**: `Tablet` 内部定义了多种锁（`_meta_lock`, `_ingest_lock`, `_compaction_lock` 等），用于协调并发的读、写和后台合并任务，确保操作的正确性和隔离性。

##### 3.1.2 Rowset: 一次写入的物理数据集合

- **文件**: `be/src/storage/rowset/rowset.h`, `be/src/storage/rowset/rowset.cpp`
- **定义**: `Rowset` 代表了一批在逻辑上被同时导入或合并的数据。它由一个 `RowsetMeta` 元数据对象和一至多个物理的 `Segment` 文件组成。每次成功的写入（如一次 Stream Load 或一次 Compaction）都会生成一个新的 `Rowset`。
- **核心职责**:
    - **数据聚合**: `Rowset` 将多个 `Segment` 文件在逻辑上聚合为一个整体。对于查询而言，它只需要关心 `Rowset`，而无需了解其内部有多少个 `Segment`。
    - **元数据**: `RowsetMeta` 记录了该 `Rowset` 的元信息，包括其版本号、所属的 `Tablet`、包含的 `Segment` 文件列表、行数、数据大小、是否重叠等。
    - **生命周期管理**: `Rowset` 实现了引用计数（`acquire`/`release`），确保在有查询正在使用它时，其物理文件不会被后台的 Compaction 任务删除。

##### 3.1.3 Segment: 列存文件单元

- **文件**: `be/src/storage/rowset/segment.h`, `be/src/storage/rowset/segment.cpp`
- **定义**: `Segment` 是 StarRocks 中最小的物理存储单元，是一个独立的文件。一个 `Rowset` 可以包含多个 `Segment` 文件。
- **核心职责**:
    - **列式存储**: `Segment` 内部以列式格式存储数据。每一列的数据被独立存储，并可以采用不同的编码和压缩方式。这使得查询时只需读取所需的列，大大减少了 I/O 开销。
    - **索引存储**: `Segment` 文件内部不仅包含数据，还包含了多种索引，如稀疏索引、布隆过滤器、位图索引等。这些索引与数据存储在一起，保证了数据和索引的一致性。
    - **数据读取**: `Segment` 提供了 `new_iterator` 方法，可以创建一个 `SegmentIterator`，用于从该文件中读取数据。

#### 3.2 写入器层次结构：从内存到磁盘

数据的物理写入过程通过一个写入器（Writer）的层次结构来完成，这与 `Tablet -> Rowset -> Segment` 的数据结构相对应。

- **`DeltaWriter`**: 位于最高层，对应 `Tablet`。它负责处理单个事务内对单个 Tablet 的写入，管理 `MemTable`，并在 `MemTable` 满时触发刷盘。
- **`RowsetWriter`**: 由 `DeltaWriter` 创建和管理，对应 `Rowset`。它负责创建一个新的 `Rowset`，管理其生命周期，并创建 `SegmentWriter` 来写入具体的 `Segment` 文件。
- **`SegmentWriter`**: 位于最底层，对应 `Segment`。它接收来自 `RowsetWriter` 的数据块（Chunk），将其按列式格式组织、编码、压缩，并写入到物理的 `Segment` 文件中。同时，它也负责生成并写入该 `Segment` 的各种索引。

这个层次结构清晰地分离了不同层级的职责，使得整个写入流程模块化且易于管理。

#### 3.3 元数据管理

- **`TabletMeta`**: 每个 `Tablet` 的所有持久化信息都封装在 `TabletMeta` 对象中。这包括 TabletID、Schema、分区信息、删除条件、以及最重要的——一个包含了所有 `RowsetMeta` 的列表。
- **`RowsetMeta`**: 记录了单个 `Rowset` 的所有元信息，如版本、行数、Segment 文件列表等。
- **`KVStore` (RocksDB)**: BE 的所有元数据，包括 `TabletMeta`，都存储在一个 Key-Value 存储中。StarRocks 将其封装在 `KVStore` 类中，其默认实现是 RocksDB。每个 `Tablet` 的元数据以 `tablet_id` 为 Key 进行存储。这种方式利用了 RocksDB 的事务性和高写入性能，来保证元数据操作的原子性和持久性。
- **一致性保障**:
    - 当一次写入成功并生成一个新的 `Rowset` 后，`Tablet::add_rowset` 方法会先将新的 `RowsetMeta` 添加到 `TabletMeta` 对象中，然后调用 `_tablet_meta->save_meta()` 将整个 `TabletMeta` 对象序列化并原子性地写入 RocksDB。
    - 只有当 `TabletMeta` 成功写入 RocksDB 后，这次写入才算真正持久化。如果在写入 RocksDB 前发生崩溃，由于 `TabletMeta` 没有被更新，新生成的 `Rowset` 文件就成了孤儿文件，会在后续的垃圾回收中被清理，从而保证了数据状态的一致性。

### 第四章：查询处理与一致性保证

StarRocks 的查询性能与数据一致性是其作为一款优秀 OLAP 引擎的基石。本章将剖析查询在 BE 端的执行流程，重点解释其如何通过多版本机制实现快照隔离（Snapshot Isolation），以及数据从磁盘到上层算子的完整读取路径。

#### 4.1 查询流程概览

一个查询的生命周期始于 FE，终于结果返回。在 BE 端，其核心流程如下：
1.  **接收计划分片**: BE 接收来自 FE 的 `TPlanFragment`，其中包含了为该 BE 定制的执行子树。对于底层数据扫描，这个计划的核心是 `OlapScanNode`。
2.  **创建执行器**: BE 的 `FragmentMgr` 会为该计划分片创建一个 `PlanFragmentExecutor`，负责调度和执行。
3.  **数据扫描**: `OlapScanNode` 开始执行，向存储引擎请求数据。
4.  **数据处理**: 数据从 `OlapScanNode` 流出，经过上层的其他执行节点（如聚合、排序、连接等）进行处理。
5.  **结果发送**: 最终结果通过 `DataStreamSender` 或 `ResultSink` 发送给其他 BE 节点或直接返回给 FE。

本章的核心，是深入探索第 3 步——`OlapScanNode` 是如何与存储引擎交互以保证一致性并高效读取数据的。

#### 4.2 快照隔离与版本管理

StarRocks 采用多版本并发控制（MVCC）来实现读写不阻塞的快照隔离。其核心思想是，每次查询都只读取某个特定历史版本的数据快照，不受并发写入和合并的影响。

##### 4.2.1 查询版本号的获取

- **FE 的角色**: 当一个查询到达 FE 时，FE 会向元数据（`GlobalStateMgr`）查询，获取当前所有分区**最新且已成功发布的版本号**。这个版本号将作为此次查询的目标版本。
- **下发计划**: FE 将这个版本号打包进 `TInternalScanRange` 结构体中，并通过 `TPlanFragment` 下发给相关的 BE 节点。
- **BE 的接收**: BE 端的 `OlapScanNode` 在 `prepare()` 阶段，通过 `set_scan_ranges` 方法接收到这些包含 `version` 的扫描范围信息。至此，BE 明确了本次查询需要读取的数据版本。

##### 4.2.2 一致性 Rowset 的捕获

- **文件**: `be/src/exec/olap_scan_node.cpp`, `be/src/storage/tablet.cpp`
- **核心方法**: `OlapScanNode::_capture_tablet_rowsets()` -> `Tablet::capture_consistent_rowsets()`
- **实现原理**:
    1.  **获取版本路径**: `Tablet::capture_consistent_rowsets()` 首先调用 `_timestamped_version_tracker.capture_consistent_versions()`。`TimestampedVersionTracker` 内部维护了一个**版本图（Version Graph）**，记录了所有 Rowset 版本之间的连接关系。此方法会在图中寻找一条从初始版本（-1）到查询指定的 `version` 的完整路径。
    2.  **路径的意义**: 这条路径代表了构成该版本快照所需的所有 Rowset 的版本链。例如，若指定版本为 10，版本图中的路径可能是 `[0-1]`, `[2-5]`, `[6-8]`, `[9-10]`，那么这四个版本对应的 Rowset 就是构成版本 10 的数据快照。
    3.  **版本丢失**: 如果由于 Compaction，某个中间版本（如 `[6-8]`）已被合并成 `[0-8]`，那么直接查找版本 10 的路径就会失败。`capture_consistent_versions` 会智能地寻找替代路径。但如果连合并后的版本也无法构成连续路径（例如数据已损坏或被GC），则会返回 "version not found" 错误。
    4.  **获取 Rowset 指针**: 拿到 `version_path` 后，`Tablet::_capture_consistent_rowsets_unlocked()` 会遍历这个路径，从 `_rs_version_map` 中查找每个 `Version` 对应的 `RowsetSharedPtr`，并将这些智能指针存入一个 `vector` 中返回给 `OlapScanNode`。
    5.  **保证生命周期**: `OlapScanNode` 将这些 `RowsetSharedPtr` 保存在其成员 `_tablet_rowsets` 中。只要 `OlapScanNode` 存在，这些 `shared_ptr` 就会使对应 Rowset 的引用计数大于 0，从而确保即使后台 Compaction 删除了这些 Rowset 的元数据，它们的物理文件也不会被清理，保证了查询期间数据的可用性。

通过这个“获取版本路径 -> 捕获 Rowset 智能指针”的机制，StarRocks 完美地实现了快照隔离。

#### 4.3 数据读取路径

当 `OlapScanNode` 拿到了 Rowset 列表后，`TabletScanner` 会创建一个 `TabletReader` 来负责实际的数据读取。

- **文件**: `be/src/storage/tablet_reader.cpp`, `be/src/storage/rowset/rowset.cpp`
- **核心逻辑**:
    1.  **创建迭代器**: `TabletReader` 在 `open()` 时，会遍历持有的所有 `Rowset`，并为每个 `Rowset` 调用 `get_segment_iterators()`。
    2.  **Segment 迭代器**: `Rowset::get_segment_iterators()` 会为它包含的每个 `Segment` 文件创建一个 `SegmentIterator`。这个 `SegmentIterator` 负责从单个 Segment 文件中读取数据，并应用下推的谓词（如 `where` 条件）。
    3.  **迭代器合并**: `TabletReader` 拿到所有 `SegmentIterator` 后，会根据表的 `KeysType` 进行合并：
        - **`UnionIterator`**: 对于明细模型（`DUP_KEYS`）和主键模型（`PRIMARY_KEYS`），由于数据无需排序聚合，直接使用 `UnionIterator` 将所有 `SegmentIterator` 的数据流简单地拼接起来即可。
        - **`MergeIterator`**: 对于聚合模型（`AGG_KEYS`）和更新模型（`UNIQUE_KEYS`），数据需要按 Key 排序后才能进行聚合或去重。因此，使用 `MergeIterator`（内部是一个最小堆）对所有 `SegmentIterator` 的数据流进行归并排序。
        - **`AggregateIterator`**: 对于聚合模型，`MergeIterator` 的上层还会再包装一个 `AggregateIterator`，它负责在数据流经过时，对 Key 相同的数据行进行动态的聚合计算。
    4.  **数据流出**: 最终，`TabletReader` 向上层 `TabletScanner` 提供一个封装好的顶层迭代器（`_collect_iter`）。`TabletScanner` 只需不断调用 `get_next()`，就能从这个迭代器链的顶端拉取处理好的数据块，而无需关心底层有多少个 Rowset 和 Segment。

这个分层的迭代器设计，优雅地将对多个物理文件的复杂读取过程，抽象成了一个单一、标准的 `ChunkIterator` 接口，是 StarRocks 高效查询实现的关键。

### 第五章：后台任务与系统维护

为了保证系统的长期稳定、高性能运行和数据可靠性，StarRocks BE 后台运行着一系列关键的维护任务。本章将重点剖析其中最重要的三个机制：Compaction（数据合并）、WAL/Binlog（持久化与复制）以及 Clone（数据恢复）。

#### 5.1 Compaction (数据合并)

随着数据不断导入，系统中会产生大量零散的、小版本的 Rowset 文件。这会导致查询时需要合并的 Rowset 过多，降低读取性能；同时，对于主键模型，历史的更新和删除操作也需要被物理地清理。Compaction 就是为了解决这些问题而生的。

##### 5.1.1 Compaction 的触发与调度

- **文件**: `be/src/storage/compaction_manager.h`, `be/src/storage/default_compaction_policy.h`
- **生命周期**:
    1.  **策略 (Policy)**: `CompactionManager` 的后台线程会周期性地遍历所有 Tablet，并调用其 `need_compaction()` 方法。该方法内部会使用一个 `CompactionPolicy` 对象（如 `DefaultCumulativeBaseCompactionPolicy`）来计算一个“合并分数”。该策略会根据 Tablet 当前的 Rowset 数量、大小、版本连续性等因素，判断是否需要合并，以及是进行 `CUMULATIVE` 合并还是 `BASE` 合并。
    2.  **调度 (Manager)**: `CompactionManager` 维护一个 `_compaction_candidates` 集合，存储所有需要合并的 Tablet 及其分数。它会从中挑选出分数最高的 Tablet 作为下一个执行目标。
    3.  **任务 (Task)**: 选定 Tablet 后，管理器调用 `tablet->create_compaction_task()` 创建一个 `CompactionTask` 实例，并将其提交到专用的 `_compaction_pool` 线程池中执行。管理器通过并发度控制，确保系统不会因 Compaction 任务过多而影响前台查询和写入。

##### 5.1.2 Compaction 的执行

- **文件**: `be/src/storage/compaction_task.h`, `be/src/storage/compaction_task.cpp`
- **核心逻辑**: Compaction 任务的执行巧妙地**复用**了标准的读写路径。
    1.  **读取输入**: `CompactionTask::run_impl()` 会创建一个 `TabletReader`，将待合并的多个输入 Rowset 作为其读取目标。
    2.  **合并写入**: 它创建一个 `DeltaWriter`，用于写入合并后的新 Rowset。然后，它不断地从 `TabletReader` 中拉取数据块（此时 `TabletReader` 内部的 `MergeIterator` 会对数据进行排序和聚合），并将这些数据块写入 `DeltaWriter`。
    3.  **原子替换**: 当所有数据都读写完毕后，`DeltaWriter` 会 `build()` 出一个新的、合并后的 `Rowset`。`CompactionTask` 随即调用 `_commit_compaction()`，此方法会获取 `Tablet` 的元数据写锁，并调用 `_tablet->modify_rowsets_without_lock()`，在版本图中原子性地用这个新的 Rowset 替换掉所有旧的输入 Rowset。
    4.  **垃圾回收**: 被替换掉的旧 Rowset 的物理文件不会马上删除，而是被放入一个垃圾回收队列，等待没有查询再引用它们时，由后台线程安全地清理。

#### 5.2 WAL 与 Binlog：持久性与复制的保障

在数据库语境中，WAL (Write-Ahead Log) 和 Binlog (Binary Log) 是两个相似但目标不同的概念。

- **WAL**: 主要用于**崩溃恢复**。它在数据变更应用到主数据结构（如 `MemTable`）**之前**，将操作写入日志，确保操作的持久性。
- **Binlog**: 主要用于**主从复制**和**时间点恢复**。它记录的是已成功提交的事务，形成一个可供下游消费的事件流。

在 StarRocks 中：
- **没有传统意义的 WAL**: 经过深入分析，StarRocks BE 中并没有一个名为 "WAL" 的独立模块。
- **Binlog 用于复制**: `be/src/storage/binlog_manager.h` 中定义的 `BinlogManager` 是一个**写后**机制。它在 Rowset 成功写入、即将被发布时，从该 Rowset 中提取数据生成 binlog，用于 CDC 和复制。它不提供写入前的持久性保证。

那么，主键模型的持久性是如何保证的呢？答案在 `TabletUpdates` 中。

- **文件**: `be/src/storage/tablet_updates.h`
- **主键表的“日志即状态”机制**:
    1.  **`EditVersionInfo` 日志**: 对于主键表的写入，`TabletUpdates::rowset_commit` 方法会将本次写入（包含新生成的 Rowset）封装成一个 `EditVersionInfo` 对象。
    2.  **持久化**: 这个 `EditVersionInfo` 对象会被追加到 `_edit_version_infos` 列表中，并且整个 `TabletUpdates` 的元数据（包含了这个日志）会被**原子性地持久化**到 RocksDB 中。
    3.  **异步 Apply**: 在 `EditVersionInfo` 持久化并向客户端确认写入成功**之后**，一个后台的 Apply 线程才会异步地消费这个日志，将变更应用到内存中的主键索引上。

这个 `EditVersionInfo` 的持久化日志，事实上扮演了 WAL 的角色，它保证了即使在 Apply 线程执行前 BE 发生崩溃，重启后也能通过重放（`_redo_edit_version_log`）这些日志来恢复 Tablet 的正确状态，从而确保了主键表写入的持久性和原子性。

#### 5.3 数据恢复：Clone 任务

当一个 Tablet 副本因为磁盘损坏、节点下线等原因数据不完整时，StarRocks 会通过 Clone 机制从一个健康的副本上恢复数据。

- **文件**: `be/src/storage/task/engine_clone_task.h`
- **核心流程**:
    1.  **FE 发起**: FE 检测到需要修复的 Tablet，向目标 BE（需要数据的节点）发送一个 `TCloneReq` 任务。
    2.  **创建任务**: 目标 BE 收到请求后，创建一个 `EngineCloneTask` 来执行克隆。
    3.  **请求快照 (`_make_snapshot`)**: `EngineCloneTask` 向一个健康的源 BE 节点发起 RPC 请求，要求为指定的 Tablet 创建一个快照。源 BE 收到请求后，会为该 Tablet 特定版本的所有 Segment 文件创建一组**硬链接**到一个临时目录。这个过程非常快，且不影响源 Tablet 的正常读写。
    4.  **下载文件 (`_download_files`)**: 源 BE 创建快照成功后，会返回快照的路径。目标 BE 接着通过 HTTP 协议，从源 BE 下载快照目录下的所有数据文件。
    5.  **完成克隆 (`_finish_clone`)**: 文件全部下载到本地后，目标 BE 会加载这些 Rowset，更新本地 Tablet 的元数据，使其包含这些新同步过来的 Rowset，并最终向 FE 报告任务完成。

通过“快照 + 下载”的机制，Clone 任务能够高效、一致地完成副本的修复和数据迁移。

### 第六章：总结与展望

#### 6.1 StarRocks BE 设计思想总结

通过对 StarRocks BE 模块从写入、存储、查询到后台任务的全链路源码剖析，我们可以总结出其几个核心的设计思想：

1.  **存算一体的 MPP 架构**: BE 节点同时负责存储和计算，通过 MPP 架构将计算任务打散到数据所在的节点，最大化地减少了数据移动，是其高性能查询的基础。
2.  **日志即数据 (Log-is-Data) 的 LSM-Tree 变种**: StarRocks 的存储模型可以看作是 LSM-Tree 的一种变种。每次写入都生成新的、不可变的 Rowset 文件（类似 SSTable），后台通过 Compaction 不断地将小的 Rowset 合并成大的、有序的 Rowset。这种设计将随机写转换为了顺序写，极大地提升了写入性能。
3.  **MVCC 实现快照隔离**: 基于版本图（`TimestampedVersionTracker`）的 MVCC 机制是 StarRocks 实现高性能并发读写的关键。查询通过捕获特定版本的 Rowset 快照，实现了与写入和合并的完全解耦，做到了读写互不阻塞。
4.  **异步化与 Pipeline**: 整个数据写入和读取路径被设计为高度异步和流水线化的。无论是写入时的 RPC 调用（`NodeChannel`），还是最终落盘（`AsyncDeltaWriter`），都采用了非阻塞的异步任务+回调模式，最大化地利用了系统资源，提升了整体吞吐。
5.  **模块化与分层设计**: 从 `OlapTableSink` 到 `DeltaWriter` 的写入层次，以及从 `TabletReader` 到 `SegmentIterator` 的读取层次，都体现了清晰的模块化和分层设计思想。每一层都只关注自己的核心职责，使得整个系统复杂但有序，易于理解和扩展。
6.  **主键模型的独特实现**: 针对主键表的更新场景，StarRocks 没有采用传统的 Read-Modify-Write 模式，而是创造性地设计了“`EditVersion` 日志 + 异步 Apply”的机制，将对主键索引的随机写操作异步化，并通过 `DelVector` 实现了高效的延迟删除，在保证功能的同时兼顾了性能。

#### 6.2 关键实现与文档对比验证

通过本次源码分析，可以验证 StarRocks 的核心实现与官方文档和社区分享的高层架构设计是高度一致的。例如，其 MVCC、LSM-Tree 模型、Compaction 机制等，都在源码中找到了对应的、逻辑严谨的实现。

然而，源码也揭示了一些文档中未详述或已演进的细节。例如，关于主键模型写入的持久性保证，社区中常讨论的“WAL”在源码中并没有直接的对应模块，其功能实际上是由 `TabletUpdates` 中的 `EditVersionInfo` 日志机制所承担。这种发现对于精确理解系统行为至关重要。

#### 6.3 未来优化方向探讨

基于对源码的理解，可以预见 StarRocks BE 未来的几个潜在优化方向：

1.  **Compaction 策略优化**: 当前的 Compaction 策略虽然有效，但仍有优化空间。例如，可以引入更智能的策略，感知业务的查询和写入模式，动态调整 Compaction 的目标和优先级，以在成本和性能之间取得更好的平衡。
2.  **主键索引性能**: `PrimaryIndex` 目前基于 RocksDB，虽然可靠，但在极高的更新吞吐下仍可能成为瓶颈。探索内存化、或针对其访问模式特化的新型索引结构，可能会带来性能上的突破。
3.  **存储格式演进**: 随着列式存储技术的发展，可以引入更先进的编码、压缩算法，或探索更紧凑的 Segment 文件格式，以进一步降低存储成本和提升扫描性能。
4.  **CBO 与存储层联动**: 让 CBO（成本估算优化器）能够获取更精细的存储层统计信息（如列的详细分布、稀疏性等），可以帮助生成更优的执行计划，尤其是在谓词下推和索引选择方面。

总之，StarRocks BE 是一个设计精良、实现复杂的现代化存储和计算引擎。通过对其源码的深入研究，我们不仅能掌握其工作原理，更能体会到其在分布式系统和数据库设计上的诸多巧思，为我们自己的技术实践带来深刻的启发。
