# P2P 服务器 + 客户端 SDK 构建（C++17）
# 构建产物：
#   server/natserver/bin/p2p_natserver   信令服务器（注册/心跳/CONNECT 协调/鉴权/NAT 检测）
#   server/proxyserver/bin/p2p_proxy     UDP 中继兜底
#   client/bin/peer                      客户端 SDK 演示程序
CC      := g++
CFLAGS  := -O2 -Wall -Wextra -std=c++17 -pthread -DJUICE_STATIC -I. -Icommon -Iserver/natserver/src -Iserver/proxyserver/src -Ithird_party/libjuice/include
LDFLAGS := third_party/libjuice/build/libjuice.a -pthread

BIN_NAT   := server/natserver/bin/p2p_natserver
BIN_PROXY := server/proxyserver/bin/p2p_proxy
BIN_PEER  := client/bin/peer
BIN_CRYPTOTEST := tests/bin/crypto_test

COMMON_SRCS := common/Crypto.cpp

NAT_SRCS := server/natserver/src/NatServer.cpp \
            server/natserver/src/RecvProcess.cpp \
            server/natserver/src/PeerManage.cpp \
            server/natserver/src/CfgFile.cpp \
            server/natserver/src/AntiAbuse.cpp \
            server/natserver/src/LicenseMgr.cpp \
            server/natserver/src/NatTypeCheck.cpp \
            server/natserver/src/StatusServer.cpp

PROXY_SRCS := server/proxyserver/src/P2PProxy.cpp

PEER_SRCS  := client/demo/peer.cpp \
              client/sdk/api/P2PClient.cpp

all: $(BIN_NAT) $(BIN_PROXY) $(BIN_PEER) $(BIN_CRYPTOTEST)

$(BIN_NAT): $(NAT_SRCS) $(COMMON_SRCS) common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(NAT_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_PROXY): $(PROXY_SRCS) $(COMMON_SRCS) common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PROXY_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_PEER): $(PEER_SRCS) $(COMMON_SRCS) common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PEER_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_CRYPTOTEST): tests/crypto_test.cpp $(COMMON_SRCS) common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/crypto_test.cpp $(COMMON_SRCS) $(LDFLAGS)

clean:
	rm -rf server/natserver/bin server/proxyserver/bin client/bin tests/bin

test: all
	bash test.sh

.PHONY: all clean test
