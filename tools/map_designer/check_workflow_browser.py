"""Optional real-browser workflow regression; all saved files go to a temp directory."""
from pathlib import Path
import json
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.request import urlopen
from unittest.mock import patch

import serve


class WorkflowBrowserTest(unittest.TestCase):
    def test_two_modes_and_numbered_layout(self):
        browser = next((p for p in (
            Path("C:/Program Files/Google/Chrome/Application/chrome.exe"),
            Path("C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe")) if p.is_file()), None)
        try:
            import websocket
        except ImportError:
            self.skipTest("websocket-client required for browser regression")
        if not browser:
            self.skipTest("Chrome/Edge required for browser regression")
        with tempfile.TemporaryDirectory() as folder, patch.object(serve, "OUT", Path(folder) / "output"):
            root = Path(folder)
            (serve.OUT / "layouts").mkdir(parents=True)
            start_layout = Path(__file__).parent / "output" / "layouts" / "1x1_000.json"
            (serve.OUT / "layouts" / "1x1_000.json").write_text(
                start_layout.read_text(encoding="utf-8"), encoding="utf-8")
            server = serve.ThreadingHTTPServer(("127.0.0.1", 0), serve.Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            profile = root / "profile"
            process = subprocess.Popen([str(browser), "--headless=new", "--disable-gpu",
                "--no-first-run", "--no-default-browser-check", "--disable-background-networking",
                "--remote-debugging-port=0", "--user-data-dir=" + str(profile), "about:blank"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                active = profile / "DevToolsActivePort"
                for _ in range(100):
                    if active.exists():
                        break
                    time.sleep(0.05)
                self.assertTrue(active.exists(), "Headless browser did not start")
                port = active.read_text().splitlines()[0]
                with urlopen("http://127.0.0.1:" + port + "/json/list") as response:
                    tab = next(t for t in json.load(response) if t["type"] == "page")
                ws = websocket.create_connection(tab["webSocketDebuggerUrl"], suppress_origin=True, timeout=5)
                seq, errors = 0, []
                def send(method, params=None):
                    nonlocal seq
                    seq += 1
                    ws.send(json.dumps({"id": seq, "method": method, "params": params or {}}))
                    while True:
                        result = json.loads(ws.recv())
                        if result.get("method") == "Runtime.exceptionThrown":
                            errors.append(result["params"])
                        if result.get("id") == seq:
                            self.assertNotIn("error", result)
                            return result.get("result", {})
                def evaluate(expression):
                    result = send("Runtime.evaluate", {"expression": expression, "returnByValue": True})
                    self.assertNotIn("exceptionDetails", result)
                    return result["result"].get("value")
                def until(expression):
                    for _ in range(150):
                        if evaluate(expression):
                            return
                        time.sleep(0.04)
                    self.fail("Browser timeout: " + expression + " / " + str(evaluate('document.getElementById("status").textContent')))
                def click(element):
                    evaluate('document.getElementById(%s).click()' % json.dumps(element))
                def choose(element, value):
                    evaluate('(()=>{const e=document.getElementById(%s);e.value=%s;e.dispatchEvent(new Event("change"));})()' %
                             (json.dumps(element), json.dumps(value)))
                try:
                    send("Runtime.enable")
                    send("Emulation.setDeviceMetricsOverride", {"width":1600, "height":1000, "deviceScaleFactor":1, "mobile":False})
                    send("Page.navigate", {"url":"http://127.0.0.1:%d/tools/map_designer/index.html" % server.server_port})
                    until('document.getElementById("mapText")?.value.includes("map_designer_layout")')
                    self.assertEqual(evaluate('document.getElementById("editMode").value'), "layout")
                    self.assertEqual(evaluate('document.getElementById("poolShape").options.length'), 3)
                    self.assertTrue(evaluate('document.getElementById("palette").parentElement.hidden'))
                    choose("layoutList", "0"); click("btnLoadLayout")
                    until('document.getElementById("status").textContent.includes("已读取 1x1_000.json")')
                    self.assertEqual(evaluate('JSON.parse(document.getElementById("mapText").value).layout_id'), 0)
                    self.assertEqual(evaluate('JSON.parse(document.getElementById("mapText").value).objects.length'), 0)
                    choose("poolShape", "L")
                    until('document.getElementById("status").textContent.includes("已新建 L")')
                    until('!!document.querySelector("[data-object=stone]")')
                    evaluate('document.querySelector("[data-object=stone]").click()')
                    point = evaluate('(()=>{const c=document.getElementById("map"),r=c.getBoundingClientRect();return {x:r.left+25.5*32*r.width/c.width,y:r.top+9.5*32*r.height/c.height};})()')
                    send("Input.dispatchMouseEvent", dict(type="mousePressed", button="left", clickCount=1, **point))
                    send("Input.dispatchMouseEvent", dict(type="mouseReleased", button="left", clickCount=1, **point))
                    until('JSON.parse(document.getElementById("mapText").value).objects.length === 1')
                    click("btnSaveLayout")
                    until('document.getElementById("status").textContent.includes("已保存 output/layouts/L_001.json")')
                    click("btnNewLayout")
                    until('document.getElementById("layoutNumber").value === "2"')
                    self.assertEqual(evaluate('JSON.parse(document.getElementById("mapText").value).objects.length'), 0)
                    choose("layoutList", "1"); click("btnLoadLayout")
                    until('document.getElementById("status").textContent.includes("已读取 L_001.json")')
                    self.assertEqual(evaluate('JSON.parse(document.getElementById("mapText").value).objects[0].kind'), "stone")
                    choose("editMode", "map")
                    until('document.getElementById("status").textContent.includes("地图模式：L")')
                    self.assertTrue(evaluate('document.getElementById("objectPalette").parentElement.hidden'))
                    self.assertEqual(evaluate('JSON.parse(document.getElementById("mapText").value).objects.length'), 0)
                    click("btnExportMap")
                    until('document.getElementById("status").textContent.includes("已导出 output/maps/L.json")')
                    self.assertEqual(errors, [])
                finally:
                    try:
                        ws.send(json.dumps({"id":99999,"method":"Browser.close"}))
                    except (OSError, websocket.WebSocketException):
                        pass
                    finally:
                        ws.close()
                self.assertTrue((serve.OUT / "layouts/L_001.json").is_file())
                self.assertTrue((serve.OUT / "maps/L.json").is_file())
            finally:
                # Close only this test-owned browser; never touch the user's sessions.
                if process.poll() is None:
                    try:
                        process.wait(timeout=5)
                    except Exception:
                        process.terminate()
                        process.wait(timeout=5)
                server.shutdown(); server.server_close(); thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
