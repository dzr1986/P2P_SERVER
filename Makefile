# P2P 服务器 + 客户端 SDK 构建（C++17）
# 构建产物：
#   server/natserver/bin/p2p_natserver   信令服务器（注册/心跳/CONNECT 协调/鉴权/NAT 检测）
#   server/proxyserver/bin/p2p_proxy     UDP 中继兜底
#   client/bin/peer                      客户端 SDK 演示程序
CC      := g++
CFLAGS  := -O2 -Wall -Wextra -std=c++17 -pthread -DJUICE_STATIC -I. -Icommon -Iserver/natserver/src -Iserver/proxyserver/src -Ithird_party/libjuice/include
LIBJUICE := third_party/libjuice/build/libjuice.a
LDFLAGS := $(LIBJUICE) -pthread -lssl -lcrypto

BIN_NAT   := server/natserver/bin/p2p_natserver
BIN_PROXY := server/proxyserver/bin/p2p_proxy
BIN_PEER  := client/bin/peer
BIN_CRYPTOTEST := tests/bin/crypto_test
BIN_SESSIONTEST := tests/bin/session_test
BIN_UIDTEST := tests/bin/uid_test
BIN_UIDGEN  := tools/bin/uidgen
BIN_TOKENGEN := tools/bin/tokengen
BIN_TOKENTEST := tests/bin/token_test
BIN_AVTEST  := tests/bin/av_frame_test
BIN_SCHEDTEST := tests/bin/sched_test
BIN_STUNTEST := tests/bin/stun_test
BIN_ABRTEST := tests/bin/abr_test
BIN_ICESDPTEST := tests/bin/ice_sdp_test
BIN_TWCCTEST := tests/bin/twcc_test
BIN_PORTMAPTEST := tests/bin/portmap_test
BIN_NATDETECTTEST := tests/bin/nat_detect_test
BIN_TCPPUNCHTEST := tests/bin/tcp_punch_test
BIN_TCPSTUNTEST := tests/bin/tcp_stun_test
BIN_NATSIMTEST := tests/bin/nat_sim_test
BIN_PATHSELECTTEST := tests/bin/path_select_test
BIN_IOTCDEMO := client/bin/iotc_demo
BIN_WAKE    := server/wakeserver/bin/p2p_wakeserver

COMMON_SRCS := common/Crypto.cpp common/Uid.cpp common/X25519.cpp common/ConnectToken.cpp common/TlsIo.cpp common/PortMap.cpp
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

all: $(LIBJUICE) $(BIN_NAT) $(BIN_PROXY) $(BIN_PEER) $(BIN_CRYPTOTEST) $(BIN_SESSIONTEST) $(BIN_UIDTEST) $(BIN_UIDGEN) $(BIN_TOKENGEN) $(BIN_TOKENTEST) $(BIN_AVTEST) $(BIN_SCHEDTEST) $(BIN_STUNTEST) $(BIN_ABRTEST) $(BIN_ICESDPTEST) $(BIN_TWCCTEST) $(BIN_PORTMAPTEST) $(BIN_NATDETECTTEST) $(BIN_TCPPUNCHTEST) $(BIN_TCPSTUNTEST) $(BIN_NATSIMTEST) $(BIN_PATHSELECTTEST) $(BIN_IOTCDEMO) $(BIN_WAKE)

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

$(BIN_TOKENGEN): tools/tokengen/tokengen.cpp $(COMMON_SRCS) common/ConnectToken.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tools/tokengen/tokengen.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_TOKENTEST): tests/token_test.cpp $(COMMON_SRCS) common/ConnectToken.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/token_test.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_AVTEST): tests/av_frame_test.cpp client/sdk/iotc/AvCodec.h client/sdk/iotc/TunnelCodec.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/av_frame_test.cpp

$(BIN_SCHEDTEST): tests/sched_test.cpp $(COMMON_SRCS) common/RegionSched.h common/Uid.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/sched_test.cpp $(COMMON_SRCS) $(LDFLAGS)

$(BIN_STUNTEST): tests/stun_test.cpp common/StunBind.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/stun_test.cpp

$(BIN_ABRTEST): tests/abr_test.cpp common/AbrEstimate.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/abr_test.cpp

$(BIN_ICESDPTEST): tests/ice_sdp_test.cpp common/IceSdp.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/ice_sdp_test.cpp

$(BIN_TWCCTEST): tests/twcc_test.cpp common/TwccEstimate.h common/AbrEstimate.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/twcc_test.cpp

$(BIN_PORTMAPTEST): tests/portmap_test.cpp common/PortMap.cpp common/PortMap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/portmap_test.cpp common/PortMap.cpp $(LDFLAGS)

$(BIN_NATDETECTTEST): tests/nat_detect_test.cpp client/sdk/transport/NatDetect.h common/NatMatrix.h common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/nat_detect_test.cpp

$(BIN_TCPPUNCHTEST): tests/tcp_punch_test.cpp common/TcpPunch.h common/Net.h common/Packet.h common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/tcp_punch_test.cpp

$(BIN_TCPSTUNTEST): tests/tcp_stun_test.cpp common/StunBind.h common/TcpPunch.h common/Net.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/tcp_stun_test.cpp

$(BIN_NATSIMTEST): tests/nat_sim_test.cpp common/NatSim.h common/NatMatrix.h common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/nat_sim_test.cpp

$(BIN_PATHSELECTTEST): tests/path_select_test.cpp common/PathSelect.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/path_select_test.cpp

$(BIN_IOTCDEMO): client/demo/iotc_demo.cpp $(IOTC_SRCS) $(COMMON_SRCS) common/ProtoDef.h $(LIBJUICE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ client/demo/iotc_demo.cpp $(IOTC_SRCS) $(COMMON_SRCS) $(LDFLAGS)

$(BIN_WAKE): server/wakeserver/WakeServer.cpp server/wakeserver/WakeServer.h $(COMMON_SRCS) common/ProtoDef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ server/wakeserver/WakeServer.cpp $(COMMON_SRCS) $(LDFLAGS)

clean:
	rm -rf server/natserver/bin server/proxyserver/bin client/bin tests/bin tools/bin

test: all
	bash test.sh

.PHONY: all clean test
