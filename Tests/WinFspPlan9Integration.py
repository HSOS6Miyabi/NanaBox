"""Integration test: mount a disposable in-memory 9P2000.L server through WinFsp."""
import ctypes
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

P = lambda fmt, *values: struct.pack("<" + fmt, *values)
S = lambda text: P("H", len(text.encode("utf-8"))) + text.encode("utf-8")

class Message:
    def __init__(self, data):
        self.data = data
        self.pos = 0
    def get(self, fmt):
        size = struct.calcsize("<" + fmt)
        result = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += size
        return result[0] if len(result) == 1 else result
    def raw(self, count):
        result = self.data[self.pos:self.pos+count]
        assert len(result) == count
        self.pos += count
        return result
    def string(self):
        return self.raw(self.get("H")).decode("utf-8")
    def end(self):
        assert self.pos == len(self.data), (self.pos, len(self.data))

class Node:
    counter = 0
    def __init__(self, directory=False, contents=b""):
        Node.counter += 1
        self.id = Node.counter
        self.directory = directory
        self.children = {}
        self.data = bytearray(contents)
        self.mode = (0o040000 if directory else 0o100000) | 0o777
    def qid(self):
        return P("BIQ", 128 if self.directory else 0, 0, self.id)

class Server:
    def __init__(self):
        self.root = Node(True)
        self.root.children["hello.txt"] = Node(contents=b"Hello 9P")
        self.root.children["comma,semicolon;.txt"] = Node(contents=b"valid punctuation")
        self.root.children["Unicode-\u4e16\u754c.txt"] = Node(contents=b"UTF-8")
        self.fids = {}
        self.error = None
        self.calls = []
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.listener.settimeout(30)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
    def node(self, fid):
        if fid not in self.fids:
            raise OSError(9, "Unknown fid")
        return self.fids[fid]
    def call(self, typ, r):
        self.calls.append(typ)
        if typ == 100:
            assert r.get("I") >= 4096 and r.string() == "9P2000.L"
            return P("I", 8192) + S("9P2000.L")
        if typ == 104:
            fid, afid = r.get("II")
            assert afid == 0xffffffff and r.string() == "" and r.string() == "TestShare"
            assert r.get("I") == 0xffffffff
            self.fids[fid] = self.root
            return self.root.qid()
        if typ == 110:
            fid, new, count = r.get("IIH")
            node = self.node(fid)
            names = [r.string() for _ in range(count)]
            result = []
            for name in names:
                if not node.directory or name not in node.children:
                    if not result:
                        raise OSError(2, "Missing path")
                    break
                node = node.children[name]
                result.append(node.qid())
            self.fids[new] = node
            return P("H", len(result)) + b"".join(result)
        if typ == 24:
            node = self.node(r.get("I")); r.get("Q")
            size = 0 if node.directory else len(node.data)
            return P("Q", 0x3fff) + node.qid() + P("III", node.mode, 0, 0) + P("QQQQQ", 1, 0, size, 4096, (size+511)//512) + P("QQQQQQQQQQ", 1700000000,0,1700000000,0,1700000000,0,1700000000,0,0,0)
        if typ == 8:
            self.node(r.get("I"))
            return P("IIQQQQQQI", 0,4096,100000,80000,80000,10000,9900,1,255)
        if typ == 12:
            node=self.node(r.get("I"));flags=r.get("I")
            if flags & 0x200:node.data.clear()
            return node.qid()+P("I",4096)
        if typ == 14:
            fid=r.get("I");name=r.string();flags,mode,gid=r.get("III")
            parent=self.node(fid)
            if name in parent.children:raise OSError(17,"Exists")
            node=Node();node.mode=0o100000|mode
            parent.children[name]=node;self.fids[fid]=node
            return node.qid()+P("I",4096)
        if typ in (116,118):
            fid,offset,count=r.get("IQI");node=self.node(fid)
            if typ==116:
                data=node.data[offset:offset+count]
                return P("I",len(data))+data
            data=r.raw(count);end=offset+count
            if end>len(node.data):node.data.extend(b"\0"*(end-len(node.data)))
            node.data[offset:end]=data
            return P("I",count)
        if typ==40:
            fid,offset,count=r.get("IQI");node=self.node(fid)
            names=list(node.children.items())
            result=bytearray()
            for index,(name,child) in enumerate(names[offset:],offset+1):
                entry=child.qid()+P("QB",index,4 if child.directory else 8)+S(name)
                if len(result)+len(entry)>count:break
                result+=entry
            return P("I",len(result))+result
        if typ==26:
            node=self.node(r.get("I"));valid,mode,uid,gid=r.get("IIII")
            size,at,an,mt,mn=r.get("QQQQQ")
            if valid&1:node.mode=(node.mode&0o170000)|(mode&0o777)
            if valid&8:
                if size>len(node.data):node.data.extend(b"\0"*(size-len(node.data)))
                else:del node.data[size:]
            return b""
        if typ==50:
            self.node(r.get("I"));r.get("I");return b""
        if typ==72:
            parent=self.node(r.get("I"));name=r.string();mode,gid=r.get("II")
            if name in parent.children:raise OSError(17,"Exists")
            child=Node(True);child.mode=0o040000|mode;parent.children[name]=child
            return child.qid()
        if typ==74:
            parent=self.node(r.get("I"));old=r.string();dest=self.node(r.get("I"));new=r.string()
            if old not in parent.children:raise OSError(2,"Missing")
            dest.children[new]=parent.children.pop(old);return b""
        if typ==122:
            fid=r.get("I");node=self.node(fid);del self.fids[fid]
            def remove(parent):
                for name,child in list(parent.children.items()):
                    if child is node:
                        if child.directory and child.children:raise OSError(39,"Not empty")
                        del parent.children[name];return True
                    if child.directory and remove(child):return True
                return False
            if not remove(self.root):raise OSError(2,"Missing")
            return b""
        raise AssertionError("Unexpected 9P operation "+str(typ))
    @staticmethod
    def receive(connection,count):
        result=bytearray()
        while len(result)<count:
            part=connection.recv(count-len(result))
            if not part:
                if not result:return None
                raise RuntimeError("Truncated request")
            result+=part
        return bytes(result)
    def serve(self):
        try:
            with self.listener.accept()[0] as connection:
                connection.settimeout(30)
                while True:
                    header=self.receive(connection,4)
                    if header is None:break
                    size=struct.unpack("<I",header)[0]
                    assert 7<=size<=1024*1024
                    r=Message(self.receive(connection,size-4))
                    typ,tag=r.get("BH")
                    try:
                        if typ==120:
                            fid=r.get("I");self.node(fid);del self.fids[fid];body=b""
                        else:body=self.call(typ,r)
                        r.end();reply=typ+1
                    except OSError as error:
                        body=P("I",error.errno);reply=7
                    # Split the header and body to exercise partial socket reads.
                    connection.sendall(P("I",7+len(body)))
                    connection.sendall(P("BH",reply,tag)+body)
        except Exception as error:
            self.error=error
        finally:self.listener.close()

kernel=ctypes.WinDLL("kernel32",use_last_error=True)
kernel.CreateEventW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_wchar_p]
kernel.CreateEventW.restype=ctypes.c_void_p
kernel.SetEvent.argtypes=[ctypes.c_void_p]
kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32]
kernel.WaitForSingleObject.restype=ctypes.c_uint32
kernel.CloseHandle.argtypes=[ctypes.c_void_p]

def run(exe, readonly):
    server=Server()
    event_name="Local\\NanaBox.Plan9.Test."+uuid.uuid4().hex
    event=kernel.CreateEventW(None,0,0,event_name)
    busy=kernel.CreateEventW(None,0,0,event_name+".Busy")
    assert event and busy
    with tempfile.TemporaryDirectory(prefix="NanaBox9P-") as temp:
        mount=pathlib.Path(temp)/"mount"
        log=pathlib.Path(temp)/"client.log"
        command=[exe,"--test-loopback",str(server.port),"TestShare",str(mount),"--stop-event",event_name,"--busy-event",event_name+".Busy"]
        if readonly:command.append("--read-only")
        with log.open("wb") as output:
            process=subprocess.Popen(command,stdout=output,stderr=subprocess.STDOUT)
            try:
                deadline=time.monotonic()+25
                while not (mount/"hello.txt").exists():
                    if process.poll() is not None or time.monotonic()>deadline:
                        raise RuntimeError("9P mount did not become ready")
                    time.sleep(.1)
                assert (mount/"hello.txt").read_bytes()==b"Hello 9P"
                assert (mount/"Unicode-\u4e16\u754c.txt").read_bytes()==b"UTF-8"
                assert "hello.txt" in os.listdir(mount)
                assert (mount/"comma,semicolon;.txt").read_bytes()==b"valid punctuation"
                if readonly:
                    try:
                        (mount/"hello.txt").write_bytes(b"changed")
                    except OSError:pass
                    else:raise AssertionError("Read-only mount accepted write")
                    assert (mount/"hello.txt").read_bytes()==b"Hello 9P"
                else:
                    data=bytes(range(251))*1000
                    child=mount/"folder";child.mkdir()
                    file=child/"data.bin"
                    with file.open("w+b") as stream:
                        stream.write(data);stream.flush();os.fsync(stream.fileno())
                        stream.seek(17003);assert stream.read(119)==data[17003:17122]
                        stream.seek(80003);stream.write(b"XYZ");stream.flush()
                        stream.truncate(120003)
                    expected=data[:80003]+b"XYZ"+data[80006:120003]
                    assert file.read_bytes()==expected
                    renamed=mount/"renamed.bin";file.rename(renamed)
                    assert renamed.read_bytes()==expected
                    renamed.unlink();child.rmdir()
                    assert not renamed.exists() and not child.exists()
                with (mount/"hello.txt").open("rb") as held:
                    assert kernel.SetEvent(event)
                    assert kernel.WaitForSingleObject(busy,5000)==0, "Busy mount was not protected"
                    assert process.poll() is None and held.read()==b"Hello 9P"
                assert kernel.SetEvent(event)
                assert process.wait(timeout=20)==0
                server.thread.join(10)
                assert not server.thread.is_alive()
                assert server.error is None,server.error
                assert not server.fids,server.fids
                assert not mount.exists()
                if readonly:assert not any(t in server.calls for t in (14,118,122,26,72,74))
                else:assert all(t in server.calls for t in (14,118,116,122,26,72,74,50))
            finally:
                kernel.SetEvent(event)
                if process.poll() is None:
                    try:process.wait(timeout=10)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
                kernel.CloseHandle(event)
                kernel.CloseHandle(busy)
                output.flush()
                print(log.read_text(encoding="utf-8",errors="replace"))
    print("WinFsp 9P mount passed:", "read-only" if readonly else "read-write")

if __name__=="__main__":
    binary=str(pathlib.Path(sys.argv[1]).resolve())
    run(binary,False)
    run(binary,True)
