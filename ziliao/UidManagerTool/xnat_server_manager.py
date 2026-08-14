"""XnatServerManager —— 客户端授权 UID 管理工具（tkinter 图形界面）。

对标 ziliao/XnatServerManager.exe 的功能：
  - 服务器地址 + 端口
  - 导入/导出 UID txt 文件
  - 本地 UID 列表 + 授权结果双列表
  - 批量授权 AddBatchUIDs / 删除 / 校验，进度条 + 统计

运行：python xnat_server_manager.py
打包：pyinstaller -F -w -n XnatServerManager xnat_server_manager.py
"""

import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from uid_protocol import (ST_AUTH, ST_ENC, ST_NORSP, ST_OFFLINE, ST_OK,
                          ST_REJECT, MSG_ADD_UID_REQ, MSG_DELETE_UID_REQ,
                          XnatClient)

APP_TITLE = "XnatServerManager - 客户端授权 UID 管理工具 V1.0.2"


def status_text(code, op="add"):
    if op == "check":
        return {ST_OK: "已授权/在线", ST_OFFLINE: "未授权", ST_NORSP: "无应答",
                ST_ENC: "加密应答"}.get(code, "未知")
    return {ST_OK: "成功", ST_AUTH: "需鉴权", ST_REJECT: "被拒绝",
            ST_NORSP: "无应答", ST_ENC: "加密应答(需密钥)"}.get(code, "未知")


class App:
    def __init__(self, root):
        self.root = root
        self.uids = []
        self.results = []
        self.busy = False
        self.q = queue.Queue()

        root.title(APP_TITLE)
        root.geometry("980x640")
        root.minsize(860, 560)

        self._build_topbar()
        self._build_lists()
        self._build_actions()
        self._build_log()
        self._status("就绪")

        root.after(80, self._poll_queue)

    def _build_topbar(self):
        top = ttk.Frame(self.root, padding=(8, 6))
        top.pack(fill="x")
        ttk.Label(top, text="服务器 IP:").pack(side="left")
        self.ip_var = tk.StringVar(value="127.0.0.1")
        ttk.Entry(top, textvariable=self.ip_var, width=18).pack(side="left", padx=(2, 8))
        ttk.Label(top, text="端口:").pack(side="left")
        self.port_var = tk.StringVar(value="16001")
        ttk.Entry(top, textvariable=self.port_var, width=8).pack(side="left", padx=2)
        ttk.Button(top, text="连接测试", command=self.on_test).pack(side="left", padx=8)
        ttk.Label(top, text="并发数:").pack(side="left", padx=(12, 0))
        self.win_var = tk.StringVar(value="16")
        ttk.Spinbox(top, from_=1, to=256, textvariable=self.win_var, width=6).pack(side="left", padx=2)

        bar = ttk.Frame(self.root, padding=(8, 0))
        bar.pack(fill="x")
        ttk.Button(bar, text="导入UID文件", command=self.on_load).pack(side="left")
        ttk.Button(bar, text="导出结果", command=self.on_export).pack(side="left", padx=6)
        ttk.Button(bar, text="清空列表", command=self.on_clear).pack(side="left")
        ttk.Label(bar, text="手动添加:").pack(side="left", padx=(14, 2))
        self.add_var = tk.StringVar()
        ttk.Entry(bar, textvariable=self.add_var, width=24).pack(side="left", padx=2)
        ttk.Button(bar, text="+", width=3, command=self.on_add_one).pack(side="left")

    def _build_lists(self):
        mid = ttk.Frame(self.root, padding=(8, 4))
        mid.pack(fill="both", expand=True)

        left = ttk.LabelFrame(mid, text="本地待授权 UID 列表", padding=4)
        left.pack(side="left", fill="both", expand=True)
        self.local_tree = ttk.Treeview(left, columns=("no", "uid"), show="headings", height=16)
        self.local_tree.heading("no", text="#")
        self.local_tree.heading("uid", text="UID")
        self.local_tree.column("no", width=46, anchor="center", stretch=False)
        self.local_tree.column("uid", width=300, anchor="w")
        sb1 = ttk.Scrollbar(left, orient="vertical", command=self.local_tree.yview)
        self.local_tree.configure(yscrollcommand=sb1.set)
        self.local_tree.pack(side="left", fill="both", expand=True)
        sb1.pack(side="right", fill="y")

        right = ttk.LabelFrame(mid, text="授权结果", padding=4)
        right.pack(side="right", fill="both", expand=True)
        self.result_tree = ttk.Treeview(right, columns=("uid", "st"), show="headings", height=16)
        self.result_tree.heading("uid", text="UID")
        self.result_tree.heading("st", text="状态")
        self.result_tree.column("uid", width=240, anchor="w")
        self.result_tree.column("st", width=110, anchor="center")
        sb2 = ttk.Scrollbar(right, orient="vertical", command=self.result_tree.yview)
        self.result_tree.configure(yscrollcommand=sb2.set)
        self.result_tree.pack(side="left", fill="both", expand=True)
        sb2.pack(side="right", fill="y")

    def _build_actions(self):
        act = ttk.Frame(self.root, padding=(8, 4))
        act.pack(fill="x")
        self.btn_add = ttk.Button(act, text="批量授权 AddBatchUIDs", command=self.on_add_batch)
        self.btn_add.pack(side="left")
        self.btn_del = ttk.Button(act, text="删除已选", command=self.on_delete)
        self.btn_del.pack(side="left", padx=6)
        self.btn_chk = ttk.Button(act, text="校验UID", command=self.on_check)
        self.btn_chk.pack(side="left")

        self.pbar = ttk.Progressbar(act, length=360, mode="determinate")
        self.pbar.pack(side="left", padx=14)
        self.prog_var = tk.StringVar(value="0 / 0")
        ttk.Label(act, textvariable=self.prog_var, width=16).pack(side="left")
        self.stat_var = tk.StringVar(value="")
        ttk.Label(act, textvariable=self.stat_var).pack(side="left", padx=10)

    def _build_log(self):
        bot = ttk.LabelFrame(self.root, text="日志", padding=4)
        bot.pack(fill="both", expand=True)
        self.log = tk.Text(bot, height=9, state="disabled", font=("Consolas", 9))
        sb = ttk.Scrollbar(bot, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True, padx=(2, 0))
        sb.pack(side="right", fill="y")

    def _log(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _status(self, text):
        self.stat_var.set(text)
        self.root.update_idletasks()

    def _poll_queue(self):
        try:
            while True:
                msg = self.q.get_nowait()
                self._handle_msg(msg)
        except queue.Empty:
            pass
        self.root.after(80, self._poll_queue)

    def _handle_msg(self, msg):
        kind = msg[0]
        if kind == "progress":
            _, done, total = msg
            self.pbar["maximum"] = max(1, total)
            self.pbar["value"] = done
            self.prog_var.set(f"{done} / {total}")
        elif kind == "done":
            _, op, results = msg
            self.busy = False
            self._set_buttons_enabled(True)
            self.results = results
            self._render_results(op)
            self._render_summary(op)
        elif kind == "log":
            self._log(msg[1])
        elif kind == "error":
            self.busy = False
            self._set_buttons_enabled(True)
            self._log("错误: " + msg[1])
            self._status(msg[1])

    def _set_buttons_enabled(self, on):
        state = "normal" if on else "disabled"
        for b in (self.btn_add, self.btn_del, self.btn_chk):
            b.configure(state=state)

    def _get_client(self):
        ip = self.ip_var.get().strip()
        port = self.port_var.get().strip()
        if not ip or not port:
            messagebox.showwarning(APP_TITLE, "请填写服务器 IP 和端口")
            return None
        try:
            win = max(1, min(int(self.win_var.get()), 256))
        except ValueError:
            win = 16
        return XnatClient(ip, port), win

    def _run_worker(self, fn):
        if self.busy:
            messagebox.showinfo(APP_TITLE, "正在执行中，请等待")
            return
        self.busy = True
        self._set_buttons_enabled(False)
        self.pbar["value"] = 0
        threading.Thread(target=fn, daemon=True).start()

    def _emit_progress(self, done, total):
        self.q.put(("progress", done, total))

    def on_test(self):
        client, _ = self._get_client()
        if client is None:
            return
        ok, desc = client.test_conn()
        self._log(f"[连接测试] server={client.addr[0]}:{client.addr[1]} -> {desc}")
        if ok:
            self._status("已连接")
            messagebox.showinfo(APP_TITLE, desc)
        else:
            self._status("连接失败")
            messagebox.showerror(APP_TITLE, desc)

    def on_load(self):
        path = filedialog.askopenfilename(
            title="导入 UID 文件",
            filetypes=[("Text Files (*.txt)", "*.txt"), ("All Files", "*.*")])
        if not path:
            return
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            raw = f.read()
        seen, added = set(self.uids), []
        for line in raw.splitlines():
            u = line.strip().split()[0] if line.strip() else ""
            if not u or u.startswith("#"):
                continue
            u = u[:32]
            if u not in seen:
                seen.add(u)
                added.append(u)
        self.uids.extend(added)
        self._render_local()
        self._log(f"[导入] {path}  新增 {len(added)} 条，当前共 {len(self.uids)} 条")
        self._status(f"已导入，共 {len(self.uids)} 条")

    def on_export(self):
        if not self.results:
            messagebox.showinfo(APP_TITLE, "结果列表为空")
            return
        from datetime import datetime
        default = "UID_%s.txt" % datetime.now().strftime("%Y%m%d_%H%M%S")
        path = filedialog.asksaveasfilename(
            title="导出结果",
            defaultextension=".txt",
            initialfile=default,
            filetypes=[("Text Files (*.txt)", "*.txt"), ("All Files", "*.*")])
        if not path:
            return
        lines = ["# UID\t状态"]
        for u, c in self.results:
            lines.append(f"{u}\t{status_text(c, 'check')}")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self._log(f"[导出] {path}  共 {len(lines) - 1} 条")

    def on_clear(self):
        self.uids.clear()
        self.results.clear()
        self._render_local()
        self.result_tree.delete(*self.result_tree.get_children())
        self.pbar["value"] = 0
        self.prog_var.set("0 / 0")
        self._status("已清空")

    def on_add_one(self):
        u = self.add_var.get().strip()[:32]
        if not u:
            return
        self.add_var.set("")
        if u not in self.uids:
            self.uids.append(u)
            self._render_local()
            self._status(f"已添加 {u}，共 {len(self.uids)} 条")

    def _render_local(self):
        self.local_tree.delete(*self.local_tree.get_children())
        for i, u in enumerate(self.uids, 1):
            self.local_tree.insert("", "end", values=(i, u))

    def _render_results(self, op):
        self.result_tree.delete(*self.result_tree.get_children())
        for u, c in self.results:
            self.result_tree.insert("", "end", values=(u, status_text(c, op)))

    def _render_summary(self, op):
        total = len(self.results)
        ok = sum(1 for _, c in self.results if c == ST_OK)
        norsp = sum(1 for _, c in self.results if c == ST_NORSP)
        other = total - ok - norsp
        self.pbar["maximum"] = max(1, total)
        self.pbar["value"] = total
        self.prog_var.set(f"{total} / {total}")
        if op == "check":
            self._status(f"校验完成: 总数={total} 已授权/在线={ok} 未授权={other} 无应答={norsp}")
        else:
            self._status(f"完成: 总数={total} 成功={ok} 失败={other} 无应答={norsp}")

    def _batch(self, op):
        client, win = self._get_client()
        if client is None:
            self.q.put(("done", op, []))
            return
        uids = list(self.uids)
        total = len(uids)
        self.q.put(("log",
                    f"[AddBatchUIDs] count={total}, sendsize={win}, server={client.addr[0]}:{client.addr[1]}"))

        def work():
            try:
                results = []
                if op == "check":
                    fn = client.check
                elif op == "del":
                    fn = client.delete
                else:
                    fn = client.add
                batch = 100
                for i in range(0, total, batch):
                    part = uids[i:i + batch]
                    results.extend(fn(part, send_size=win))
                    self.q.put(("progress", min(i + batch, total), total))
                self.q.put(("done", op, results))
            except Exception as e:
                self.q.put(("error", str(e)))

        self._run_worker(work)

    def on_add_batch(self):
        if not self.uids:
            messagebox.showinfo(APP_TITLE, "本地列表为空，请先导入 UID 文件")
            return
        self._batch("add")

    def on_delete(self):
        if not self.uids:
            return
        self._batch("del")

    def on_check(self):
        if not self.uids:
            messagebox.showinfo(APP_TITLE, "本地列表为空，请先导入 UID 文件")
            return
        self._batch("check")


def main():
    root = tk.Tk()
    app = App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
