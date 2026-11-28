"""Render paper-sized vector figures and PNG previews from the archived trials."""

import csv
import json
from collections import defaultdict
from decimal import Decimal
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch


FIGURES = Path(__file__).resolve().parent
RESULTS = FIGURES.parent
RING = RESULTS / "ring/2026-11-25"
PORTS = RESULTS / "spark-04-two-qsfp"
COLUMN = 3.4  # inches at final print size
DOUBLE_COLUMN = 7.0
INK = "#252525"
BLUE = "#4f7491"
HATCHED = {"facecolor": "white", "edgecolor": INK, "hatch": "////"}
FILLED = {"facecolor": BLUE, "edgecolor": INK}
TRIAL_MARK = {"fmt": "o", "ms": 2.5, "mfc": "white", "mec": INK, "mew": 0.55,
              "ecolor": INK, "elinewidth": 0.55, "capsize": 1.4, "zorder": 4}


def read_report(path):
    return json.loads(path.read_text())


def ring_points(report):
    bounds = [t["aggregate_payload_gbps_bounds"] for t in report["trials"]]
    center = (median(b[0] for b in bounds) + median(b[1] for b in bounds)) / 2
    return center, bounds


def hardware_totals(path):
    totals = defaultdict(Decimal)
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            if row["mode"] == "both":
                totals[row["repeat"]] += Decimal(row["gbps"])
    return [float(total) for total in totals.values()]


def style_axes(ax, grid_axis):
    ax.set_axisbelow(True)
    ax.grid(axis=grid_axis, color="#dedede", linewidth=0.45)
    ax.spines[["top", "right"]].set_visible(False)


def draw_trials(ax, x, bounds, offsets):
    for dx, (low, high) in zip(offsets, bounds, strict=True):
        ax.errorbar(x + dx, (low + high) / 2, yerr=(high - low) / 2 if high > low else None,
                    **TRIAL_MARK)


def save(fig, name):
    fig.savefig(FIGURES / f"{name}.pdf", metadata={"CreationDate": None, "ModDate": None})
    fig.savefig(FIGURES / f"{name}.png", dpi=600)
    plt.close(fig)


def sender_ports():
    fig, ax = plt.subplots(figsize=(COLUMN, 2.58))
    fig.subplots_adjust(left=0.16, right=0.99, bottom=0.17, top=0.75)
    one_peer = RESULTS / "spark-04-to-02"
    groups = [
        (read_report(PORTS / "same-cable-mtu1500.json")["aggregate"],
         read_report(one_peer / "same-cable-jumbo/results.json"),
         read_report(one_peer / "hardware-mtu1500.json")["results"]["aggregate-multi"]),
        (read_report(PORTS / "mtu1500.json"),
         read_report(PORTS / "mtu4200-hugepages.json"),
         read_report(PORTS / "hardware/mtu1500.json")["results"]["1500"]),
    ]
    series = [
        ("DPDK, MTU 1500, 4 KiB packet-pool pages", HATCHED),
        ("DPDK, MTU 4200, 2 MiB packet-pool pages", FILLED),
        ("Hardware RoCEv2, MTU 1500", {"facecolor": "#bcbcbc", "edgecolor": INK}),
    ]
    for group, reports in enumerate(groups):
        for offset, report, (_, fill) in zip((-0.30, 0, 0.30), reports, series, strict=True):
            x = group + offset
            rates = [trial["payload_gbps"] for trial in report["trials"]]
            center = median(rates)
            ax.bar(x, center, width=0.24, linewidth=0.65, **fill)
            draw_trials(ax, x, [(r, r) for r in rates], (-0.06, 0, 0.06))
            ax.annotate(f"{center:.2f}", (x, max(rates)), xytext=(0, 4),
                        textcoords="offset points", ha="center", va="bottom")
    style_axes(ax, "y")
    ax.set(xlim=(-0.54, 1.54), ylim=(0, 225), yticks=range(0, 201, 50),
           ylabel="Aggregate goodput (Gb/s)")
    ax.set_xticks([0, 1], ["One 100G port\nOne peer", "Two 100G ports\nTwo peers"])
    fig.legend(handles=[Patch(**fill, label=label) for label, fill in series],
               loc="upper center", bbox_to_anchor=(0.53, 0.99), ncols=1, frameon=False,
               borderaxespad=0, labelspacing=0.14, handlelength=1.6)
    save(fig, "one-way-ports")


def pcie_placement():
    links = ["spark-04-to-spark-02", "spark-02-to-spark-01", "spark-01-to-spark-04"]
    nov27 = RESULTS / "ring/2026-11-27"
    modes = [
        ("Scatter/gather TX", [RING / "one-way-sg-shared/results.json", RING / "one-way-sg-split/results.json"]),
        ("Copy TX", [nov27 / "one-way-copy-tx-app-rx-shared/results.json",
                     nov27 / "one-way-copy-tx-app-rx-split/results.json"]),
    ]
    markers = [{"marker": "o", "mfc": "white", "mec": INK},
               {"marker": "D", "mfc": BLUE, "mec": INK}]
    fig, panels = plt.subplots(2, 2, figsize=(DOUBLE_COLUMN, 3.80), sharey="col",
                               gridspec_kw={"width_ratios": [1, 1.38]})
    fig.subplots_adjust(left=0.083, right=0.98, bottom=0.12, top=0.875, wspace=0.41, hspace=0.53)

    for row, (mode, reports) in enumerate(modes):
        agg, edges = panels[row]
        aggregate_label, link_label = [("a", "b"), ("c", "d")][row]
        agg.set_title(f"({aggregate_label}) {mode} · Ring aggregate", loc="left")
        edges.set_title(f"({link_label}) {mode} · Directed links", loc="left")
        centers = []
        per_link = []
        positions = [(0, -0.14, -4), (1, 0.13, 4)]
        for path, (x, y_offset, label_offset), marker in zip(reports, positions, markers, strict=True):
            report = read_report(path)
            center, bounds = ring_points(report)
            centers.append(center)
            draw_trials(agg, x, bounds, (-0.12, -0.06, 0.10))
            agg.plot(x, center, ms=4.8, mew=0.8, zorder=5, **marker)
            agg.annotate(f"{center:.1f}", (x, max(b[1] for b in bounds)), xytext=(0, 5),
                         textcoords="offset points", ha="center")
            rates = [median(float(t["links"][link]["tx"]["payload_gbps"]) for t in report["trials"])
                     for link in links]
            per_link.append(rates)
            for y, rate in enumerate(rates):
                y_pos = y + y_offset
                edges.plot(rate, y_pos, ms=4.8, mew=0.8, zorder=4, **marker)
                edges.annotate(f"{rate:.1f}", (rate, y_pos), xytext=(label_offset, 0),
                               textcoords="offset points", va="center", ha="right" if label_offset < 0 else "left")
        agg.plot([0, 1], centers, color="#888888", lw=0.8, zorder=1)
        for y, (shared, split) in enumerate(zip(*per_link, strict=True)):
            edges.plot([shared, split], [y - 0.14, y + 0.13], color="#888888", lw=0.8, zorder=1)
        agg.set(xlim=(-0.32, 1.32), ylim=(264, 304), yticks=[270, 280, 290, 300],
                ylabel="Goodput (Gb/s)")
        agg.set_xticks([0, 1], ["Shared PCIe", "Split PCIe"])
        edges.set(xlim=(88, 100.4), ylim=(2.49, -0.49), xticks=[90, 92, 94, 96, 98, 100])
        edges.set_yticks(range(len(links)), ["04 → 02", "02 → 01", "01 → 04"])
        style_axes(agg, "y")
        style_axes(edges, "x")
    panels[1, 1].set_xlabel("Sender goodput (Gb/s)")
    fig.legend(handles=[Line2D([], [], linestyle="none", ms=4.8, label=label, **marker)
                        for label, marker in zip(("Shared PCIe", "Split PCIe"), markers, strict=True)],
               loc="upper center", bbox_to_anchor=(0.51, 0.994), borderaxespad=0,
               frameon=False, ncols=2, columnspacing=1.35, handlelength=1.0)
    save(fig, "one-way-ring-pcie")


def full_duplex():
    shared_runs = RESULTS / "ring/2026-11-27"
    copy = ring_points(read_report(shared_runs / "full-duplex-copy-shared/results.json"))
    gather = ring_points(read_report(shared_runs / "full-duplex-sg-shared/results.json"))
    hardware = [hardware_totals(RESULTS / path) for path in (
        "ring/2026-11-24/hardware/rates-after.csv", "ring/2026-11-27/full-duplex-hardware-mtu4200/rates.csv")]
    fig, ax = plt.subplots(figsize=(COLUMN, 2.56))
    fig.subplots_adjust(left=0.16, right=0.99, bottom=0.315, top=0.96)
    configs = [copy, gather] + [(median(rates), [(v, v) for v in rates]) for rates in hardware]
    fills = [{"facecolor": "#c8c8c8", "edgecolor": INK}, FILLED, HATCHED, {**HATCHED, "hatch": "xx"}]
    for x, ((center, bounds), fill) in enumerate(zip(configs, fills, strict=True)):
        ax.bar(x, center, width=0.55, linewidth=0.65, **fill)
        draw_trials(ax, x, bounds, (-0.12, 0, 0.12))
        ax.annotate(f"{center:.2f}" if x >= 2 else f"{center:.1f}",
                    (x, max(b[1] for b in bounds)), xytext=(0, 5), textcoords="offset points", ha="center",
                    bbox={"facecolor": "white", "edgecolor": "none", "pad": 0.35})
    style_axes(ax, "y")
    ax.set(xlim=(-0.59, 3.59), ylim=(0, 650), yticks=range(0, 601, 100),
           ylabel="Aggregate goodput (Gb/s)")
    ax.set_xticks(range(4), ["Copy", "Scatter/gather", "MTU 1500\n(default)", "MTU 4200"],
                  va="top", linespacing=1.0)
    for left, right, label in ((0, 1, "DPDK, shared PCIe, MTU 4200\n2 MiB packet-pool pages"), (2, 3, "Hardware RoCEv2")):
        transform = ax.get_xaxis_transform()
        ax.plot([left - 0.37, right + 0.37], [-0.235, -0.235], transform=transform,
                color=INK, lw=0.5, clip_on=False)
        ax.text((left + right) / 2, -0.28, label, transform=transform, ha="center", va="top")
    save(fig, "full-duplex-ring")


if __name__ == "__main__":
    plt.rcParams.update({"font.family": "STIXGeneral", "font.size": 8.5, "axes.labelsize": 9,
                         "axes.titlesize": 9, "axes.titleweight": "bold", "axes.titlepad": 6,
                         "axes.linewidth": 0.6, "text.color": INK, "axes.edgecolor": INK,
                         "axes.labelcolor": INK, "xtick.color": INK, "ytick.color": INK,
                         "xtick.major.size": 2.5, "ytick.major.size": 2.5, "hatch.linewidth": 0.4,
                         "pdf.fonttype": 42, "savefig.facecolor": "white"})
    sender_ports()
    pcie_placement()
    full_duplex()
