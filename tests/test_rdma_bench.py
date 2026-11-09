import contextlib
from dataclasses import replace
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import rdma_bench as rb


BW_HEADER = " #bytes #iterations BW peak[Gb/sec] BW average[Gb/sec] MsgRate[Mpps]\n"
LAT_HEADER = (" #bytes #iterations t_min[usec] t_max[usec] t_typical[usec] "
              "t_avg[usec] t_stdev[usec] 99% percentile[usec] 99.9% percentile[usec]\n")


def endpoint(ip="127.0.0.1", tools=None):
    return rb.Endpoint("test-" + ip, "fake0", ip, "fake_rdma", 1, 3,
                       "::ffff:" + ip, {"script_sha256": "same"}, tools or {})


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class MeasurementTests(unittest.TestCase):
    def test_default_matrix(self):
        config = rb.Config()
        self.assertEqual(config.sizes, (64, 16384, 1048576, 4194304, 16777216, 67108864, 268435456, 1073741824))
        self.assertEqual(len(config.sizes) * config.repetitions * 4, 96)

    def test_native_bw_and_bidi_sections(self):
        job = rb.Job("test", rb.MODES[1], 64, "measured", "both")
        text = BW_HEADER + "64 1200 0.00 350.00 1.00\n"
        text += "Local results:\n" + BW_HEADER + "64 600 0.00 180.00 0.55\n"
        text += "Remote results:\n" + BW_HEADER + "64 600 0.00 170.00 0.45\n"
        parsed = rb.parse_output(text, job)
        self.assertEqual(parsed["aggregate"]["average_gbps"], 350)
        row = rb.measured_row(job, {"parsed": parsed}, {})
        self.assertEqual(row["client_to_server_gbps"], 180)
        self.assertEqual(row["server_to_client_gbps"], 170)
        with self.assertRaisesRegex(rb.BenchError, "Missing/unsupported"):
            rb.parse_output(BW_HEADER + "64 1200 0.00 350.00 1.00\n", job)

    def test_native_latency_stats_and_calibration(self):
        job = rb.Job("test", rb.MODES[2], 2**30, "calibration", "pingpong", iterations=100)
        text = LAT_HEADER + f"{2**30} 100 45000 60000 49000 50000 100 57000 59000\n"
        parsed = rb.parse_output(text, job)
        self.assertEqual(parsed["aggregate"]["p999_us"], 59000)
        self.assertEqual(rb.calibrated_iterations(parsed), 150)
        self.assertEqual(rb.calibrated_iterations({"aggregate": {"average_us": 100000}}), 100)

    def test_reject_substituted_size_iterations_units_and_nonfinite(self):
        job = rb.Job("test", rb.MODES[0], 64, "smoke", iterations=100)
        cases = [BW_HEADER + "32 100 0 100 1\n", BW_HEADER + "64 10 0 100 1\n",
                 BW_HEADER.replace("Gb/sec", "MB/sec") + "64 100 0 100 1\n",
                 BW_HEADER + "64 100 0 nan 1\n", ""]
        for text in cases:
            with self.subTest(text=text), self.assertRaises(rb.BenchError):
                rb.parse_output(text, job)

    def test_command_role_reversal_uses_native_roles(self):
        e = endpoint(tools={"ib_write_bw": {"path": "/bin/ib_write_bw"}})
        config = rb.Config()
        job = rb.Job("test", rb.MODES[0], 2**30, "measured", "server_to_client")
        client = rb.build_command(e, "127.0.0.2", "client", config, job)
        server = rb.build_command(e, "127.0.0.2", "server", config, job)
        self.assertNotIn("127.0.0.2", client)
        self.assertEqual(server[-1], "127.0.0.2")
        self.assertIn("--bind_source_ip=127.0.0.1", client)
        self.assertEqual(client[client.index("-s") + 1], str(2**30))
        self.assertEqual(client[client.index("-D") + 1], "20")
        self.assertEqual(client[client.index("-f") + 1], "5")

    def test_counter_alerts_ignore_normal_traffic(self):
        before = {"values": {"net/tx_bytes": 0, "net/rx_errors": 0, "rdma/retry": 2}, "unavailable": []}
        after = {"values": {"net/tx_bytes": 1000, "net/rx_errors": 3, "rdma/retry": 1}, "unavailable": []}
        result = rb.counter_changes(before, after)
        self.assertEqual(len(result["alerts"]), 2)
        self.assertEqual(result["deltas"]["net/tx_bytes"], 1000)

    def test_cli_requires_explicit_selection_and_valid_configuration(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            rb.main(["--client", "127.0.0.2"])
        for config in (replace(rb.Config(), duration=1), replace(rb.Config(), sizes=(64, 64)),
                       replace(rb.Config(), test_timeout=float("nan"))):
            with self.assertRaises(rb.BenchError):
                config.validate()


class DiscoveryTests(unittest.TestCase):
    def test_match_interface_ip_and_rocev2_and_reject_ambiguity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def write(path, value):
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(value)
            write("class/net/fake0/operstate", "up")
            port = "class/infiniband/mlx5_0/ports/1/"
            write(port + "state", "4: ACTIVE")
            write(port + "link_layer", "Ethernet")
            for i, kind in enumerate(("IB/RoCE v1", "RoCE v2")):
                write(port + f"gids/{i}", "::ffff:169.254.1.1")
                write(port + f"gid_attrs/types/{i}", kind)
                write(port + f"gid_attrs/ndevs/{i}", "fake0")
            args = rb.parser().parse_args(["--server", "--interface", "fake0"])
            ip_output = '[{"addr_info":[{"family":"inet","local":"169.254.1.1"}]}]'
            def command(argv):
                return ip_output if argv[0] == "ip" else "active_mtu: 1024 (3)"
            with patch.object(rb, "SYSFS", root), patch.object(rb, "run_text", side_effect=command):
                e = rb.discover_endpoint(args)
                self.assertEqual(e.gid_index, 1)
                self.assertEqual(e.details["rdma_active_mtu"], 1024)
                write(port + "gid_attrs/types/0", "RoCE v2")
                with self.assertRaisesRegex(rb.BenchError, "Expected one IPv4 RoCEv2 GID"):
                    rb.discover_endpoint(args)
                args.gid_index = 1
                self.assertEqual(rb.discover_endpoint(args).gid_index, 1)

    def test_route_and_versions(self):
        with patch.object(rb, "run_text", return_value='[{"dev":"wrong0"}]'):
            with self.assertRaisesRegex(rb.BenchError, "does not use"):
                rb.verify_route(endpoint(), "127.0.0.2")
        local = endpoint(tools={"ib_write_bw": {"version": "6.20"}})
        peer = endpoint(tools={"ib_write_bw": {"version": "other"}})
        with self.assertRaisesRegex(rb.BenchError, "version mismatch"):
            rb.verify_peer(local, peer)


class ChannelTests(unittest.TestCase):
    def setUp(self):
        a, b = socket.socketpair()
        self.a, self.b = rb.Channel(a), rb.Channel(b)

    def tearDown(self):
        self.a.close()
        self.b.close()

    def test_fragmented_and_buffered_messages(self):
        self.a.sock.sendall(b'{"type": "rea')
        self.assertIsNone(self.b.poll())
        self.a.sock.sendall(b'dy"}\n{"type":"go"}\n')
        self.assertEqual(self.b.receive("ready", 1)["type"], "ready")
        self.assertEqual(self.b.receive("go", 1)["type"], "go")

    def test_failure_timeout_and_disconnect(self):
        with self.assertRaisesRegex(rb.BenchError, "Timed out"):
            self.b.receive("ready", 0.02)
        self.a.send("error", message="native test failed")
        with self.assertRaisesRegex(rb.BenchError, "native test failed"):
            self.b.poll()
        self.a.close()
        with self.assertRaisesRegex(rb.BenchError, "disconnected"):
            self.b.poll()


class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in ("ib_write_bw", "ib_write_lat"):
            target = self.bin / tool
            target.write_text(Path(__file__).with_name("fake_perftest.py").read_text())
            target.chmod(0o755)
        self.environment = patch.dict(os.environ, {"PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                                                 "FAKE_EVENTS": str(self.root / "events.jsonl")})
        self.environment.start()
        self.tool_directory = patch.object(rb, "PERFTEST_BIN", self.bin)
        self.tool_directory.start()

    def tearDown(self):
        self.tool_directory.stop()
        self.environment.stop()
        self.tmp.cleanup()

    @patch.object(rb, "log")
    def run_sessions(self, config, _log):
        client, server = endpoint(), endpoint("127.0.0.2")
        rb.probe_tools(client, config)
        rb.probe_tools(server, config)
        a, b = socket.socketpair()
        ca, cb = rb.Channel(a), rb.Channel(b)
        cs, ss = rb.Store(self.root, rb.new_run_id(), "client"), rb.Store(self.root, rb.new_run_id(), "server")
        cs.setup(config, client, server)
        ss.setup(config, client, server)
        runner = rb.Session(ca, client, server, config, cs)
        worker = rb.Session(cb, server, client, config, ss)
        errors = []
        def serve():
            try:
                worker.serve()
                ss.finish("complete")
            except Exception as exc:
                errors.append(exc)
                try:
                    cb.send("error", message=str(exc))
                except rb.BenchError:
                    pass
                ss.finish("failed", str(exc))
            finally:
                worker.cleanup()
                cb.close()
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            runner.suite()
            cs.finish("complete")
        except Exception as exc:
            errors.append(exc)
            try:
                ca.send("error", message=str(exc))
            except rb.BenchError:
                pass
            cs.finish("failed", str(exc))
        finally:
            runner.cleanup()
            ca.close()
            thread.join(timeout=5)
        self.assertFalse(thread.is_alive(), "server did not exit")
        self.assert_no_native_processes()
        return cs, ss, errors

    def assert_no_native_processes(self):
        events = self.root / "events.jsonl"
        if events.exists():
            for line in events.read_text().splitlines():
                pid = json.loads(line)["pid"]
                with self.assertRaises(ProcessLookupError, msg=f"native process {pid} leaked"):
                    os.kill(pid, 0)

    def test_real_socket_coordination_all_modes_and_reports(self):
        config = rb.Config(perftest_port=free_port())
        client, server, errors = self.run_sessions(config)
        self.assertEqual(errors, [])
        self.assertEqual(len(client.rows), 96)
        runs = [json.loads(l) for l in (client.path / "runs.jsonl").read_text().splitlines()]
        latency = [r for r in runs if r["job"]["mode"] == rb.MODES[2]]
        calibrated = [r for r in latency if r["job"]["size"] == 2**20 and r["job"]["phase"] == "measured"]
        self.assertEqual(calibrated[0]["job"]["iterations"], 150)
        warmup = [r for r in latency if r["job"]["size"] == 2**20 and r["job"]["phase"] == "warmup"]
        self.assertEqual(warmup[0]["job"]["iterations"], 15)
        bidi = [r for r in client.rows if r["mode"] == rb.MODES[1]]
        self.assertEqual(bidi[0]["client_to_server_gbps"], 180)
        self.assertEqual(bidi[0]["server_to_client_gbps"], 170)
        reverse = [r for r in runs if r["job"]["direction"] == "server_to_client"]
        self.assertEqual(reverse[0]["server"]["argv"][-1], "127.0.0.1")
        self.assertIn("summary.md", [p.name for p in client.path.iterdir()])
        self.assertEqual(len((client.path / "results.csv").read_text().splitlines()), 97)
        self.assertEqual(json.loads((server.path / "status.json").read_text())["status"], "complete")

    def test_native_failure_preserves_earlier_measurements(self):
        config = rb.Config(modes=(rb.MODES[0],), sizes=(64, 16384), repetitions=1,
                           duration=5, perftest_port=free_port())
        with patch.dict(os.environ, {"FAKE_FAIL_MEASURED_SIZE": "16384"}):
            client, server, errors = self.run_sessions(config)
        self.assertTrue(errors)
        self.assertEqual(len(client.rows), 2)
        self.assertEqual(json.loads((client.path / "status.json").read_text())["status"], "failed")
        self.assertIn("injected native failure", " ".join(map(str, errors)))
        self.assertTrue((client.path / "summary.md").exists())

    def test_native_bw_warmup_and_duration_margins(self):
        config = rb.Config(sizes=(64, 2**20), repetitions=1, duration=5, perftest_port=free_port())
        with patch.dict(os.environ, {"FAKE_REQUIRE_WARMUP": "1"}):
            client, server, errors = self.run_sessions(config)
        self.assertEqual(errors, [])
        self.assertEqual(len(client.rows), 8)
        runs = [json.loads(line) for line in (client.path / "runs.jsonl").read_text().splitlines()]
        for run in runs:
            if run["job"]["mode"] == rb.MODES[2]:
                continue
            for role in ("client", "server"):
                argv = run[role]["argv"]
                self.assertIn("--perform_warm_up", argv)
                if run["job"]["phase"] == "measured":
                    self.assertEqual(argv[argv.index("-D") + 1], "5")
                    self.assertEqual(argv[argv.index("-f") + 1], "1")
                else:
                    self.assertNotIn("-f", argv)
        self.assertIn("1s start margin + 3s measured + 1s end margin", (client.path / "summary.md").read_text())
        self.assertEqual(json.loads((server.path / "status.json").read_text())["status"], "complete")

    def test_reject_perftest_without_native_warmup_fix(self):
        with patch.dict(os.environ, {"FAKE_PERFTEST_VERSION": "6.20"}):
            with self.assertRaisesRegex(rb.BenchError, "build_perftest.sh"):
                rb.probe_tools(endpoint(), rb.Config())

    def test_process_execution_timeout_and_cleanup(self):
        config = rb.Config(modes=(rb.MODES[0],), sizes=(64,), perftest_port=free_port())
        e = endpoint()
        rb.probe_tools(e, config)
        store = rb.Store(self.root, rb.new_run_id(), "client")
        job = rb.Job("timeout", rb.MODES[0], 64, "smoke", iterations=100)
        # A listener that never gets a connector stays alive until timeout.
        native = rb.NativeProcess(rb.build_command(e, "127.0.0.2", "server", config, job), store.native_dir, job)
        a, b = socket.socketpair()
        ca, cb = rb.Channel(a), rb.Channel(b)
        try:
            native.wait_ready(ca, config)
            with self.assertRaisesRegex(rb.BenchError, "execution timeout"):
                rb.wait_native(native, ca, 0.05)
        finally:
            native.stop()
            ca.close()
            cb.close()
        self.assert_no_native_processes()

    def test_listener_readiness_does_not_consume_connection(self):
        config = rb.Config(modes=(rb.MODES[0],), sizes=(64,), repetitions=1, duration=5, perftest_port=free_port())
        client, _, errors = self.run_sessions(config)
        self.assertEqual(errors, [])
        self.assertEqual(len(client.rows), 2)

    def test_startup_timeout_even_when_waiting_banner_is_printed(self):
        config = rb.Config(modes=(rb.MODES[0],), sizes=(64,), perftest_port=free_port(), startup_timeout=0.15)
        e = endpoint()
        rb.probe_tools(e, config)
        store = rb.Store(self.root, rb.new_run_id(), "client")
        job = rb.Job("startup-timeout", rb.MODES[0], 64, "smoke", iterations=100)
        a, b = socket.socketpair()
        ca, cb = rb.Channel(a), rb.Channel(b)
        with patch.dict(os.environ, {"FAKE_NO_LISTEN": "1"}):
            native = rb.NativeProcess(rb.build_command(e, "127.0.0.2", "server", config, job), store.native_dir, job)
        try:
            with self.assertRaisesRegex(rb.BenchError, "listener was not ready"):
                native.wait_ready(ca, config)
            self.assertIn("Waiting for client", native.out_path.read_text())
        finally:
            native.stop()
            ca.close()
            cb.close()
        self.assert_no_native_processes()

    def test_main_dry_run_does_not_connect_or_create_outputs(self):
        with patch.object(rb, "discover_endpoint", return_value=endpoint()), patch.object(rb, "open_control") as connect:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                code = rb.main(["--client", "127.0.0.2", "--all", "--dry-run", "--output-dir", str(self.root / "unused")])
        self.assertEqual(code, 0)
        self.assertIn("96 measured runs", output.getvalue())
        self.assertIn("5s start margin + 10s measured + 5s end margin", output.getvalue())
        self.assertIn("--perform_warm_up", output.getvalue())
        connect.assert_not_called()
        self.assertFalse((self.root / "unused").exists())
        self.assertFalse((self.root / "events.jsonl").exists())

    def test_full_cli_handshake_and_sigterm_cleanup(self):
        # Run the real main() in separate processes, replacing only hardware
        # discovery/route inspection. This exercises signal and peer propagation.
        wrapper = self.root / "launch.py"
        wrapper.write_text(
            "import sys, signal\n"
            f"sys.path.insert(0, {str(Path(rb.__file__).parent)!r})\n"
            "import rdma_bench as rb\n"
            f"rb.PERFTEST_BIN = rb.Path({str(self.bin)!r})\n"
            "ip = '127.0.0.2' if '--server' in sys.argv else '127.0.0.1'\n"
            "e = rb.Endpoint('test', 'fake0', ip, 'fake', 1, 3, '::ffff:' + ip, {'script_sha256':'same'}, {})\n"
            "rb.discover_endpoint = lambda args: e\n"
            "rb.verify_route = lambda *args: None\n"
            "signal.signal(signal.SIGTERM, rb.interrupted)\n"
            "sys.exit(rb.main())\n")
        control, native_port = free_port(), free_port()
        common = ["--output-dir", str(self.root / "cli"), "--control-port", str(control),
                  "--perftest-port", str(native_port), "--connect-timeout", "5"]
        env = {**os.environ, "FAKE_HANG": "1"}
        with (self.root / "server.log").open("w") as sl, (self.root / "client.log").open("w") as cl:
            server = subprocess.Popen([sys.executable, str(wrapper), "--server", *common], stdout=sl, stderr=sl, env=env)
            client = subprocess.Popen([sys.executable, str(wrapper), "--client", "127.0.0.2", "--all", *common], stdout=cl, stderr=cl, env=env)
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    events = self.root / "events.jsonl"
                    if events.exists() and len(events.read_text().splitlines()) >= 2:
                        break
                    if client.poll() is not None or server.poll() is not None:
                        self.fail("CLI exited early: " + (self.root / "client.log").read_text() + (self.root / "server.log").read_text())
                    time.sleep(0.05)
                else:
                    self.fail("CLI did not launch native processes")
                client.send_signal(signal.SIGTERM)
                self.assertEqual(client.wait(timeout=6), 130)
                self.assertEqual(server.wait(timeout=6), 1)
            finally:
                for process in (client, server):
                    if process.poll() is None:
                        process.kill()
                        process.wait()
        self.assert_no_native_processes()
        statuses = list((self.root / "cli").glob("*/status.json"))
        self.assertEqual(len(statuses), 2)
        self.assertTrue(all(json.loads(p.read_text())["status"] == "failed" for p in statuses))


if __name__ == "__main__":
    unittest.main()
