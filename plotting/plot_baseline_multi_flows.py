import os
import re
import argparse
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

from parsing_files import parse_trex_log, parse_rx_application, parse_pf_ring_if_output, parse_rx_log, parse_cpu_usage


SCENARIO_NAMES = ("AGH", "BRAS", "CICIDS2017_Monday")

colors = ["#318526", "#FA5C06", "#c8ac0d", "#4169E1", "#9467bd"]
markers = ["^", "*", "s", "p", "o"]


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


def parse_configuration_eval(input_folder, parsed_results=None, folders_to_skip=None):
    """
    Parse configuration evaluation results from the directory structure.
    
    Args:
        input_folder: Path to the input folder containing results
        parsed_results: Dictionary to accumulate results (default: new dict)
        folders_to_skip: Folders to skip during parsing
    
    Returns:
        Dictionary with structure:
        {scenario_name: {threads_count: {strategy: {config_key: [loop_data]}}}}
    """
    if parsed_results is None:
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
                    rx_app_packets = parse_rx_application(loop_folder, pf_ring= True if "pf_ring" in strategy.lower() else False)
                    if "pf_ring" in strategy.lower():
                        total_pf_ring_packets, total_drops = parse_pf_ring_if_output(loop_folder)
                    rx_ethtool_packets, out_of_buffer = parse_rx_log(loop_folder)
                    cpu_usage = parse_cpu_usage(loop_folder)
                    parsed_results[scenario_name][threads_count][strategy].append({
                        "loop": loop,
                        "tx_packets": tx_packets,
                        "rx_app_packets": rx_app_packets,
                        "rx_ethtool_packets": rx_ethtool_packets,
                        "out_of_buffer": out_of_buffer,
                        "total_pf_ring_packets": total_pf_ring_packets if "pf_ring" in strategy.lower() else None,
                        "total_drops": total_drops if "pf_ring" in strategy.lower() else None,
                        "cpu_usage": cpu_usage
                    })

    return {
        scenario: {
            threads_count: {
                strategy: loops
                for strategy, loops in strategies.items()
            }
            for threads_count, strategies in threads_map.items()
        }
        for scenario, threads_map in parsed_results.items()
    }

def plot_by_scenario(parsed_results, output_folder):
    scenarios = [s for s in SCENARIO_NAMES if s in parsed_results]
    if not scenarios:
        return

    # Create vertically stacked subplots (N rows, 1 column) sharing X axis
    fig, axes = plt.subplots(
        nrows=len(scenarios),
        ncols=1,
        figsize=(12, 4 * len(scenarios)),
        sharex=True
    )

    # Handle single subplot case where axes is not an array
    if len(scenarios) == 1:
        axes = [axes]

    for idx, scenario_name in enumerate(scenarios):
        ax = axes[idx]
        scenario_data = parsed_results[scenario_name]

        thread_counts = sorted(scenario_data.keys())
        x_positions = np.arange(len(thread_counts))

        all_strategies = sorted({
            strategy
            for thread_data in scenario_data.values()
            for strategy in thread_data.keys()
        })

        for color_idx, strategy in enumerate(all_strategies):
            strategy_color = colors[color_idx % len(colors)]
            strategy_marker = markers[color_idx % len(markers)]

            y_values = []
            for threads_count in thread_counts:
                if strategy not in scenario_data[threads_count]:
                    y_values.append(np.nan)
                    continue

                loop_rx_percentages = []
                loops_data = scenario_data[threads_count][strategy]
                for loop in loops_data:
                    tx_packets = loop.get("tx_packets")
                    rx_app_packets = loop.get("rx_app_packets")
                    if tx_packets is None or rx_app_packets is None or tx_packets <= 0:
                        continue
                    loop_rx_percentages.append((rx_app_packets / tx_packets) * 100)

                y_values.append(np.mean(loop_rx_percentages) if loop_rx_percentages else np.nan)

            if not np.all(np.isnan(y_values)):
                ax.plot(
                    x_positions,
                    y_values,
                    marker=strategy_marker,
                    linestyle='-',
                    color=strategy_color,
                    linewidth=2,
                    markersize=8,
                    label=strategy,
                    alpha=0.8
                )

        # Bold scenario label on top of each subplot
        ax.set_title(f"{scenario_name}", fontweight='bold', fontsize=14)

        ax.set_ylabel("RX Packets (%)", fontsize=14)
        ax.set_ylim(-5, 105)
        ax.set_xticks(x_positions)
        ax.grid(True, alpha=0.3)

    # Set x-ticks and label on the bottom subplot
    last_scenario_threads = sorted(parsed_results[scenarios[-1]].keys())
    axes[-1].set_xticklabels([str(tc) for tc in last_scenario_threads], fontsize=14)
    axes[-1].set_xlabel("Threads", fontsize=14)

    # Single shared legend at the top of the entire figure
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.02), ncol=len(labels), fontsize=12)

    plt.tight_layout()

    # Save composite figure
    output_path = os.path.join(output_folder, "baseline_multi_flows.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.savefig(output_path.split(".")[0] + ".eps", format="eps", bbox_inches="tight")
    print(f"Saved plot: {output_path}")
    plt.close()



if __name__ == "__main__":
    # Get input and output folders from command line arguments
    parser = argparse.ArgumentParser(description="Parse baseline multi flow evaluation results")
    parser.add_argument(
        "-i", "--input",
        type=str,
        required=True,
        help="Path to the input folder containing baseline multi flow evaluation results"
    )
    parser.add_argument(
        "-o", "--output",
        type=str,
        required=True,
        help="Path to the output folder where graphs will be saved"
    )
    
    args = parser.parse_args()
    
    if not os.path.isdir(args.input):
        raise ValueError(f"Input folder does not exist: {args.input}")
    
    os.makedirs(args.output, exist_ok=True)
        
    print(f"Input folder: {args.input}")
    print(f"Output folder: {args.output}")
    
    # Parse configuration evaluation results
    parsed_results = parse_configuration_eval(args.input)
    
    print(f"Parsed {len(parsed_results)} scenarios")

    plt.rcParams.update({'font.size': 14})
    plt.rcParams.update({'legend.title_fontsize': 12})

    plot_by_scenario(parsed_results, args.output)
    
    print("Plotting complete!")
