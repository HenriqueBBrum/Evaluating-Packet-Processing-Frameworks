import os
import re
import argparse
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

from parsing_files import parse_trex_log, parse_suricata_stats_log, parse_rx_application


SCENARIO_NAMES = ("BRAS", "AGH", "CICIDS2017_Monday")


def extract_scenario_name(scenario_folder_name):
    upper_name = scenario_folder_name.upper()
    if "CICIDS2017_MONDAY" in upper_name:
        return "CICIDS2017_Monday"
    if "BRAS" in upper_name:
        return "BRAS"
    if "AGH" in upper_name:
        return "AGH"
    return None


def parse_threads_count(threads_folder_name):
    matches = re.findall(r"\d+", threads_folder_name)
    return int(matches[0]) if matches else None


def parse_configuration_eval(input_folder):
    """
    Parse configuration evaluation results from the directory structure.
    
    Returns:
        Dictionary with structure:
        {scenario_name: {threads_count: {strategy: [loop_data]}}}
    """
    parsed_results = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )

    for threads in os.listdir(input_folder):
        threads_folder = os.path.join(input_folder, threads)
        if not os.path.isdir(threads_folder):
            continue

        threads_count = parse_threads_count(threads)
        if threads_count is None:
            continue

        for scenario in os.listdir(threads_folder):
            scenario_folder = os.path.join(threads_folder, scenario)
            if not os.path.isdir(scenario_folder):
                continue

            scenario_name = extract_scenario_name(scenario)
            if scenario_name is None:
                continue

            for strategy in os.listdir(scenario_folder):
                strategy_folder = os.path.join(scenario_folder, strategy)
                if not os.path.isdir(strategy_folder):
                    continue
                    
                for loop in os.listdir(strategy_folder):
                    loop_folder = os.path.join(strategy_folder, loop)
                    if not os.path.isdir(loop_folder):
                        continue

                    tx_packets = parse_trex_log(loop_folder)
                    suricata_total, suricata_drops = parse_suricata_stats_log(loop_folder, tool=strategy)

                    parsed_results[scenario_name][threads_count][strategy].append({
                        "loop": loop,
                        "tx_packets": tx_packets,
                        "suricata_total": suricata_total,
                        "suricata_drops": suricata_drops
                    })

    return {
        scenario: {
            threads_count: dict(strategies)
            for threads_count, strategies in threads_map.items()
        }
        for scenario, threads_map in parsed_results.items()
    }


def plot_stacked_bars_by_scenario(parsed_results, output_folder):
    for scenario_name in SCENARIO_NAMES:
        if scenario_name not in parsed_results:
            continue

        scenario_data = parsed_results[scenario_name]
        thread_counts = sorted(scenario_data.keys())
        if not thread_counts:
            continue

        # Get all distinct strategies across all thread counts
        all_strategies = set()
        for thread_data in scenario_data.values():
            all_strategies.update(thread_data.keys())
        all_strategies = sorted(all_strategies)

        if not all_strategies:
            continue

        fig, ax = plt.subplots(figsize=(12, 6))

        group_positions = np.arange(len(thread_counts))
        num_strategies = len(all_strategies)
        total_group_width = 0.8
        bar_width = total_group_width / num_strategies

        colors = ["#318526", "#FA5C06", "#4169E1", "#c8ac0d", "#9467bd"]
        added_labels = set()

        for group_idx, threads_count in enumerate(thread_counts):
            thread_data = scenario_data[threads_count]

            # Calculate metrics per strategy for sorting worst to best
            strategy_metrics = []
            for strategy in all_strategies:
                if strategy not in thread_data:
                    continue

                loops = thread_data[strategy]
                rx_pcts, inspected_pcts = [], []

                for loop in loops:
                    tx = loop.get("tx_packets", 0)
                    if tx <= 0:
                        continue

                    s_total = loop.get("suricata_total", 0)
                    s_drops = loop.get("suricata_drops", 0)

                    rx_pct = (s_total / tx) * 100
                    # % Inspected = (total_packets - total_drop) / total_tx
                    inspected_pct = ((s_total - s_drops) / tx) * 100

                    print(strategy, rx_pct, inspected_pct)
                    print(tx, s_total, s_drops)
                    input()

                    rx_pcts.append(rx_pct)
                    inspected_pcts.append(inspected_pct)

                avg_rx = np.mean(rx_pcts) if rx_pcts else 0.0
                avg_inspected = np.mean(inspected_pcts) if inspected_pcts else 0.0

                strategy_metrics.append({
                    "strategy": strategy,
                    "avg_rx": avg_rx,
                    "avg_inspected": avg_inspected
                })

            # Sort strategies from worst to best based on RX percentage
            strategy_metrics.sort(key=lambda x: x["avg_rx"])

            # Plot bars for the current thread group
            group_start = group_positions[group_idx] - (total_group_width / 2) + (bar_width / 2)
            for bar_idx, metrics in enumerate(strategy_metrics):
                strat = metrics["strategy"]
                x_pos = group_start + (bar_idx * bar_width)

                inspected_val = metrics["avg_inspected"]
                rx_val = metrics["avg_rx"]

                base_color = colors[bar_idx % len(colors)]
                label_name = strat if strat not in added_labels else None

                # Bottom bar: Inspected Packets
                ax.bar(
                    x_pos,
                    inspected_val,
                    width=bar_width * 0.9,
                    color=base_color,
                    alpha=0.9,
                    label=label_name
                )

                # Top bar: RX Packets stacked on top
                ax.bar(
                    x_pos,
                    rx_val,
                    bottom=inspected_val,
                    width=bar_width * 0.9,
                    color=base_color,
                    alpha=0.4,
                    hatch="//"
                )

                if label_name:
                    added_labels.add(strat)

        ax.set_xlabel("Number of Threads", fontsize=12)
        ax.set_ylabel("Percentage of TX Packets (%)", fontsize=12)
        ax.set_title(f"Packet Processing Metrics (Worst to Best) - Scenario: {scenario_name}")

        ax.set_xticks(group_positions)
        ax.set_xticklabels([f"{t} Threads" for t in thread_counts], fontsize=11)
        ax.set_ylim(0, 205)  # Stacked bars can reach up to 200% combined

        ax.grid(True, axis='y', alpha=0.3)
        ax.legend(title="Strategies", loc="upper left")

        plt.tight_layout()

        output_path = os.path.join(
            output_folder,
            f"{scenario_name}_stacked_packet_percentages.png"
        )
        plt.savefig(output_path, dpi=300, bbox_inches='tight')
        print(f"Saved plot: {output_path}")
        plt.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Plot stacked packet percentage bars per scenario")
    parser.add_argument(
        "-i", "--input",
        type=str,
        required=True,
        help="Path to input directory containing raw results"
    )
    parser.add_argument(
        "-o", "--output",
        type=str,
        required=True,
        help="Path to output directory to save figures"
    )

    args = parser.parse_args()

    if not os.path.isdir(args.input):
        raise ValueError(f"Input folder does not exist: {args.input}")

    os.makedirs(args.output, exist_ok=True)

    parsed_results = parse_configuration_eval(args.input)
    plot_stacked_bars_by_scenario(parsed_results, args.output)
    print("Plotting complete!")