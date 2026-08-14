"""XN 私有协议客户端模块（对应服务端 common/ProtoDef.h，纯 Python，无第三方依赖）。

报文头：8 字节
    magic(2B,BE)=0x584E  version(1B)=0x02  msg_id(1B)  length(4B,BE)

UuidReq（43 字节）: uuid[33] + dev_type(1) + nattype(1) + lan_port(2,BE)
                    + session_pts(4,BE) + extlen(2,BE)
"""

import socket
import struct

MAGIC = 0x584E
VER = 0x02
MAX_UUID_LEN = 32
MAX_PKT = 2048

MSG_HEARTBEAT_RSP = 0x02
MSG_ADD_UID_REQ = 0x0D
MSG_DELETE_UID_REQ = 0x0E
MSG_CHECK_UID_REQ = 0x0F
MSG_CHECK_UID_RSP = 0x12
MSG_HEARTBEAT_RSP_ENC = 0x1C

HEAD = struct.Struct(">HBBI")
UUID_REQ_FMT = struct.Struct("33sBBHIH")
EXTINFO_REQ_FMT = struct.Struct("33s")

RESULT_TXT = {
    "ok": "成功",
    "auth": "需鉴权",
    "reject": "被拒绝",
    "offline": "未在线/未授权",
    "online": "在线/已授权",
    "none": "无应答",
}

ST_OK = 0
ST_AUTH = 1
ST_REJECT = 2
ST_NORSP = 3
ST_ENC = 4
ST_OFFLINE = 5

RSP_ADD_DEL_MAP = {0: ST_OK, 1: ST_AUTH, 2: ST_REJECT}
RSP_CHECK_MAP = {0: ST_OFFLINE, 1: ST_OK}


def build_head(msg_id, length):
    return HEAD.pack(MAGIC, VER, msg_id, length)


def build_uuid_req(msg_id, uuid_str):
    uuid_bytes = uuid_str[:MAX_UUID_LEN].encode("utf-8")
    payload = UUID_REQ_FMT.pack(uuid_bytes, 2, 0, 0, 0, 0)
    return build_head(msg_id, len(payload)) + payload


def build_check_req(uuid_str):
    uuid_bytes = uuid_str[:MAX_UUID_LEN].encode("utf-8")
    payload = EXTINFO_REQ_FMT.pack(uuid_bytes)
    return build_head(MSG_CHECK_UID_REQ, len(payload)) + payload


def parse_result(msg_id, data):
    if len(data) < HEAD.size:
        return None
    head = HEAD.unpack_from(data, 0)
    if head[0] != MAGIC or head[1] != VER:
        return None
    rsp_id = head[2]
    plen = head[3]
    payload = data[HEAD.size:HEAD.size + plen]
    if rsp_id == MSG_HEARTBEAT_RSP_ENC:
        return ST_ENC
    if rsp_id == MSG_HEARTBEAT_RSP and len(payload) >= 1:
        code = payload[0]
        return RSP_ADD_DEL_MAP.get(code, ST_NORSP)
    if rsp_id == MSG_CHECK_UID_RSP and len(payload) >= 1:
        return RSP_CHECK_MAP.get(payload[0], ST_NORSP)
    return None


class XnatClient:
    def __init__(self, server, port, timeout=0.8):
        self.addr = (server, int(port))
        self.timeout = timeout

    def _recv_one(self, sock, expect_rsp):
        if not expect_rsp:
            return ST_OK
        try:
            data, _ = sock.recvfrom(MAX_PKT)
        except socket.timeout:
            return ST_NORSP
        return parse_result(MSG_ADD_UID_REQ, data)

    def batch_send(self, uuids, msg_id, expect_rsp=True, send_size=16, timeout=None):
        """批量发送 ADD/DELETE/CHECK，返回 [(uuid, 状态码)]。

        状态码: 0=成功 1=需鉴权 2=被拒绝 3=无应答 4=加密应答(未配置密钥，视作需鉴权)
        """
        tmo = timeout if timeout is not None else self.timeout
        window = max(1, min(send_size, 256))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(tmo)
        results = []
        n = len(uuids)
        i = 0
        try:
            while i < n:
                end = min(i + window, n)
                for u in uuids[i:end]:
                    sock.sendto(build_uuid_req(msg_id, u), self.addr)
                for k in range(end - i):
                    status = self._recv_one(sock, expect_rsp)
                    results.append((uuids[i + k], status))
                    if expect_rsp and status == ST_NORSP:
                        for j in range(k + 1, end - i):
                            results.append((uuids[i + j], ST_NORSP))
                        break
                i = end
        finally:
            sock.close()
        return results

    def add(self, uuids, send_size=16):
        return self.batch_send(uuids, MSG_ADD_UID_REQ, True, send_size)

    def delete(self, uuids, send_size=16):
        return self.batch_send(uuids, MSG_DELETE_UID_REQ, False, send_size)

    def check(self, uuids, send_size=16):
        results = []
        window = max(1, min(send_size, 256))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(self.timeout)
        n = len(uuids)
        i = 0
        try:
            while i < n:
                end = min(i + window, n)
                for u in uuids[i:end]:
                    sock.sendto(build_check_req(u), self.addr)
                for k in range(end - i):
                    status = self._recv_one(sock, True)
                    results.append((uuids[i + k], status))
                    if status == ST_NORSP:
                        for j in range(k + 1, end - i):
                            results.append((uuids[i + j], ST_NORSP))
                        break
                i = end
        finally:
            sock.close()
        return results

    def test_conn(self):
        """发送单条 UID 授权探测服务端连通性，返回 (ok, 描述)。"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(max(0.5, self.timeout))
        try:
            sock.sendto(build_uuid_req(MSG_ADD_UID_REQ, "conn-test"), self.addr)
            status = self._recv_one(sock, True)
        except socket.timeout:
            status = ST_NORSP
        except OSError as e:
            sock.close()
            return False, str(e)
        sock.close()
        if status == ST_OK:
            return True, "连接成功，服务端应答正常"
        if status == ST_ENC:
            return True, "服务端已开启鉴权（需要 AuthSecret 密钥）"
        if status == ST_AUTH:
            return True, "已连通，服务端要求鉴权"
        if status == ST_REJECT:
            return True, "已连通，该 UID 被服务端拒绝"
        return False, "无应答，请检查地址/端口/防火墙"
