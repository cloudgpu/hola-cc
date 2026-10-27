#!/usr/bin/env python3
"""A fake OpenAI-compatible chat server for holacc end-to-end tests.

It replies from a script: a JSON list of steps, consumed in order, one per
request. A step is {"tool": name, "args": {...}} (one tool call) or
{"text": "..."} (a final answer). Every request body is appended to the log
file as one JSON line, so the test can check what the agent sent.

usage: fake_llm.py PORT SCRIPT.json LOG.jsonl
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

port, script_path, log_path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
with open(script_path) as f:
    steps = json.load(f)
state = {"i": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"data":[{"id":"fake"}]}')

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        req = json.loads(body or b"{}")
        if not self.path.endswith("/chat/completions"):
            # capability probes (e.g. /api/show) do not consume a step
            data = b"{}"
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        req["_auth"] = self.headers.get("Authorization")
        with open(log_path, "a") as f:
            f.write(json.dumps(req) + "\n")
        i = state["i"]
        state["i"] += 1
        step = steps[i] if i < len(steps) else {"text": "done"}
        if "tool" in step:
            msg = {"role": "assistant", "content": None, "tool_calls": [{
                "id": "call_%d" % i, "type": "function",
                "function": {"name": step["tool"], "arguments": json.dumps(step.get("args", {}))}}]}
            finish = "tool_calls"
        else:
            msg = {"role": "assistant", "content": step["text"]}
            finish = "stop"
        if req.get("stream"):
            self.stream_reply(i, req, msg, finish)
            return
        out = {"id": "fake-%d" % i, "object": "chat.completion", "model": req.get("model", "fake"),
               "choices": [{"index": 0, "message": msg, "finish_reason": finish}],
               "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}
        data = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


    def stream_reply(self, i, req, msg, finish):
        """The same reply as server-sent events, the way streaming providers send it."""
        base = {"id": "fake-%d" % i, "object": "chat.completion.chunk", "model": req.get("model", "fake")}
        delta = {"role": "assistant"}
        if msg.get("tool_calls"):
            tc = msg["tool_calls"][0]
            delta["tool_calls"] = [{"index": 0, "id": tc["id"], "type": "function",
                                    "function": {"name": tc["function"]["name"],
                                                 "arguments": tc["function"]["arguments"]}}]
        else:
            delta["content"] = msg["content"]
        chunks = [
            dict(base, choices=[{"index": 0, "delta": delta, "finish_reason": None}]),
            dict(base, choices=[{"index": 0, "delta": {}, "finish_reason": finish}]),
            dict(base, choices=[], usage={"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}),
        ]
        body = "".join("data: %s\n\n" % json.dumps(c) for c in chunks) + "data: [DONE]\n\n"
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


HTTPServer(("127.0.0.1", port), Handler).serve_forever()
