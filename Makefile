# P2P 服务器 + 客户端 SDK 构建（C++17）
# 构建产物：
#   server/natserver/bin/p2p_natserver   信令服务器（注册/心跳/CONNECT 协调/鉴权/NAT 检测）
#   server/proxyserver/bin/p2p_proxy     UDP 中继兜底
#   client/bin/peer                      客户端 SDK 演示程序
CC      := g++
CFLAGS  := -O2 -Wall -Wextra -std=c++17 -pthread -DJUICE_STATIC -I. -Icommon -Iserver/natserver/src -Iserver/proxyserver/src -Ithird_party/libjuice/include
LIBJUICE := third_party/libjuice/build/libjuice.a
LDFLAGS := $(LIBJUICE) -pthread

BIN_NAT   := server/natserver/bin/p2p_natserver
BIN_PROXY := server/proxyserver/bin/p2p_proxy
BIN_PEER  := client/bin/peer
BIN_CRYPTOTEST := tests/bin/crypto_test
BIN_SESSIONTEST := tests/bin/session_test
BIN_UIDTEST := tests/bin/uid_test
BIN_UIDGEN  := tools/bin/uidgen
BIN_AVTEST  := tests/bin/av_frame_test
BIN_IOTCDEMO := client/bin/iotc_demo

COMMON_SRCS := common/Crypto.cpp common/Uid.cpp common/X25519.cpp
IOTC_SRCS := client/sdk/iotc/IOTC.cpp \
             client/sdk/iotc/AVAPIs.cpp \
             client/sdk/iotc/RDTAPIs.cpp \
             client/sdk/iotc/P2PTunnelAPIs.cpp \
             client/sdk/api/P2PClient.cpp

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

all: $(LIBJUICE) $(BIN_NAT) $(BIN_PROXY) $(BIN_PEER) $(BIN_CRYPTOTEST) $(BIN_SESSIONTEST) $(BIN_UIDTEST) $(BIN_UIDGEN) $(BIN_AVTEST) $(BIN_IOTCDEMO)

$(LIBJUICE):
	cmake -B third_party/libjuice/build -S third_party/libjuice -DCMAKE_BUILD_TYPE=Release
	cmake --build third_party/libjuice/build --target juice-static -j
	ln -sfn libjuice-static.a $(LIBJUICE)

$(BIN_NAT): $(NAT_SRCS) $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(NAT_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_PROXY): $(PROXY_SRCS) $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PROXY_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_PEER): $(PEER_SRCS) $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PEER_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_CRYPTOTEST): tests/crypto_test.cpp $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/crypto_test.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_SESSIONTEST): tests/session_test.cpp client/sdk/session/Session.h $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/session_test.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_UIDTEST): tests/uid_test.cpp $(COMMON_SRCS) common/Uid.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/uid_test.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_UIDGEN): tools/uidgen/uidgen.cpp $(COMMON_SRCS) common/Uid.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tools/uidgen/uidgen.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_AVTEST): tests/av_frame_test.cpp client/sdk/iotc/AvCodec.h client/sdk/iotc/TunnelCodec.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/av_frame_test.cpp

$(BIN_IOTCDEMO): client/demo/iotc_demo.cpp $(IOTC_SRCS) $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ client/demo/iotc_demo.cpp $(IOTC_SRCS) $(COMMON_SRCS) $(LDFLAGS)

clean:
	rm -rf server/natserver/bin server/proxyserver/bin client/bin tests/bin tools/bin

test: all
	bash test.sh

.PHONY: all clean test
