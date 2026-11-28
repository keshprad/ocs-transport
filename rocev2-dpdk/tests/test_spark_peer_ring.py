import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check_spark_peer_ring as profile
import peer_vf


class PeerRingTest(unittest.TestCase):
    def test_each_peer_has_one_uplink_and_each_worker_has_four_cores(self):
        links = {(tx.host, rx.host): (tx, rx) for tx, rx in profile.edges()}
        self.assertEqual(len(links), 6)
        for host in profile.ring.HOSTS:
            ports, cores = set(), []
            for (src, dst), (tx, _) in links.items():
                if src != host:
                    continue
                _, rx = links[dst, src]
                self.assertEqual(tx.pci, rx.pci)
                ports.add(tx.pci)
                cores.extend(tx.cpus + rx.cpus)
            self.assertEqual(ports, set(profile.INTERFACES))
            self.assertEqual(len(cores), 16)
            self.assertEqual(len(set(cores)), 16)

    def test_receiver_start_substitutes_both_devices_only(self):
        for _, rx in profile.edges():
            args = profile.unit_argv(rx, profile.RELEASE, profile.RELEASE / "lib")
            cmd = "ExecStart=" + " ".join(args)
            old = cmd.replace(peer_vf.PLACEHOLDER, peer_vf.OLD[rx.name])
            result = peer_vf.dropin(rx.name, old)
            self.assertIn(cmd + "\n", result)
            self.assertEqual(cmd.count(peer_vf.PLACEHOLDER), 2)
            self.assertIn("prepare " + rx.name, result)
            self.assertIn("release " + rx.name, result)
            with self.assertRaises(RuntimeError):
                peer_vf.dropin(rx.name, old.replace("4096", "1024"))

    def test_vf_failure_rolls_back_and_foreign_allocations_survive(self):
        role = "forward-rx"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parent = root / peer_vf.PARENTS[role]
            iface = parent / "net" / "pf0"
            iface.mkdir(parents=True)
            (iface / "mtu").write_text("4200")
            count = parent / "sriov_numvfs"
            state = root / "owner"
            with (patch.object(peer_vf, "PCI", root),
                  patch.object(peer_vf, "state_file", return_value=state),
                  patch.dict(os.environ, INVOCATION_ID="f"*32)):
                count.write_text("1")
                with self.assertRaisesRegex(RuntimeError, "already has VFs"):
                    peer_vf.prepare(role)
                self.assertEqual(count.read_text(), "1")
                self.assertFalse(state.exists())

                count.write_text("0")
                vf = root / "0002:01:01.2"
                (vf / "net" / "vf0").mkdir(parents=True)
                (vf / "physfn").symlink_to(parent)
                (parent / "virtfn0").symlink_to(vf)
                with patch.object(peer_vf, "run", side_effect=RuntimeError("VF setup error")):
                    with self.assertRaisesRegex(RuntimeError, "VF setup error"):
                        peer_vf.prepare(role)
                self.assertEqual(count.read_text().strip(), "0")
                self.assertFalse(state.exists())

                count.write_text("1")
                state.write_text("# " + "a"*32 + "\n")
                with self.assertRaisesRegex(RuntimeError, "different invocation"):
                    peer_vf.release(role)
                self.assertEqual(count.read_text(), "1")


if __name__ == "__main__":
    unittest.main()
