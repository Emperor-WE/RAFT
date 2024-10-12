# KVstorageBaseRaft-cpp

本项目为：[【代码随想录知识星球】](https://www.programmercarl.com/other/project_fenbushi.html)项目分享-基于Raft的k-v存储数据库。 

## 使用方法

### 1.库准备
- boost
- protoc [3.21.11]

**安装说明**

### 2.编译启动
#### 使用rpc
```
mkdir bl
cd bl
cmake ..
make
```
之后在目录bin就有对应的可执行文件生成：
- provider
- consumer
注意先运行provider，再运行consumer。

#### 使用raft集群
```
mkdir bl
cd bl
cmake..
make
```

```
// make sure you in bin directory ,and this has a test.conf file
raftCoreRun -n 3 -f test.conf
```

#### 使用kv
在启动raft集群之后启动`callerMain`即可。


## Docs


## todoList

- [x] 完成raft节点的集群功能
- [ ] 去除冗余的库：muduo、boost 
- [ ] 代码精简优化
- [x] code format
- [ ] 代码解读 maybe
