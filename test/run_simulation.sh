#!/bin/bash
# run_simulation.sh - Kịch bản mô phỏng toàn bộ hệ thống Sensor Gateway với 5 loại cảm biến

echo "================================================================"
echo "          BẮT ĐẦU MÔ PHỎNG HỆ THỐNG SENSOR GATEWAY              "
echo "================================================================"

# Đảm bảo dọn dẹp các tiến trình cũ nếu còn
pkill -f "./server" || true
pkill -f "./sensor_node" || true
rm -f logFifo gateway.db gateway.db-wal gateway.db-shm /dev/shm/gatewayStatus

# 1. Khởi chạy Gateway Server (Base port 1234, HTTP port 8080)
echo "[1/4] Khởi động Server Gateway (cổng TCP 1234, HTTP 8080)..."
./server 1234 8080 &
SERVER_PID=$!
sleep 1

# 2. Khởi chạy đồng thời 5 loại sensor node và 1 node ID giả lập lỗi
echo "[2/4] Khởi động các sensor node mô phỏng..."
./sensor_node temperature 10 127.0.0.1 1234 600 &
PID_T=$!

./sensor_node humidity 21 127.0.0.1 1234 700 --fragment &
PID_H=$!

./sensor_node motion 30 127.0.0.1 1234 1000 &
PID_M=$!

./sensor_node door 40 127.0.0.1 1234 900 &
PID_D=$!

./sensor_node smoke_gas 50 127.0.0.1 1234 800 --corrupt &
PID_S=$!

# Node thử nghiệm sensor ID không hợp lệ (không có trong room_sensor.map)
./sensor_node temperature 99 127.0.0.1 1234 1000 &
PID_BAD=$!

echo "[3/4] Đang thu thập và xử lý dữ liệu trong 10 giây..."
sleep 5

echo "--- THỬ NGHIỆM GỌI API /api/status ---"
curl -s http://127.0.0.1:8080/api/status | head -n 30 || true

echo -e "\n--- THỬ NGHIỆM GỌI API /api/history?id=10&n=3 ---"
curl -s "http://127.0.0.1:8080/api/history?id=10&n=3" || true

sleep 5

echo -e "\n--- TRÍCH XUẤT 15 DÒNG NHẬT KÝ gateway.log MỚI NHẤT ---"
if [ -f gateway.log ]; then
    tail -n 15 gateway.log
fi

# 3. Kết thúc mô phỏng và kiểm tra graceful shutdown
echo -e "\n[4/4] Dừng các sensor node và phát tín hiệu SIGINT tắt Gateway..."
kill -INT $PID_T $PID_H $PID_M $PID_D $PID_S $PID_BAD 2>/dev/null || true
sleep 1
kill -INT $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo "================================================================"
echo "          MÔ PHỎNG HOÀN TẤT THÀNH CÔNG VÀ AN TOÀN!              "
echo "================================================================"

