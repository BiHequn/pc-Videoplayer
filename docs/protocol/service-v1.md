# 本地演示服务协议 v1

服务监听 `127.0.0.1`，默认端口 18760。客户端接受 localhost 或 IPv4 地址。每条 TCP 消息先写四字节网络字节序无符号长度，再写 UTF-8 JSON。长度必须为 1～2097152 字节；接收方完整收集长度和正文，支持 TCP 拆包、合包。无效长度或 JSON 关闭连接。

请求：

```json
{"version":1,"id":"随机请求标识","op":"login","body":{"username":"demo","password":"example"}}
```

响应：

```json
{"version":1,"id":"原请求标识","op":"login","ok":true,"code":"OK","body":{"username":"demo","token":"随机会话令牌"}}
```

客户端核对 version、id、op、ok、code、body，单次请求读写共用五秒期限。传输异常重新连接，重试间隔 1、2、4 秒，保留待重试请求 id。空闲连接每两秒发送 heartbeat，服务端五秒读超时关闭没有心跳的连接。自动重连首先用令牌恢复会话，再查询上传的已落盘偏移。

| op | body | 成功结果 |
| --- | --- | --- |
| register | username、password | username |
| login | username、password | username、token |
| heartbeat | 空对象 | 空对象 |
| session.restore | token | username |
| logout | token | 空对象 |
| upload.init | token、filename、size、sha256 | uploadId、offset、size、sha256、filename、state |
| upload.status | token、uploadId | 同初始化 |
| upload.chunk | token、uploadId、offset、sha256、data | 同初始化；offset 为已确认字节 |
| upload.finish | token、uploadId | state=completed、path |
| upload.cancel | token、uploadId | state=canceled；已完成任务保持 completed |

账号名 1～64 UTF-8 字节，不含控制字符；密码 1～1024 字节。密码用 BCrypt PBKDF2-HMAC-SHA256、16 字节随机盐、600000 次迭代生成 32 字节哈希。令牌为 32 字节随机数。账号和会话以 JSON 原子替换持久化；不保存明文密码。服务启动前独占规范化数据目录中的 `.service.lock` 文件句柄，工作线程全部退出后释放；不同端口或不同进程也不能同时写同一数据目录，进程异常退出由操作系统释放句柄。该演示传输为本机 TCP，未增加 TLS 或公网账号治理。

上传 data 为小写十六进制编码的文件字节，每块最多 256 KiB；sha256 是该块的小写 SHA-256。文件任务以完整 SHA-256 和 size 标识，按账号隔离存储。服务端刷盘文件块并原子发布偏移记录后确认。重复偏移的相同字节幂等确认，冲突返回 CHUNK_CONFLICT，偏移不同返回 OFFSET_MISMATCH。重启时丢弃超过已落盘记录的尾部字节。完成时校验整文件哈希再发布 `.bin` 文件，filename 仅作展示，路径使用生成的标识。

暂停保留可恢复进度。取消上传删除未完成的服务端临时数据；重新上传从零开始。取消响应以服务端状态为准：已经发布的任务保留 completed，落盘失败保留 IO_ERROR，不将这些结果覆盖为 canceled。取消下载会保存缓存进度。客户端退出后重新登录并选择同一上传文件，可从服务端已确认偏移继续。

上传请求收到 UNAUTHORIZED 时，客户端清除令牌、用户名和登录状态，并同时更新账号及上传错误；TCP 连接可继续用于重新登录。重连后的有效令牌可恢复原会话，主动断开则清除本地登录状态。

常见错误码：BAD_REQUEST、BAD_VERSION、UNKNOWN_OP、BAD_CREDENTIALS、ACCOUNT_EXISTS、UNAUTHORIZED、INVALID_UPLOAD、INVALID_CHUNK、HASH_MISMATCH、OFFSET_MISMATCH、CHUNK_CONFLICT、INCOMPLETE、NOT_FOUND、IO_ERROR。客户端将业务错误与连接错误分开显示。
