"""Exercise the production Windows 9P process manager against a loopback 9P server."""
import json
import pathlib
import subprocess
import sys
import tempfile
import time

from WinFspPlan9Integration import Server

def run(manager, helper):
    with tempfile.TemporaryDirectory(prefix="NanaBox9PManager-") as tmp:
        root = pathlib.Path(tmp).resolve()
        log = root / "manager.log"
        with log.open("wb") as err:
            proc = subprocess.Popen([manager, "--plan9-manager-test", helper, str(root)],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=err, text=True, encoding="utf-8")
            def send(action, success=True, **payload):
                proc.stdin.write(json.dumps({"RequestType": action, **payload}) + "\n")
                proc.stdin.flush()
                line = proc.stdout.readline()
                assert line, "Manager exited"
                response = json.loads(line)
                assert response["Success"] == success, response
                return response.get("Data", response)
            def update(server, automatic=True, readonly=False):
                return send("Update", Shares=[{"Name":"TestShare","Port":server.port,
                    "AutoMount":automatic,"ReadOnly":readonly}])
            def mount(target="", success=True):
                return send("Mount", success, Settings={"Name":"TestShare","Target":target})
            def unmount(success=True):
                return send("Unmount", success, Settings={"Name":"TestShare"})
            def ready():
                deadline=time.monotonic()+20
                while time.monotonic()<deadline:
                    row=send("Query")["Shares"][0]
                    if row["Mounted"]: return row
                    time.sleep(.1)
                raise AssertionError("Automatic mount not ready: "+str(row))
            def closed(server):
                server.thread.join(10)
                assert not server.thread.is_alive()
                assert server.error is None,server.error
                assert not server.fids,server.fids
            try:
                first=Server()
                update(first)
                row=ready()
                default=root/"TestShare"
                assert pathlib.Path(row["MountPoint"])==default,row
                assert (default/"hello.txt").read_bytes()==b"Hello 9P"
                with (default/"hello.txt").open("rb") as file:
                    unmount(False)
                    assert file.read()==b"Hello 9P"
                    assert send("Query")["Shares"][0]["Mounted"]
                row=unmount()["Shares"][0]
                assert not row["Mounted"] and row["Suppressed"]
                closed(first)
                assert not default.exists()
                row=update(first)["Shares"][0]
                assert row["Suppressed"] and not row["Mounted"]
                second=Server()
                row=update(second,False,True)["Shares"][0]
                assert not row["Mounted"] and not row["Starting"]
                mount(r"..\escape",False)
                mount(str(root.parent/"escape"),False)
                occupied=root/"occupied";occupied.mkdir()
                marker=occupied/"keep";marker.write_text("preserve")
                mount("occupied",False)
                assert marker.read_text()=="preserve"
                row=mount("Custom")["Shares"][0]
                custom=root/"Custom"
                assert row["Mounted"] and pathlib.Path(row["MountPoint"])==custom
                assert (custom/"hello.txt").read_bytes()==b"Hello 9P"
                try:
                    (custom/"hello.txt").write_bytes(b"changed")
                except OSError: pass
                else: raise AssertionError("Managed read-only mount accepted a write")
                row=update(second,False,True)["Shares"][0]
                assert row["Mounted"] and pathlib.Path(row["MountPoint"])==custom
                assert send("Update",Shares=[])["Shares"]==[]
                closed(second)
                assert not custom.exists()
                proc.stdin.write('{"RequestType":"Exit"}\n');proc.stdin.flush()
                assert proc.wait(timeout=15)==0
            finally:
                if proc.poll() is None:
                    proc.stdin.close()
                    try:proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait()
                err.flush()
                print(log.read_text(encoding="utf-8",errors="replace"))
    print("Windows 9P managed automatic and manual mount integration passed.")

if __name__=="__main__":
    run(*(str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:3]))
