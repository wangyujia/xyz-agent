#!/bin/bash
# Knowledge Base HTTP Server 启动脚本
# 用法: bash start_server.sh [--daemon]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KB_ROOT="$SCRIPT_DIR"
PID_FILE="$KB_ROOT/.server.pid"
LOG_FILE="$KB_ROOT/.server.log"

export KB_ROOT

start_server() {
    # 检查是否已运行
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Server already running (PID: $pid)"
            return 0
        fi
        rm -f "$PID_FILE"
    fi

    cd "$KB_ROOT"

    if [ "$1" = "--daemon" ]; then
        nohup python -m server.main > "$LOG_FILE" 2>&1 &
        echo $! > "$PID_FILE"
        echo "Server started in background (PID: $!)"
        echo "Log: $LOG_FILE"
    else
        python -m server.main 2>&1 | tee "$LOG_FILE"
    fi
}

stop_server() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid"
            echo "Server stopped (PID: $pid)"
        fi
        rm -f "$PID_FILE"
    else
        # fallback: kill by port
        local pids=$(lsof -ti:8900 2>/dev/null)
        if [ -n "$pids" ]; then
            echo "$pids" | xargs kill
            echo "Server stopped (port 8900)"
        else
            echo "Server not running"
        fi
    fi
}

status_server() {
    if [ -f "$PID_FILE" ]; then
        local pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Running (PID: $pid)"
            return 0
        fi
    fi
    # check port
    if lsof -ti:8900 >/dev/null 2>&1; then
        echo "Running (port 8900 occupied)"
        return 0
    fi
    echo "Stopped"
    return 1
}

case "${1:-start}" in
    start|--daemon)
        start_server "$1"
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        sleep 1
        start_server --daemon
        ;;
    status)
        status_server
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|--daemon}"
        ;;
esac
