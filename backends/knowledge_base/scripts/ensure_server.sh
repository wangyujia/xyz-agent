#!/bin/bash
# KB HTTP Server 保活脚本
# 检查 :8900 是否在监听，若不在则启动

PORT=8900
PIDFILE="/tmp/kb_server.pid"
LOGFILE="/root/code/knowledge_base/server.log"
WORKDIR="/root/code/knowledge_base"

# 检查端口是否已在监听
if lsof -ti:$PORT > /dev/null 2>&1; then
    exit 0  # 正常运行，静默退出
fi

# 端口未监听，启动服务
cd "$WORKDIR"
nohup python -m server.main >> "$LOGFILE" 2>&1 &
echo $! > "$PIDFILE"

# 等待启动
sleep 2

# 验证
if lsof -ti:$PORT > /dev/null 2>&1; then
    echo "KB Server started on :$PORT (PID: $(cat $PIDFILE))"
else
    echo "ERROR: KB Server failed to start. Check $LOGFILE"
    exit 1
fi
