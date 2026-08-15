# P2P 客户端 SDK 独立构建（C++17）
# 用法：
#   make            编译 peer 演示程序
#   make clean      清理构建产物
#   make run ARGS="..."  编译并运行（ARGS 传给 peer）
#
# 依赖：
#   ../common/          Crypto.cpp + 头文件（Crypto.h / ProtoDef.h / Log.h / Util.h）
#   ../third_party/libjuice/  libjuice 静态库 + juice.h 头文件
#
# 构建产物：
#   bin/peer            客户端 SDK 演示程序

CC      := g++
CFLAGS  := -O2 -Wall -Wextra -std=c++17 -pthread -DJUICE_STATIC
CFLAGS  += -I. -I.. -I../common -I../third_party/libjuice/include
LDFLAGS := ../third_party/libjuice/build/libjuice.a -pthread -lssl -lcrypto

BIN_PEER := bin/peer

PEER_SRCS := demo/peer.cpp \
             sdk/api/P2PClient.cpp

COMMON_SRCS := ../common/Crypto.cpp ../common/TlsIo.cpp ../common/PortMap.cpp

HEADERS := sdk/api/P2PClient.h \
           sdk/plat/Plat.h \
           sdk/proto/Codec.h \
           sdk/session/Session.h \
           sdk/transport/NatDetect.h \
           sdk/transport/UdpSocket.h \
           ../common/Crypto.h \
           ../common/ProtoDef.h \
           ../common/Log.h \
           ../common/Util.h

all: $(BIN_PEER)

$(BIN_PEER): $(PEER_SRCS) $(COMMON_SRCS) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PEER_SRCS) $(COMMON_SRCS) $(LDFLAGS)

clean:
	rm -rf bin

run: all
	./$(BIN_PEER) $(ARGS)

.PHONY: all clean run
