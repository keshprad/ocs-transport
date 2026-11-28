import unittest

from check_spark_bandwidth import DMA_PROOF_PROFILES, Endpoint, parse_result
from spark_services import CycleError


def sender_journal(endpoint):
    profile = endpoint.settings
    payload, writes = profile.payload, profile.writes
    messages = [
        f"BANDWIDTH role=tx payload_bytes={payload} elapsed_ns={payload} thread_cpu_ns={payload} "
        f"payload_gbps=8.000 poll_cores=1.000 writes={writes} qps=1 workers=1 send_window=256 tx_burst=1",
        f"WRITE complete: {payload} bytes, {writes} packets, 0 retries, peer verified",
        f"Application completed {payload} bytes",
        f"Application observed {writes} WRITE completions per QP via libibverbs",
        f"Application DMA-BUF region mapped for scatter/gather TX ({payload} bytes)",
        "APP_PROCESSES worker_pid=11 app_pid=12",
        f"APP_SERIAL_COMPLETIONS writes_per_qp={writes}",
        "DPDK_TX_BURST_COMPLETION queues=1",
        f"MLX5_DMABUF_MAP device={endpoint.link[1]} address=0x10000 bytes={payload} lkey=0x00002234 offset=0",
    ]
    for write in range(writes):
        wqe = write * 2
        messages += [
            f"MLX5_DMABUF_DSEG port=0 queue=0 wqe={wqe} address=0x{0x10000 + write * profile.write_bytes:x} "
            f"bytes={profile.write_bytes} lkey=0x00002234",
            f"MLX5_DMABUF_CQE port=0 queue=0 wqe={wqe}",
        ]
    return messages


class DmaProofTest(unittest.TestCase):
    def test_registered_payload_and_hardware_completion_at_both_mtus(self):
        for name, profile in DMA_PROOF_PROFILES.items():
            with self.subTest(mtu=profile.ethernet_mtu):
                endpoint = Endpoint("tx", name)
                proof = parse_result(endpoint, sender_journal(endpoint))["dma_buf_proof"]
                self.assertEqual(proof["registered_bytes"], profile.payload)
                self.assertEqual(proof["completed_payload_wqes"], profile.writes)
                self.assertEqual(proof["verified_offsets"], list(range(0, profile.payload, profile.write_bytes)))

    def test_registration_log_alone_does_not_prove_dma(self):
        endpoint = Endpoint("tx", "app-mr-gather-mtu-1500-dma-proof")
        lines = [line for line in sender_journal(endpoint) if not line.startswith("MLX5_DMABUF_")]
        with self.assertRaisesRegex(CycleError, "registration record"):
            parse_result(endpoint, lines)

    def test_rejects_wrong_descriptor_and_missing_hardware_completion(self):
        endpoint = Endpoint("tx", "app-mr-gather-mtu-1500-dma-proof")
        lines = sender_journal(endpoint)
        first_dseg = next(i for i, line in enumerate(lines) if line.startswith("MLX5_DMABUF_DSEG "))
        for old, new in (("0x00002234", "0x00001111"), ("0x10000", "0x10001"),
                         ("bytes=1024", "bytes=1008")):
            with self.subTest(field=old):
                changed = lines.copy()
                changed[first_dseg] = changed[first_dseg].replace(old, new)
                with self.assertRaisesRegex(CycleError, "descriptor does not match"):
                    parse_result(endpoint, changed)

        with self.assertRaisesRegex(CycleError, "missing successful mlx5 TX completion"):
            parse_result(endpoint, lines[:-1])
        with self.assertRaisesRegex(CycleError, "TX completion error"):
            parse_result(endpoint, lines + ["MLX5_DMABUF_CQ_ERROR port=0 queue=0"])


if __name__ == "__main__":
    unittest.main()
