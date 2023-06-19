#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>

#define SERVER_PORT 80

#ifndef NULL
#define NULL ((void*)0)
#endif

static int debug = 0;

// socket的错误处理
void err_msg(const int *sockfd, char *name) {
    fprintf(stderr, "%s error! reason:%s\n", name, strerror(errno));
    close(*sockfd);
    exit(1);
}

// 获取请求信息
int get_line(int socket, char *buf, int size) {
    int count = 0;
    char end = '\0';
    int len;

    while ((count < size - 1) && end != '\n') {
        len = read(socket, &end, 1);
        if (len == 1) {
            if (end == '\r') {
                continue;
            } else if (end == '\n') {
                break;
            }
            // 处理一般字符
            buf[count] = end;
            count++;
        } else if (len == -1) {
            // 读取出错
            perror("read failed!");
            count = -1;
            break;
        } else {
            // 返回0，客户端关闭连接
            fprintf(stderr, "client close.\n");
            count = -1;
            break;
        }
    }
    if (count >= 0)
        buf[count] = '\0';
    return count;
}

// 400请求错误页面
void bad_request(int client_sock) {
    const char *reply = "\
<!DOCTYPE html>\n\
<html>\n\
<head>\n\
    <title>400 Bad Request</title>\n\
</head>\n\
<body>\n\
    <h1>400 Bad Request</h1>\n\
    <p>The server cannot process the request due to a client error.</p>\n\
</body>\n\
</html>";

    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout, "%s", reply);
    if (len <= 0) {
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }
}

// 404未找到页面
void file_not_found(int client_sock) {
    const char *reply = "\
<!DOCTYPE html>\n\
<html>\n\
<head>\n\
    <title>404 Not Found</title>\n\
</head>\n\
<body>\n\
    <h1>404 Not Found</h1>\n\
    <p>The page you requested could not be found.</p>\n\
</body>\n\
</html>";

    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout, "%s", reply);
    if (len <= 0) {
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }
}

// 500服务器内部错误页面
void inner_error(int client_sock) {
    const char *reply = "\
<!DOCTYPE html>\n\
<html>\n\
<head>\n\
    <title>500 Internal Server Error</title>\n\
</head>\n\
<body>\n\
    <h1>500 Internal Server Error</h1>\n\
    <p>Sorry, something went wrong on the server.</p>\n\
</body>\n\
</html>";

    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout, "%s", reply);
    if (len <= 0) {
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }
}

// 501方法未实现页面
void unimplemented(int client_sock) {
    const char *reply = "\
<!DOCTYPE html>\n\
<html>\n\
<head>\n\
    <title>501 Not Implemented</title>\n\
</head>\n\
<body>\n\
    <h1>501 Not Implemented</h1>\n\
    <p>The requested method is not implemented on this server.</p>\n\
</body>\n\
</html>";

    int len = write(client_sock, reply, strlen(reply));
    if (debug)
        fprintf(stdout, "%s", reply);
    if (len <= 0) {
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }
}

// 发送HTTP头部
int headers(int client_sock, FILE *resource) {
    struct stat st;
    char tmp[64];
    char main_header[1024];
    strcpy(main_header, "HTTP/1.0 200 OK\r\nServer: Daj's server\r\nContent-Type: text/html\r\nConnection: Close\r\n");
    // 获取文件ID
    int file_id = fileno(resource);
    // 获取文件ID出错情况
    if (fstat(file_id, &st) == -1) {
        inner_error(client_sock);
        return -1;
    }
    // 获取HTML长度并将Content-Length追加到响应头
    snprintf(tmp, 64, "Content-Length: %ld\r\n\r\n", st.st_size);
    strcat(main_header, tmp);

    if (debug)
        fprintf(stdout, "header: %s\n", main_header);

    // 向socket发送消息并输出错误信息
    if (send(client_sock, main_header, strlen(main_header), 0) < 0) {
        fprintf(stderr, "send failed. data: %s,reason: %s\n", main_header, strerror(errno));
        return -1;
    }
    return 0;
}

// 发送HTML内容
void content(int client_sock, FILE *resource) {
    char buf[1024];
    fgets(buf, sizeof(buf), resource);
    while (!feof(resource)) {
        int len = write(client_sock, buf, strlen(buf));
        // 写入错误处理
        if (len < 0) {
            fprintf(stderr, "send content error. reason: %s\n", strerror(errno));
            break;
        }
        if (debug)
            fprintf(stdout, "%s", buf);

        fgets(buf, sizeof(buf), resource);
    }
}

// 响应HTTP请求
void resp_to_req(int client_sock, const char *path) {
    int ret;
    FILE *resource = NULL;
    resource = fopen(path, "r+");
    if (resource == NULL) {
        file_not_found(client_sock);
        return;
    }

    // 发送HTTP头部
    ret = headers(client_sock, resource);
    // 发送内容
    if (!ret)
        content(client_sock, resource);

    // 关闭文件
    fclose(resource);
}

// 解析HTTP请求
void *parse_http_req(void *pclient_sock) {
    // 请求行长度
    int len;
    // 请求行
    char buf[256];
    // 请求方法
    char req_method[64];
    // 请求资源
    char url[256];
    // 文件路径
    char path[256];
    // 文件元数据结构体
    struct stat st;
    // 客户端socket
    int client_sock = *(int *) pclient_sock;

    // 读取请求行
    len = get_line(client_sock, buf, sizeof(buf));
    // 读取请求行成功，进行处理
    if (len > 0) {
        int i = 0, j = 0;
        while (!isspace(buf[j]) && i < sizeof(req_method) - 1) {
            req_method[i++] = buf[j++];
        }
        req_method[i] = '\0';
        if (debug)
            printf("request method: %s\n", req_method);

        // 处理GET请求
        if (strncasecmp(req_method, "GET", i) == 0) {
            if (debug)
                printf("request method = GET\n");
            // 获取url
            while (isspace(buf[j++]));      // 跳过空白格
            i = 0;
            while (!isspace(buf[j]) && i < sizeof(url) - 1) {
                url[i++] = buf[j++];
            }
            url[i] = '\0';
            if (debug)
                printf("url: %s\n", url);

            // 继续读取请求头
            do {
                len = get_line(client_sock, buf, sizeof(buf));
                if (debug)
                    printf("read: %s\n", buf);
            } while (len > 0);

            // 定位本地HTML文件
            {
                char *pos = strchr(url, '?');
                if (pos) {
                    *pos = '\0';
                    printf("real url: %s\n", url);
                }
            }

            // 将url与目录拼接成路径
            sprintf(path, "./html_docs/%s", url);
            if (debug)
                printf("path: %s\n", path);

            // 判断文件是否存在
            if (stat(path, &st) == -1) {
                // 文件不存在，响应未找到
                fprintf(stderr, "stat %s failed, reason: %s\n", path, strerror(errno));
                file_not_found(client_sock);
            } else {
                // 文件存在，执行HTTP响应
                if (S_ISDIR(st.st_mode)) {
                    strcat(path, "/index.html");
                }
                resp_to_req(client_sock, path);
            }

        } else {
            // 对于非GET请求的响应
            fprintf(stderr, "warning! other request [%s]\n", req_method);
            do {
                len = get_line(client_sock, buf, sizeof(buf));
                if (debug)
                    printf("read: %s\n", buf);
            } while (len > 0);
            unimplemented(client_sock);
        }

    } else {
        bad_request(client_sock);
    }

    // 处理完请求后关闭客户端的socket连接
    close(client_sock);
    // 释放手动分配的内存
    if(pclient_sock)
        free(pclient_sock);
}

int main() {
    // 设置邮箱
    int ret;
    // 创建socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    // 创建socket错误处理
    if (sockfd == -1) {
        err_msg(&sockfd, "create socket");
    }
    // 定义结构体变量
    struct sockaddr_in server_addr;
    // 清零结构体
    bzero(&server_addr, sizeof(server_addr));
    // 选择IPv4协议族
    server_addr.sin_family = AF_INET;
    // 监听本地所有IP地址
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // 绑定端口号
    server_addr.sin_port = htons(SERVER_PORT);
    // 将socket与结构体绑定
    ret = bind(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr));
    // bind错误处理
    if (ret == -1) {
        err_msg(&sockfd, "bind");
    }
    // 设置监听并设定接收队列长度
    ret = listen(sockfd, 128);
    // listen函数的错误处理
    if (ret == -1) {
        err_msg(&sockfd, "listen");
    }
    // 提示语
    printf("等待客户端的连接\n");

    while (1) {
        struct sockaddr_in client;
        socklen_t client_addr_len = sizeof(client);
        char client_ip[64];
        int client_sock, len;
        char buf[256];
        pthread_t id;
        int *pclient_sock = NULL;

        // 建立一个socket描述符对客户端进行响应
        client_sock = accept(sockfd, (struct sockaddr *) &client, &client_addr_len);

        // 输出客户端地址和端口号
        printf("client ip: %s\nport: %d\n",
               inet_ntop(AF_INET, &client.sin_addr.s_addr, client_ip, sizeof(client_ip)),
               ntohs(client.sin_port));

        // 启动线程处理HTTP请求
        pclient_sock = (int *) malloc(sizeof(int));
        *pclient_sock = client_sock;
        pthread_create(&id, NULL, parse_http_req, pclient_sock);
    }
}
