#include "kvServer.h"

#include <rpcprovider.h>

#include "mprpcconfig.h"

void KvServer::DprintfKVDB()
{
    if (!Debug)
    {
        return;
    }
    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER
    {
        // for (const auto &item: m_kvDB) {
        //     DPrintf("[DBInfo ----]Key : %s, Value : %s", &item.first, &item.second);
        // }
        m_skipList.display_list();
    };
}

/**
 * @brief 追加键值
 * @note 已存在，则追加，不会替换原有值
 */
void KvServer::ExecuteAppendOpOnKVDB(Op op)
{
    // if op.IfDuplicate {   //get请求是可重复执行的，因此可以不用判复
    //	return
    // }
    m_mtx.lock();

    m_skipList.insert_set_element(op.Key, op.Value);

    // if (m_kvDB.find(op.Key) != m_kvDB.end()) {
    //     m_kvDB[op.Key] = m_kvDB[op.Key] + op.Value;
    // } else {
    //     m_kvDB.insert(std::make_pair(op.Key, op.Value));
    // }
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    //    DPrintf("[KVServerExeAPPEND-----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
    //    op.Key, op.Value)
    DprintfKVDB();
}

/* 从跳表中查找 op.key 是否存在 */
void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist)
{
    m_mtx.lock();
    *value = "";
    *exist = false;
    if (m_skipList.search_element(op.Key, *value))
    {
        *exist = true;
        // *value = m_skipList.se //value已经完成赋值了
    }
    // if (m_kvDB.find(op.Key) != m_kvDB.end()) {
    //     *exist = true;
    //     *value = m_kvDB[op.Key];
    // }
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    if (*exist)
    {
        //                DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, value :%v", op.ClientId,
        //                op.RequestId, op.Key, value)
    }
    else
    {
        //        DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, But No KEY!!!!", op.ClientId,
        //        op.RequestId, op.Key)
    }
    DprintfKVDB();
}

/**
 * @brief 将op键值插入到跳表数据库并更新m_lastRequestId
 * @note 插入新键值对 或 更新已存在键值对
 */
void KvServer::ExecutePutOpOnKVDB(Op op)
{
    m_mtx.lock();
    m_skipList.insert_set_element(op.Key, op.Value);
    // m_kvDB[op.Key] = op.Value;
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    //    DPrintf("[KVServerExePUT----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
    //    op.Key, op.Value)
    DprintfKVDB();
}

// 处理来自clerk的Get RPC
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply)
{
    Op op;
    op.Operation = "Get";
    op.Key = args->key();
    op.Value = "";
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;
    // raftIndex：raft预计的logIndex, 虽然是预计，但是正确情况下是准确的，op的具体内容对raft来说 是隔离的
    m_raftNode->Start(op, &raftIndex, &_, &isLeader);

    if (!isLeader)
    {
        reply->set_err(ErrWrongLeader);
        return;
    }

    // create waitForCh
    m_mtx.lock();

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
    {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock(); // 直接解锁，等待任务执行完成，不能一直拿锁等待

    // timeout
    Op raftCommitOp;

    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp))
    {
        //        DPrintf("[GET TIMEOUT!!!]From Client %d (Request %d) To Server %d, key %v, raftIndex %d", args.ClientId,
        //        args.RequestId, kv.me, op.Key, raftIndex)
        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader);

        // 是否是同一个 client 的重复请求
        if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader)
        {
            // 如果超时，代表raft集群不保证已经commitIndex该日志，但是如果是已经提交过的get请求，是可以再执行的。
            //  不会违反线性一致性
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist)
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else
        {
            reply->set_err(ErrWrongLeader); // 返回这个，其实就是让clerk换一个节点重试
        }
    }
    else
    {
        // raft已经提交了该command（op），可以正式开始执行了
        //         DPrintf("[WaitChanGetRaftApplyMessage<--]Server %d , get Command <-- Index:%d , ClientId %d, RequestId
        //         %d, Opreation %v, Key :%v, Value :%v", kv.me, raftIndex, op.ClientId, op.RequestId, op.Operation, op.Key,
        //         op.Value)
        // todo 这里还要再次检验的原因：感觉不用检验，因为leader只要正确的提交了，那么这些肯定是符合的
        // TODO 为什么取出 队头 元素就是匹配的呢
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId)
        {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist)
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else
        {
            reply->set_err(ErrWrongLeader);
            //            DPrintf("[GET ] 不满足：raftCommitOp.ClientId{%v} == op.ClientId{%v} && raftCommitOp.RequestId{%v}
            //            == op.RequestId{%v}", raftCommitOp.ClientId, op.ClientId, raftCommitOp.RequestId, op.RequestId)
        }
    }
    m_mtx.lock(); // todo 這個可以先弄一個defer，因爲刪除優先級並不高，先把rpc發回去更加重要
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

/**
 * @brief 从Raft节点中获取消息（不要误以为是执行【GET】命令）
 *      解析命令
 * @details
 *      1.解析 command
 *      2.若 command 已经写入快照啊，无需处理
 *      3.判断是否为重复指令【依据clientID 和 requestID】，只有不是重复的指令才会执行
 *      4.快照
 *      5.send command
 */
void KvServer::GetCommandFromRaft(ApplyMsg message)
{
    Op op;
    op.parseFromString(message.Command);

    DPrintf(
        "[KvServer::GetCommandFromRaft-kvserver{%d}]  Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
        "Opreation {%s}, Key :{%s}, Value :{%s}",
        m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    
    // 该 command 并未写入快照
    // TODO 这里的注释和代码不符合吧
    // command 已经写入快照，无需在做处理
    if (message.CommandIndex <= m_lastSnapShotRaftLogIndex)
    {
        return;
    }

    // 状态机(KVServer解决复制问题)   复制命令不会执行
    if (!ifRequestDuplicate(op.ClientId, op.RequestId))
    {
        // execute command
        if (op.Operation == "Put")
        {
            ExecutePutOpOnKVDB(op);
        }
        if (op.Operation == "Append")
        {
            ExecuteAppendOpOnKVDB(op);
        }
        //  kv.lastRequestId[op.ClientId] = op.RequestId  在Executexxx函数里面更新的
    }
    // 到这里kvDB已经制作了快照
    if (m_maxRaftState != -1)
    {
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
        // 如果raft的log太大（大于指定的比例）就把制作快照
    }

    // Send message to the chan of op.ClientId
    SendMessageToWaitChan(op, message.CommandIndex);
}

/* 是否是重复请求（统一客户端的同一请求）*/
bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    if (m_lastRequestId.find(ClientId) == m_lastRequestId.end())
    {
        return false;
        // todo :不存在这个client就创建
    }
    return RequestId <= m_lastRequestId[ClientId];
}

/** 
 * @brief PUT 请求【幂等】
 * @details
 *      1.根据请求args初始化请求操作 op
 *      2.leader节点处理请求【leader强一致性】[执行成功后会放入安全映射队列LockQueue]
 *      3.通过LockQueue时间等待出队判断执行是否超时
 *          a) 超时
 *              i) 重复请求 -- 无需再次执行 ok
 *              ii）不是重复请求 -- 执行失败
 *          b) 未超时，则验证执行的是否是该请求【可能出现leader节点强制覆盖】，返回执行结果
 * @note
 *      get和put append执行的具体细节是不一樣的. 
 *      PutAppend在收到raft消息之后執行，具体函数里面只判断幂等性（是否重複）. 
 *      get函數收到raft消息之後在，因爲get无论是否重复都可以再執行.
 */
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply)
{
    Op op;
    op.Operation = args->op();
    op.Key = args->key();
    op.Value = args->value();
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();
    int raftIndex = -1;
    int _ = -1;
    bool isleader = false;

    m_raftNode->Start(op, &raftIndex, &_, &isleader);

    if (!isleader)
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]  From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
            "not leader",
            m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

        reply->set_err(ErrWrongLeader);
        return;
    }
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]  From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
        "leader ",
        m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
    {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock(); // 直接解锁，等待任务执行完成，不能一直拿锁等待

    // timeout
    Op raftCommitOp;

    // 通过超时pop来限定命令执行时间，如果超过时间还没拿到消息说明命令执行超时了。
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp))
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]  TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

        if (ifRequestDuplicate(op.ClientId, op.RequestId))
        {
            reply->set_err(OK); // 超时了,但因为是重复的请求，返回ok，实际上就算没有超时，在真正执行的时候也要判断是否重复
        }
        else
        {
            reply->set_err(ErrWrongLeader); /// 这里返回这个的目的让clerk重新尝试
        }
    }
    else
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]  WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        // 没超时，命令可能真正的在raft集群执行成功了。
        // 可能发生leader的变更导致日志被覆盖，因此必须检查
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId)
        {
            reply->set_err(OK);
        }
        else
        {
            reply->set_err(ErrWrongLeader);
        }
    }

    m_mtx.lock();

    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::ReadRaftApplyCommandLoop()
{
    while (true)
    {
        // 如果只操作applyChan不用拿锁，因为applyChan自己带锁
        auto message = applyChan->Pop(); // 阻塞弹出
        DPrintf(
            "[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}]  收到了下raft的消息",
            m_me);
        // listen to every command applied by its raft ,delivery to relative RPC Handler

        if (message.CommandValid)
        {
            GetCommandFromRaft(message);
        }
        if (message.SnapshotValid)
        {
            GetSnapShotFromRaft(message);
        }
    }
}

// raft会与persist层交互，kvserver层也会，因为kvserver层开始的时候需要恢复kvdb的状态
//  关于快照raft层与persist的交互：保存kvserver传来的snapshot；生成leaderInstallSnapshot RPC的时候也需要读取snapshot；
//  因此snapshot的具体格式是由kvserver层来定的，raft只负责传递这个东西
//  snapShot里面包含kvserver需要维护的persist_lastRequestId 以及kvDB真正保存的数据persist_kvdb
void KvServer::ReadSnapShotToInstall(std::string snapshot)
{
    if (snapshot.empty())
    {
        // bootstrap without any state?
        return;
    }
    parseFromString(snapshot);

    //    r := bytes.NewBuffer(snapshot)
    //    d := labgob.NewDecoder(r)
    //
    //    var persist_kvdb map[string]string  //理应快照
    //    var persist_lastRequestId map[int64]int //快照这个为了维护线性一致性
    //
    //    if d.Decode(&persist_kvdb) != nil || d.Decode(&persist_lastRequestId) != nil {
    //                DPrintf("KVSERVER %d read persister got a problem!!!!!!!!!!",kv.me)
    //        } else {
    //        kv.kvDB = persist_kvdb
    //        kv.lastRequestId = persist_lastRequestId
    //    }
}

/**
 * @brief 向等待通道发送消息【只是放入队列，等待发送】
 */
bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}]  Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
    {
        return false;
    }
    waitApplyCh[raftIndex]->Push(op);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}]  Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    return true;
}

/**
 * @brief 检查是否需要制作快照，需要的话就向raft之下制作快照
 */
void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion)
{
    // TODO 为什么这里要 /10.0
    if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0)
    {
        // Send SnapShot Command
        auto snapshot = MakeSnapShot();
        m_raftNode->Snapshot(raftIndex, snapshot);
    }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg message)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot))
    {
        ReadSnapShotToInstall(message.Snapshot);
        m_lastSnapShotRaftLogIndex = message.SnapshotIndex;
    }
}

/* 安全序列化this KvServer对象 */
std::string KvServer::MakeSnapShot()
{
    std::lock_guard<std::mutex> lg(m_mtx);
    std::string snapshotData = getSnapshotData();
    return snapshotData;
}

void KvServer::PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                         ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done)
{
    KvServer::PutAppend(request, response);
    done->Run();
}

void KvServer::Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
                   ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done)
{
    KvServer::Get(request, response);
    done->Run();
}

/**
 * @brief KvServer构造 -- 跳表最大层数为 6
 * @param[in] me 节点标识
 * @param[in] maxraftstate 日志大小阈值，超过则触发快照
 * @param[in] 节点配置文件
 * @param[in] port 节点端口
 */
KvServer::KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port) : m_skipList(6)
{
    std::shared_ptr<Persister> persister = std::make_shared<Persister>(me);

    m_me = me;
    m_maxRaftState = maxraftstate;

    applyChan = std::make_shared<LockQueue<ApplyMsg>>();

    m_raftNode = std::make_shared<Raft>();

    /* clerk层面 kvserver开启rpc接受功能，同时raft与raft节点之间也要开启rpc功能，因此有两个注册 */ 
    std::thread t([this, port]() -> void
        {
            // provider是一个rpc网络服务对象。把UserService对象发布到rpc节点上
            RpcProvider provider;
            provider.NotifyService(this);
            // TODO 这里获取了原始指针，后面检查一下有没有泄露的问题 或者 shareptr释放的问题
            provider.NotifyService(this->m_raftNode.get());  // todo：
            // 启动一个rpc服务发布节点   Run以后，进程进入阻塞状态，等待远程的rpc调用请求
            provider.Run(m_me, port); 
        });
    
    t.detach();

    // 开启rpc远程调用能力，需要注意必须要保证所有节点都开启rpc接受功能之后才能开启rpc远程调用能力
    // 这里使用睡眠来保证
    //std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
    DPrintf("[KvServer]  raftServer node: start to sleep to wait all ohter raftnode start!");
    sleep(6);
    //std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;
    DPrintf("[KvServer]  raftServer node: wake up! start to connect other raftnode!");
    
    // 获取所有raft节点ip、port ，并进行连接  ,要排除自己
    MprpcConfig config;
    config.LoadConfigFile(nodeInforFileName.c_str());
    std::vector<std::pair<std::string, short>> ipPortVt;
    for (int i = 0; i < INT_MAX - 1; ++i)
    {
        std::string node = "node" + std::to_string(i);

        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty())
        {
            break;
        }
        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str())); // 沒有atos方法，可以考慮自己实现
    }
    std::vector<std::shared_ptr<RaftRpcUtil>> servers;
    // 进行连接
    for (int i = 0; i < ipPortVt.size(); ++i)
    {
        if (i == m_me)
        {
            servers.push_back(nullptr);
            continue;
        }
        std::string otherNodeIp = ipPortVt[i].first;
        short otherNodePort = ipPortVt[i].second;
        auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
        servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));

        //std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
        DPrintf("[KvServer]  node(%d) 连接node(%d) success!", m_me, i);
    }
    sleep(ipPortVt.size() - me); // 等待所有节点相互连接成功，再启动raft
    m_raftNode->init(servers, m_me, persister, applyChan);
    // kv的server直接与raft通信，但kv不直接与raft通信，所以需要把ApplyMsg的chan传递下去用于通信，两者的persist也是共用的

    //////////////////////////////////

    // You may need initialization code here.
    // m_kvDB; //kvdb初始化
    m_skipList;
    waitApplyCh;
    m_lastRequestId;
    m_lastSnapShotRaftLogIndex = 0; // todo:感覺這個函數沒什麼用，不如直接調用raft節點中的snapshot值？？？
    auto snapshot = persister->ReadSnapshot();
    if (!snapshot.empty())
    {
        ReadSnapShotToInstall(snapshot);
    }
    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this); // 马上向其他节点宣告自己就是leader
    t2.join();                                                 // 由於ReadRaftApplyCommandLoop一直不會結束，达到一直卡在这的目的
}
