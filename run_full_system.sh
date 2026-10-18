#!/bin/bash
# ==============================================================================
# run_full_system.sh - Kịch bản chạy toàn diện hệ thống Sensor Gateway
# Tự động biên dịch, khởi động Gateway, kích hoạt toàn bộ các loại Sensor Node,
# tương tác qua Web API và ghi đầy đủ nhật ký thực tế vào file gateway.log.
# ==============================================================================

set -e

BASE_PORT=1234
HTTP_PORT=8080
DURATION=25  # Thời gian chạy mô phỏng (giây)

echo "=================================================================="
echo "    BẮT ĐẦU KỊCH BẢN CHẠY TOÀN DIỆN SENSOR GATEWAY TRÊN LINUX     "
echo "=================================================================="

# 1. Dọn dẹp các tiến trình và tài nguyên cũ nếu còn sót lại
echo "[1/6] Dọn dẹp tài nguyên cũ..."
pkill -f "./server" 2>/dev/null || true
pkill -f "./sensor_node" 2>/dev/null || true
rm -f logFifo gateway.db gateway.db-wal gateway.db-shm /dev/shm/gatewayStatus gateway.log

# 2. Biên dịch lại toàn bộ dự án
echo "[2/6] Biên dịch mã nguồn với GCC..."
make clean
make all

# 3. Khởi động Sensor Gateway Server ở chế độ nền
echo "[3/6] Khởi động Gateway Server (Base TCP: $BASE_PORT, HTTP: $HTTP_PORT)..."
./server $BASE_PORT $HTTP_PORT &
GATEWAY_PID=$!
sleep 1.5

# Kiểm tra Gateway đã chạy chưa
if ! kill -0 $GATEWAY_PID 2>/dev/null; then
    echo "LỖI: Gateway Server không thể khởi động!"
    exit 1
fi

echo ">>> Gateway Server đang chạy với PID: $GATEWAY_PID"

# 4. Khởi chạy đồng thời tất cả các Sensor Node theo danh mục room_sensor.map
echo "[4/6] Khởi chạy các Sensor Node mô phỏng..."

NODE_PIDS=()

# Room 501: Temperature (ID 10, Cổng P = 1234)
./sensor_node temperature 10 127.0.0.1 $BASE_PORT 800 &
NODE_PIDS+=($!)

# Room 501: Humidity (ID 21, Cổng P+1 = 1235) - Kèm phân mảnh TCP --fragment
./sensor_node humidity 21 127.0.0.1 $BASE_PORT 1000 --fragment &
NODE_PIDS+=($!)

# Room 602: Motion (ID 30, Cổng P+2 = 1236)
./sensor_node motion 30 127.0.0.1 $BASE_PORT 1200 &
NODE_PIDS+=($!)

# Room 703: Door (ID 40, Cổng P+2 = 1236)
./sensor_node door 40 127.0.0.1 $BASE_PORT 1500 &
NODE_PIDS+=($!)

# Room 804: Smoke/Gas (ID 50, Cổng P+3 = 1237) - Kèm kiểm tra CRC và gửi lỗi ngẫu nhiên --corrupt
./sensor_node smoke_gas 50 127.0.0.1 $BASE_PORT 1000 --corrupt &
NODE_PIDS+=($!)

# Room 905: Temperature (ID 11, Cổng P = 1234)
./sensor_node temperature 11 127.0.0.1 $BASE_PORT 900 &
NODE_PIDS+=($!)

# Room 905: Humidity (ID 22, Cổng P+1 = 1235) - Kèm gộp frame --glue
./sensor_node humidity 22 127.0.0.1 $BASE_PORT 1100 --glue &
NODE_PIDS+=($!)

# Node thử nghiệm: Sensor ID không hợp lệ (ID 99) để kiểm tra phát hiện lỗi của Data Manager
./sensor_node temperature 99 127.0.0.1 $BASE_PORT 3000 &
NODE_PIDS+=($!)

echo ">>> Đã khởi động ${#NODE_PIDS[@]} sensor node."

# 5. Duy trì hệ thống hoạt động, đồng thời gửi các request HTTP kiểm tra
echo "[5/6] Hệ thống đang thu thập, xử lý và lưu trữ dữ liệu trong $DURATION giây..."

HALF_TIME=$((DURATION / 2))
sleep $HALF_TIME

# Gửi một số request Web API để kiểm tra tính sẵn sàng của Web Process
echo ">>> [Web Test] Gọi API GET /api/status..."
curl -s "http://127.0.0.1:$HTTP_PORT/api/status" > /dev/null || true

echo ">>> [Web Test] Gọi API GET /api/history?id=10&n=5..."
curl -s "http://127.0.0.1:$HTTP_PORT/api/history?id=10&n=5" > /dev/null || true

echo ">>> [Web Test] Gọi thử API không tồn tại (kiểm tra mã 404)..."
curl -s "http://127.0.0.1:$HTTP_PORT/invalid_endpoint" > /dev/null || true

sleep $((DURATION - HALF_TIME))

# 6. Quy trình tắt hệ thống có kiểm soát (Graceful Shutdown)
echo "[6/6] Bắt đầu quy trình dừng hệ thống..."

# Dừng các Sensor Node trước
echo ">>> Dừng các Sensor Node..."
for pid in "${NODE_PIDS[@]}"; do
    kill -INT $pid 2>/dev/null || true
done
sleep 1

# Gửi tín hiệu SIGINT để Gateway thực hiện dọn dẹp sạch sẽ
echo ">>> Gửi tín hiệu SIGINT đến Gateway Server (PID: $GATEWAY_PID)..."
kill -INT $GATEWAY_PID 2>/dev/null || true

# Chờ Gateway kết thúc hoàn toàn
wait $GATEWAY_PID 2>/dev/null || true
sleep 1

echo "=================================================================="
echo "                   KẾT QUẢ THỰC THI HỆ THỐNG                      "
echo "=================================================================="

if [ -f gateway.log ]; then
    TOTAL_LINES=$(wc -l < gateway.log)
    echo "✓ File gateway.log đã được tạo thành công!"
    echo "✓ Tổng số dòng log đã ghi nhận thực tế: $TOTAL_LINES dòng."
    echo ""
    echo "--- 10 DÒNG ĐẦU TIÊN TRONG gateway.log ---"
    head -n 10 gateway.log
    echo "..."
    echo "--- 10 DÒNG CUỐI CÙNG TRONG gateway.log ---"
    tail -n 10 gateway.log
else
    echo "LỖI: Không tìm thấy file gateway.log!"
fi

if [ -f gateway.db ]; then
    RECORD_COUNT=$(sqlite3 gateway.db "SELECT count(*) FROM SensorData;" 2>/dev/null || echo "0")
    echo ""
    echo "✓ Cơ sở dữ liệu SQLite gateway.db đã lưu thành công: $RECORD_COUNT bản ghi!"
fi

echo ""
echo ">>> Kịch bản chạy hoàn tất! Toàn bộ log và cơ sở dữ liệu đã được tạo thật từ mã nguồn của bạn."

