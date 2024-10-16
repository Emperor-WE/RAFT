#include "rpcprovider.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <fstream>
#include <string>
#include "rpcheader.pb.h"
#include "util.h"

int set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        return -1; // 获取文件状态标志失败
    }

    flags |= O_NONBLOCK; // 设置非阻塞标志
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        return -1; // 设置文件状态标志失败
    }

    return 0; // 成功
}

RpcProvider::RpcProvider() : m_pool(18)
{
    m_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int optval = 1;
    if (setsockopt(m_lfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
        perror("setsockopt");
        close(m_lfd);
        return;
    }
    m_epfd = epoll_create(20);
}

/**
 * @brief 这里是框架提供给外部使用的，可以发布rpc方法的函数接口
 * @details 只是简单的把服务描述符和方法描述符全部保存在本地而已
 * @todo todo 待修改 要把本机开启的ip和端口写在文件里面
 */
void RpcProvider::NotifyService(google::protobuf::Service *service)
{
    ServiceInfo service_info;

    // 获取了服务对象的描述信息
    const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();
    // 获取服务的名字
    std::string service_name = pserviceDesc->name();
    // 获取服务对象service的方法的数量
    int methodCnt = pserviceDesc->method_count();

    for (int i = 0; i < methodCnt; ++i)
    {
        // 获取了服务对象指定下标的服务方法的描述（抽象描述） UserService   Login
        const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        //DPrintf("[RpcProvider]  service_name=%s, method_name=%s", service_name.c_str(), method_name.c_str());

        service_info.m_methodMap.insert({method_name, pmethodDesc});
    }
    service_info.m_service = service;
    m_serviceMap.insert({service_name, service_info});
}

// 启动rpc服务节点，开始提供rpc远程网络调用服务
void RpcProvider::Run(int nodeIndex, short port)
{
    // 获取可用ip
    char *ipC;
    char hname[128];
    struct hostent *hent;
    gethostname(hname, sizeof(hname));  // 获取本地主机名
    // 传入本地主机的名称hname，该函数会查找并返回一个包含主机信息的hostent结构体指针
    // 这个函数会从DNS或/etc/hosts文件中查找主机名对应的IP地址信息
    hent = gethostbyname(hname);
    // hent->h_addr_list 该数组包含了主机的所有IP地址
    for (int i = 0; hent->h_addr_list[i]; i++)
    {
        // 将h_addr_list数组中的二进制IP地址转换为点分十进制字符串格式
        ipC = inet_ntoa(*(struct in_addr *)(hent->h_addr_list[i])); // IP地址
    }
    std::string ip = std::string(ipC);
    //    // 获取端口
    //    if(getReleasePort(port)) //在port的基础上获取一个可用的port，不知道为何没有效果
    //    {
    //        std::cout << "可用的端口号为：" << port << std::endl;
    //    }
    //    else
    //    {
    //        std::cout << "获取可用端口号失败！" << std::endl;
    //    }

    // 写入文件 "test.conf"
    std::string node = "node" + std::to_string(nodeIndex);
    std::ofstream outfile;
    outfile.open("test.conf", std::ios::app); // 打开文件并追加写入
    if (!outfile.is_open())
    {
        //std::cout << "打开文件失败！" << std::endl;
        DPrintf("[RpcProvider]  open test.conf file error!");
        exit(EXIT_FAILURE);
    }
    outfile << node + "ip=" + ip << std::endl;
    outfile << node + "port=" + std::to_string(port) << std::endl;
    outfile.close();

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    //server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(m_lfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(m_lfd);
    }

    if (listen(m_lfd, 20) == -1) {
        perror("listen");
        close(m_lfd);
    }

    set_nonblocking(m_lfd);

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_lfd;

    if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_lfd, &event) == -1) {
        perror("epoll_ctl");
        close(m_lfd);
        close(m_epfd);
    }

    epoll_event events[20];
    while(true)
    {
        memset(events, 0, sizeof(epoll_event) * 20);
        int n = epoll_wait(m_epfd, events, 20, -1);
        if (n == -1) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == m_lfd)
            {
                boost::asio::post(m_pool, std::bind(&RpcProvider::OnConnection, this));
                continue;
            }
            else
            {
                boost::asio::post(m_pool, std::bind(&RpcProvider::OnMessage, this, fd));
            }
        }
    }

}

// 新的socket连接回调
void RpcProvider::OnConnection()
{
    while(true)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(m_lfd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                perror("accept");
                return;
            }
        }

        set_nonblocking(conn_fd);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = conn_fd;
        if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
            perror("epoll_ctl: conn_fd");
            close(conn_fd);
        } else {
            std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>New connection from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << std::endl;
        }
    }
}

/**
 * @brief 已建立连接用户的读写事件回调 如果远程有一个rpc服务的调用请求，那么OnMessage方法就会响应
 * @details 这里来的肯定是一个远程调用请求
 *          因此本函数需要：解析请求，根据服务名，方法名，参数，来调用service的来callmethod来调用本地的业务
 * @note 在框架内部，RpcProvider和RpcConsumer协商好之间通信用的protobuf数据类型
 *          buffer 构成：
                    【header_size(4个字节) + header_str + args_str】
            其中 header_str
                    【service_name method_name args_size】
 */
void RpcProvider::OnMessage(int sockfd)
{
    // char buffer[256];
    // ssize_t bytes_read = read(sockfd, buffer, 256);
    // if (bytes_read <= 0) {
    //     if (bytes_read == 0) {
    //         std::cout << "==========================Connection closed by client===============================" << std::endl;
    //     } else {
    //         perror("read");
    //     }
    //     close(sockfd);
    //     epoll_ctl(m_epfd, EPOLL_CTL_DEL, sockfd, NULL);
    // }

    char buffer[4096];  // 增加缓冲区大小
    ssize_t total_bytes_read = 0;

    std::string recv_buf;

    while (true) {
        ssize_t bytes_read = read(sockfd, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            if (bytes_read == 0)
            {
                std::cout << "==========================Connection closed by client===============================" << std::endl;
                //close(sockfd);
                //epoll_ctl(m_epfd, EPOLL_CTL_DEL, sockfd, NULL);
                //exit(-1);
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) 
                {
                    break;
                }
                else
                {
                    std::cout << "==========================read error===============================" << std::endl;
                    //close(sockfd);
                    //epoll_ctl(m_epfd, EPOLL_CTL_DEL, sockfd, NULL);
                    //exit(-1);
                }
            }
        }

        total_bytes_read += bytes_read;
        recv_buf.append(buffer, bytes_read);
    }

    //printf("[RpcProvider]  OnMessage read message-%d: %s\n", bytes_read, buffer);

    // 网络上接收的远程rpc调用请求的字符流    Login args
    // 从缓冲区中检索所有剩余的数据并将其转换为 std::string 类型返回
    //buffer[bytes_read] = '\0'; // 确保字符串以 null 结尾
    //std::string recv_buf(buffer, bytes_read);

    // 使用protobuf的CodedInputStream来解析数据流
    google::protobuf::io::ArrayInputStream array_input(recv_buf.data(), recv_buf.size());
    google::protobuf::io::CodedInputStream coded_input(&array_input);
    uint32_t header_size{};

    coded_input.ReadVarint32(&header_size); // 解析header_size

    // 根据header_size读取数据头的原始字符流，反序列化数据，得到rpc请求的详细信息
    std::string rpc_header_str;
    RPC::RpcHeader rpcHeader;   // 【service_name method_name args_size】
    std::string service_name;
    std::string method_name;

    // 设置读取限制，不必担心数据读多
    google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
    coded_input.ReadString(&rpc_header_str, header_size);
    // 恢复之前的限制，以便安全地继续读取其他数据
    coded_input.PopLimit(msg_limit);
    uint32_t args_size{};
    if (rpcHeader.ParseFromString(rpc_header_str))
    {
        // 数据头反序列化成功
        service_name = rpcHeader.service_name();
        method_name = rpcHeader.method_name();
        args_size = rpcHeader.args_size();
    }
    else
    {
        // 数据头反序列化失败
        //std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
        DPrintf("[RpcProvider]  rpc_header_str=%s, parse error!", rpc_header_str);
        return;
    }

    // 获取rpc方法参数的字符流数据
    std::string args_str;
    // 直接读取args_size长度的字符串数据
    bool read_args_success = coded_input.ReadString(&args_str, args_size);

    if (!read_args_success)
    {
        // 处理错误：参数数据读取失败
        return;
    }

    //打印调试信息
    //    std::cout << "============================================" << std::endl;
    //    std::cout << "header_size: " << header_size << std::endl;
    //    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
        //std::cout << "service_name: " << service_name << std::endl;
        //std::cout << "method_name: " << method_name << std::endl;
    //    std::cout << "args_str: " << args_str << std::endl;
        //std::cout << "============================================" << std::endl;
        // if(service_name == "kvServerRpc" || method_name == "PutAppend")
        // {
        //     exit(1);
        // }

    // 获取service对象和method对象
    auto it = m_serviceMap.find(service_name);
    if (it == m_serviceMap.end())
    {
        std::cout << "服务：" << service_name << " is not exist!" << std::endl;
        std::cout << "当前已经有的服务列表为:";
        for (auto item : m_serviceMap)
        {
            std::cout << item.first << " ";
        }
        std::cout << std::endl;
        return;
    }
    // 查找 method 方法对象
    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end())
    {
        std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
        return;
    }

    google::protobuf::Service *service = it->second.m_service;      // 获取service对象  new UserService
    const google::protobuf::MethodDescriptor *method = mit->second; // 获取method对象  Login

    /* 生成rpc方法调用的请求request和响应response参数,由于是rpc的请求，因此请求需要通过request来序列化 */

    // 获取对应RPC方法(method)的请求消息原型
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str))
    {
        std::cout << "request parse error, content:" << args_str << std::endl;
        return;
    }
    // 创建响应消息的实例，用于接收RPC调用的结果(method)
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();

    // 给下面的method方法的调用，绑定一个Closure的回调函数 --- SendRpcResponse回调
    // closure是执行完本地方法之后会发生的回调，因此需要完成序列化和反向发送请求的操作
    google::protobuf::Closure *done =
        google::protobuf::NewCallback<RpcProvider, int, google::protobuf::Message *>(
            this, &RpcProvider::SendRpcResponse, sockfd, response);

    /** new UserService().Login(controller, request, response, done)  在框架上根据远端rpc请求，调用当前rpc节点上发布的方法
     * 
     * 为什么下面这个service->CallMethod 要这么写？或者说为什么这么写就可以直接调用远程业务方法了
     * 这个service在运行的时候会是注册的service
     * 用户注册的service类 继承 .protoc生成的serviceRpc类 继承 google::protobuf::Service
     * 用户注册的service类里面没有重写CallMethod方法，是 .protoc生成的serviceRpc类 里面重写了google::protobuf::Service中
     * 的纯虚函数CallMethod，而 .protoc生成的serviceRpc类 会根据传入参数自动调取 生成的xx方法（如Login方法），
     * 由于xx方法被 用户注册的service类 重写了，因此这个方法运行的时候会调用 用户注册的service类 的xx方法
     */
    service->CallMethod(method, nullptr, request, response, done);
}

// Closure的回调操作，用于序列化rpc的响应和网络发送,发送响应回去
void RpcProvider::SendRpcResponse(int sockfd, google::protobuf::Message *response)
{
    std::string response_str;
    if (response->SerializeToString(&response_str)) // response进行序列化
    {
        // 序列化成功后，通过网络把rpc方法执行的结果发送会rpc的调用方
        //DPrintf("response:%s", response_str);
        write(sockfd, response_str.c_str(), response_str.size());
        //DPrintf("[1111111111111111111111111RpcProvider]  Sent response of size %d, %s", n);
        // std::cout << "[8888888888888888888888888RpcProvider]  SendRpcResponse-size:" <<response_str.size() <<  response_str << std::endl;
    }
    else
    {
        std::cout << "serialize response_str error!" << std::endl;
    }
    //    conn->shutdown(); // 模拟http的短链接服务，由rpcprovider主动断开连接  //改为长连接，不主动断开
}

RpcProvider::~RpcProvider()
{
    //std::cout << "[func - RpcProvider::~RpcProvider()]: ip和port信息：" << m_muduo_server->ipPort() << std::endl;
    //DPrintf("[RpcProvider]  RpcProvider::~RpcProvider(), ip和port信息:%s!", m_muduo_server->ipPort().c_str());
    //m_eventLoop.quit();
    //    m_muduo_server.   怎么没有stop函数，奇奇怪怪，看csdn上面的教程也没有要停止，甚至上面那个都没有
}
