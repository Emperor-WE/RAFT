# KVstorageBaseRaft-cpp

本项目为：[【代码随想录知识星球】](https://www.programmercarl.com/other/project_fenbushi.html)项目分享-基于Raft的k-v存储数据库。 

## 改进
* 去除 provider对muduo库的依赖，改为非阻塞epoll监听 + 线程池的方式实现rpc消息接收和转发。
* 原项目KvServer层Append和Put方法均走的跳表的 insert_set_element 方法，修改Append方法为追加，若原K-V DB存在则不对原有值进行修改
* raft代码中，每次的心跳发送、日志项同步、节点选举和投票都是新建线程去处理，处理完任务之后便被销毁，使得频频繁的创建和销毁线程。对此改为基于boost库的线程池，直接添加任务调度即可，用于避免线程的频繁创建和销毁
* 原rpc测试用例中IP地址和端口不一致，统一测试用例IP地址和端口
* 移除序列化测试等代码，精简项目目录结构

## Doc
- [x] 增加代码注释
- [x] 增加 rpc、raft、kvServer代码图解。我在原本画图解的时候，将各个模块都画在了一张图上，这里我记不做拆分了，还是将其放在一起（画图使用的是 processon）。

## 使用方法
1.库准备
- boost
- protoc [3.21.11]

2.编译
```
mkdir bl
cd bl
cmake ..
make
```

3.使用rpc
- provider
- consumer
注意先运行provider，再运行consumer。

4.使用raft集群
```
raftCoreRun -n 3 -f test.conf
```

5.使用kv
* 在启动raft集群之后启动`callerMain`即可。

## todoList
- [ ] 协程库fiber修改为无栈协程
- [ ] 协程库fiber修改为对称协程调度
