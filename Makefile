# ==============================================================================
# Makefile - Sensor Gateway Smart Home
# ==============================================================================
CC ?= gcc
CFLAGS ?= -Wall -Wextra -pthread -Iinclude -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDFLAGS ?= -pthread -lsqlite3 -lrt -lm

# Thư mục
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = .
TEST_DIR = test

# Danh sách mã nguồn cho Server Gateway
SERVER_SRCS = $(SRC_DIR)/main.c \
              $(SRC_DIR)/sbuffer.c \
              $(SRC_DIR)/protocol_adapter.c \
              $(SRC_DIR)/connmgr.c \
              $(SRC_DIR)/datamgr.c \
              $(SRC_DIR)/storagemgr.c \
              $(SRC_DIR)/shared_status.c \
              $(SRC_DIR)/log_process.c \
              $(SRC_DIR)/web_server.c

# Mã nguồn cho Sensor Node mô phỏng
NODE_SRCS = $(SRC_DIR)/sensor_node.c \
            $(SRC_DIR)/protocol_adapter.c

# Object files
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SERVER_SRCS))
NODE_OBJS = $(OBJ_DIR)/sensor_node.o $(OBJ_DIR)/protocol_adapter.o

# Binaries
TARGET_SERVER = $(BIN_DIR)/server
TARGET_NODE = $(BIN_DIR)/sensor_node

# Mục tiêu mặc định
.PHONY: all clean debug test valgrind helgrind sim

all: $(TARGET_SERVER) $(TARGET_NODE)

# Chế độ Debug (-DDEBUG bật log chẩn đoán, -g để debug gdb)
debug: CFLAGS += -DDEBUG -g -O0
debug: all

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Biên dịch các file .c thành .o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link binary server
$(TARGET_SERVER): $(SERVER_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo ">>> Biên dịch thành công $(TARGET_SERVER)"

# Link binary sensor_node
$(TARGET_NODE): $(NODE_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)
	@echo ">>> Biên dịch thành công $(TARGET_NODE)"

# Biên dịch và chạy bộ Unit Test
test:
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SRC_DIR)/protocol_adapter.c $(TEST_DIR)/test_crc.c -o bin/test_crc $(LDFLAGS)
	./bin/test_crc
	$(CC) $(CFLAGS) $(SRC_DIR)/protocol_adapter.c $(TEST_DIR)/test_adapters.c -o bin/test_adapters $(LDFLAGS)
	./bin/test_adapters
	$(CC) $(CFLAGS) $(SRC_DIR)/sbuffer.c $(TEST_DIR)/test_sbuffer.c -o bin/test_sbuffer $(LDFLAGS)
	./bin/test_sbuffer

# Chạy mô phỏng toàn bộ hệ thống
sim: all
	bash $(TEST_DIR)/run_simulation.sh

# Chạy kịch bản hoàn chỉnh (khởi động tất cả node, web, và tạo file gateway.log thực tế)
run: all
	bash run_full_system.sh

demo: run

# Kiểm tra rò rỉ bộ nhớ với Valgrind
valgrind: debug
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./server 1234 8080

# Kiểm tra data race và đồng bộ hóa đa luồng với Helgrind
helgrind: debug
	valgrind --tool=helgrind ./server 1234 8080

# Dọn dẹp các file sinh ra
clean:
	rm -rf $(OBJ_DIR) bin
	rm -f $(TARGET_SERVER) $(TARGET_NODE)
	rm -f gateway.log logFifo gateway.db gateway.db-wal gateway.db-shm
	@echo ">>> Đã dọn dẹp sạch sẽ tài nguyên build."

