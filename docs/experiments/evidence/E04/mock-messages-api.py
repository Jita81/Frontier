# Local stand-in for POST /v1/messages: records each request, replies with a
# thinking block then a text block (the reply text is argv[2]).
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
out, reply = sys.argv[1], sys.argv[2]
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['content-length']))
        req = json.loads(body)
        rec = {"path": self.path, "version": self.headers.get("anthropic-version"),
               "content_type": self.headers.get("content-type"),
               "has_key_header": bool(self.headers.get("x-api-key")), "body": req}
        open(out, "a").write(json.dumps(rec) + "\n")
        resp = {"id": "msg_mock", "type": "message", "role": "assistant", "model": req["model"],
                "content": [{"type": "thinking", "thinking": "x \"text\": decoy", "signature": "s"},
                            {"type": "text", "text": reply}],
                "stop_reason": "end_turn", "usage": {"input_tokens": 1, "output_tokens": 1}}
        data = json.dumps(resp, separators=(",", ":")).encode()
        self.send_response(200); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", int(sys.argv[3])), H).serve_forever()
