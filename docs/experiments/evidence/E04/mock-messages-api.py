# Local stand-in for POST /v1/messages: records each request, replies with a
# thinking block then a text block (the reply text is argv[2]).
# argv[4], optional: "refusal" (stop_reason refusal), "500" (HTTP 500), or
# "slow:<s>" (sleep <s> seconds before replying).
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
out, reply = sys.argv[1], sys.argv[2]
mode = sys.argv[4] if len(sys.argv) > 4 else "ok"
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['content-length']))
        req = json.loads(body)
        rec = {"path": self.path, "version": self.headers.get("anthropic-version"),
               "content_type": self.headers.get("content-type"),
               "has_key_header": bool(self.headers.get("x-api-key")), "body": req}
        open(out, "a").write(json.dumps(rec) + "\n")
        if mode.startswith("slow:"): time.sleep(float(mode[5:]))
        resp = {"id": "msg_mock", "type": "message", "role": "assistant", "model": req["model"],
                "content": [{"type": "thinking", "thinking": "x \"text\": decoy", "signature": "s"},
                            {"type": "text", "text": reply}],
                "stop_reason": "refusal" if mode == "refusal" else "end_turn",
                "usage": {"input_tokens": 1, "output_tokens": 1}}
        if mode == "500": resp = {"type": "error", "error": {"type": "api_error", "message": "mock"}}
        data = json.dumps(resp, separators=(",", ":")).encode()
        self.send_response(500 if mode == "500" else 200); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", int(sys.argv[3])), H).serve_forever()
