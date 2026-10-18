#!/bin/bash
# run_all_tests.sh - Chạy tự động toàn bộ bộ kiểm thử đơn vị

set -e

echo "================================================================"
echo "          TIẾN HÀNH BIÊN DỊCH VÀ CHẠY TOÀN BỘ UNIT TEST         "
echo "================================================================"

mkdir -p bin

# 1. Test CRC-16
echo "[1/3] Biên dịch và chạy test_crc..."
gcc -Wall -Wextra -pthread -Iinclude src/protocol_adapter.c test/test_crc.c -o bin/test_crc
./bin/test_crc

# 2. Test Adapters
echo "[2/3] Biên dịch và chạy test_adapters..."
gcc -Wall -Wextra -pthread -Iinclude src/protocol_adapter.c test/test_adapters.c -o bin/test_adapters
./bin/test_adapters

# 3. Test sbuffer đa luồng
echo "[3/3] Biên dịch và chạy test_sbuffer..."
gcc -Wall -Wextra -pthread -Iinclude src/sbuffer.c test/test_sbuffer.c -o bin/test_sbuffer
./bin/test_sbuffer

echo "================================================================"
echo "    >>> TẤT CẢ CÁC BÀI KIỂM THỬ ĐÃ ĐẠT 100% TIÊU CHUẨN! <<<     "
echo "================================================================"

