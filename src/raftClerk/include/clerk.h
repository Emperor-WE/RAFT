#ifndef SKIP_LIST_ON_RAFT_CLERK_H
#define SKIP_LIST_ON_RAFT_CLERK_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <raftServerRpcUtil.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>
#include "kvServerRPC.pb.h"
#include "mprpcconfig.h"

/**
 * @brief 客户端与集群交互的封装
 */
class Clerk
{
private:
    std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers; // 保存所有raft节点的fd TODO：全部初始化为-1，表示没有连接上
    std::string m_clientId;
    int m_requestId;
    int m_recentLeaderId;   // 只是有可能是领导

    // 用于生成返回随机的clientId
    std::string Uuid()
    {
        // TODO 生成优化
        return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
    }

    // MakeClerk  todo
    void PutAppend(std::string key, std::string value, std::string op);

public:
    // 对外暴露的三个功能和初始化
    void Init(std::string configFileName);
    std::string Get(std::string key);

    void Put(std::string key, std::string value);
    void Append(std::string key, std::string value);

public:
    Clerk();
};

#endif // SKIP_LIST_ON_RAFT_CLERK_H
