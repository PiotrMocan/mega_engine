"""Capture a settled authored scene through the native renderer's MCP endpoint."""
import argparse
import base64
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time


def main():
    if os.name != "nt":
        raise RuntimeError("This verifier requires native Windows Python")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--asset-root", type=Path)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--aa", default="off")
    parser.add_argument("--samples", type=int, choices=range(1, 9), default=1)
    parser.add_argument("extra", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if not any(output.is_relative_to(root / "build" / tree) for tree in
               ("windows-release", "windows-debug", "windows-asan")):
        raise ValueError("Capture output must stay inside an allowed Windows build tree")
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, VCR_ASSET_ROOT=str((args.asset_root or root / "assets").resolve()))
    if args.validation:
        env["VK_VALIDATION_VALIDATE_SYNC"] = "true"
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    command = [str(args.exe.resolve()), "--mcp-stdio", "--benchmark", "--warmup", "300",
               "--duration", "1", "--present", "immediate", "--aa", args.aa,
               "--width", str(args.width), "--height", str(args.height),
               "--validation" if args.validation else "--no-validation", *extra]
    with (output / "renderer.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=output, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8")
        responses = queue.Queue()

        def read_output():
            try:
                for line in process.stdout:
                    responses.put(line)
            finally:
                responses.put(None)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        request_id = 0

        def request(method, params):
            nonlocal request_id
            request_id += 1
            process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": request_id,
                                           "method": method, "params": params}) + "\n")
            process.stdin.flush()
            line = responses.get(timeout=240)
            if line is None:
                raise RuntimeError("Renderer closed MCP stdout; inspect renderer.log")
            response = json.loads(line)
            if response.get("id") != request_id or "error" in response:
                raise RuntimeError(f"Invalid response: {response}")
            result = response["result"]
            if result.get("isError"):
                raise RuntimeError(f"Tool failed: {result}")
            return result

        def tool(name, arguments=None):
            return request("tools/call", {"name": name, "arguments": arguments or {}})

        try:
            request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                   "clientInfo": {"name": "vegetation-verifier", "version": "1"}})
            process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
            process.stdin.flush()
            # The renderer runs uncapped; this wait only lets GPU histories settle.
            time.sleep(2)
            for sample in range(args.samples):
                if sample:
                    time.sleep(1)
                suffix = f"-{sample + 1}" if sample else ""
                result = tool("capture.screenshot")
                image = next(item for item in result["content"] if item["type"] == "image")
                (output / f"scene{suffix}.png").write_bytes(base64.b64decode(image["data"], validate=True))
                state = tool("debug.get_state")
                (output / f"state{suffix}.json").write_text(json.dumps(state, indent=2), encoding="utf-8")
            tool("engine.quit")
            process.wait(timeout=30)
            if process.returncode:
                raise RuntimeError(f"Renderer exit code {process.returncode}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
            reader.join(timeout=5)
            process.stdin.close()
            process.stdout.close()
    text = (output / "renderer.log").read_text(encoding="utf-8", errors="replace")
    if "Validation Error" in text or "ERROR: AddressSanitizer" in text:
        raise RuntimeError("GPU validation or AddressSanitizer error; inspect renderer.log")
    print(output / "scene.png")


if __name__ == "__main__":
    main()
