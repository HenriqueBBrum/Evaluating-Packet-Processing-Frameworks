import os
import re
import argparse
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

from parsing_files import parse_trex_log, parse_rx_application, parse_pf_ring_if_output, parse_rx_log, parse_cpu_usage

colors = ["#318526", "#FA5C06", "#c8ac0d", "#4169E1", "#9467bd"]
markers = ["^", "*", "s", "p"]

def parse_configuration_eval(input_folder, parsed_results=None, folders_to_skip=None):
    """
    Parse configuration evaluation results from the directory structure.
    
    Args:
        input_folder: Path to the input folder containing results
        parsed_results: Dictionary to accumulate results (default: new dict)
        folders_to_skip: Folders to skip during parsing
    
    Returns:
        Dictionary with structure: {scenario_key: {strategy: {config_key: [loop_data]}}}
        where scenario_key = (speed, packet_size, protocol)
    """
    if parsed_results is None:
        parsed_results = {}

    for threads in os.listdir(input_folder):
        threads_folder = os.path.join(input_folder, threads)
        if not os.path.isdir(threads_folder):
            continue

        for scenario in os.listdir(threads_folder):
            scenario_folder = os.path.join(threads_folder, scenario)
            if not os.path.isdir(scenario_folder):
                continue

            scenario_info = scenario.split("_")

            speed = re.findall(r'\d+', scenario_info[0])[0]
            packet_size = re.findall(r'\d+', scenario_info[1])[0]
            protocol = scenario_info[2]

            scenario_key = (speed, packet_size, protocol)
            if scenario_key not in parsed_results:
                parsed_results[scenario_key] = {}

            for strategy in os.listdir(scenario_folder):
                strategy_folder = os.path.join(scenario_folder, strategy)
                if not os.path.isdir(strategy_folder):
                    continue

                if strategy not in parsed_results[scenario_key]:
                    parsed_results[scenario_key][strategy] = {}

                for config in os.listdir(strategy_folder):
                    config_folder = os.path.join(strategy_folder, config)
                    if not os.path.isdir(config_folder):
                        continue

                    config_key = config.split("_", 1)[-1]

                    if config_key not in parsed_results[scenario_key][strategy]:
                        parsed_results[scenario_key][strategy][config_key] = []

                    for loop in os.listdir(config_folder):
                        loop_folder = os.path.join(config_folder, loop)
                        if not os.path.isdir(loop_folder):
                            continue

                        tx_packets = parse_trex_log(loop_folder)
                        rx_app_packets = parse_rx_application(loop_folder, pf_ring= True if "pf_ring" in strategy.lower() else False)
                        if "pf_ring" in strategy.lower():
                            total_pf_ring_packets, total_drops = parse_pf_ring_if_output(loop_folder)
                        rx_ethtool_packets, out_of_buffer = parse_rx_log(loop_folder)
                        cpu_usage = parse_cpu_usage(loop_folder)
                        parsed_results[scenario_key][strategy][config_key].append({
                            "loop": loop,
                            "tx_packets": tx_packets,
                            "rx_app_packets": rx_app_packets,
                            "rx_ethtool_packets": rx_ethtool_packets,
                            "out_of_buffer": out_of_buffer,
                            "total_pf_ring_packets": total_pf_ring_packets if "pf_ring" in strategy.lower() else None,
                            "total_drops": total_drops if "pf_ring" in strategy.lower() else None,
                            "cpu_usage": cpu_usage
                        })

    return parsed_results


def _average_rx_percentage(loops_data):
    rx_percentages = []
    for loop in loops_data:
        if (loop["rx_app_packets"] is not None and
            loop["tx_packets"] is not None and
            loop["tx_packets"] > 0):
            percentage = (loop["rx_app_packets"] / loop["tx_packets"]) * 100
            rx_percentages.append(percentage)

    return np.mean(rx_percentages) if rx_percentages else 0


def plot_by_speed(parsed_results, output_folder):
    # Group data by speed
    speeds_data = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for (speed, packet_size, protocol), strategies in parsed_results.items():
        for strategy, configs in strategies.items():
            for config_key, loops_data in configs.items():
                avg_rx_percentage = _average_rx_percentage(loops_data)
                
                speeds_data[speed][(packet_size, protocol)][strategy].append({
                    "config": config_key,
                    "rx_percentage": avg_rx_percentage
                })
    
    # For each speed and strategy, select only the best config
    best_configs = defaultdict(lambda: defaultdict(dict))
    for speed, scenario_data in speeds_data.items():
        for (packet_size, protocol), strategy_data in scenario_data.items():
            for strategy, configs_list in strategy_data.items():
                if strategy not in best_configs[speed][(packet_size, protocol)]:
                    # Find config with highest average rx_percentage across all scenarios
                    best_config = max(configs_list, key=lambda x: x["rx_percentage"])
                    best_configs[speed][(packet_size, protocol)][strategy] = best_config["config"]

    
    # Create a plot for each speed
    for speed in sorted(speeds_data.keys(), key=lambda x: int(x)):
        fig, ax = plt.subplots(figsize=(12, 6))
        
        # Prepare data for X-axis (sorted by packet_size, then protocol)
        x_labels = sorted(speeds_data[speed].keys(), 
                         key=lambda x: (int(x[0]), x[1]))
        x_positions = np.arange(len(x_labels))
        
        # Get all unique strategies
        all_strategies = set()
        for scenario_data in speeds_data[speed].values():
            all_strategies.update(scenario_data.keys())
        
        all_strategies = sorted(all_strategies)
        
        # Plot lines for each strategy (only the best config)
        color_idx = 0
        for strategy in all_strategies:
            strategy_color = colors[color_idx % len(colors)]
            strategy_marker = markers[color_idx % len(markers)]
            color_idx += 1
            
            # Collect Y values for this strategy and best config across all scenarios
            y_values = []
            valid_x_positions = []
            
            for x_pos, (packet_size, protocol) in enumerate(x_labels):
                best_config = best_configs[speed][(packet_size, protocol)][strategy]
                scenario_data = speeds_data[speed][(packet_size, protocol)]
                
                if strategy in scenario_data:
                    # Find matching best config
                    for config_entry in scenario_data[strategy]:
                        if config_entry["config"] == best_config:
                            y_values.append(config_entry["rx_percentage"])
                            valid_x_positions.append(x_pos)
                            break

            # Plot line if there's data
            if valid_x_positions:
                ax.plot(
                    valid_x_positions, 
                    y_values, 
                    marker=strategy_marker,
                    linestyle='-',
                    color=strategy_color,
                    linewidth=2,
                    markersize=8,
                    label=f"{strategy}",
                    alpha=0.8
                )
        
        # Customize plot
        ax.set_xlabel("Packet Size (bytes) & Protocol")
        ax.set_ylabel("RX Packets (%)")
        # ax.set_title(f"Packet Reception Percentage - Speed: {speed} Gbps", )

        ax.set_ylim(-5,105)
        ax.set_xticks(x_positions)
        ax.set_xticklabels([f"{size}B/{proto}" for size, proto in x_labels], fontsize=12)
        ax.tick_params(which="major", labelsize=12)

        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=12)
        
        plt.tight_layout()
        
        # Save plot
        output_path = os.path.join(output_folder, f"speed_{speed}Gbps_rx_percentage.png")
        plt.savefig(output_path, dpi=300, bbox_inches='tight')
        print(f"Saved plot: {output_path}")
        plt.close()


def plot_configs_by_strategy(parsed_results, output_folder):
    strategy_speed_config = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))

    for (speed, packet_size, proto), strategies in parsed_results.items():
        for strategy, configs in strategies.items():
            for config_key, loops_data in configs.items():
                avg_rx_percentage = _average_rx_percentage(loops_data)
                strategy_speed_config[strategy][packet_size+"_"+proto][config_key][speed].append(avg_rx_percentage)

    for strategy in sorted(strategy_speed_config.keys()):
        for packet_proto_key in sorted(strategy_speed_config[strategy].keys()):
            fig, ax = plt.subplots(figsize=(10, 6))

            config_data = strategy_speed_config[strategy][packet_proto_key]
            all_speeds = sorted(
                {speed for speed_data in config_data.values() for speed in speed_data.keys()},
                key=lambda x: int(x)
            )
            x_positions = np.arange(len(all_speeds))
            speed_to_x = {speed: idx for idx, speed in enumerate(all_speeds)}
            for idx, config_key in enumerate(sorted(config_data.keys())):
                y_values = []
                valid_x_positions = []
                for speed in all_speeds:
                    values = config_data[config_key].get(speed, [])
                    if values:
                        y_values.append(float(np.mean(values)))
                        valid_x_positions.append(speed_to_x[speed])

                if valid_x_positions:
                    ax.plot(
                        valid_x_positions,
                        y_values,
                        marker=markers[idx % len(markers)],
                        linestyle='-',
                        color=colors[idx % len(colors)],
                        linewidth=2,
                        markersize=7,
                        label=config_key,
                        alpha=0.85
                    )

            ax.set_xlabel("Speed (Gbps)")
            ax.set_ylabel("RX Packets (%)")
            ax.set_ylim(-5, 105)

            ax.set_xticks(x_positions)
            ax.set_xticklabels([f"{speed}" for speed in all_speeds])

            ax.tick_params(which="major", labelsize=12)

            ax.grid(True, alpha=0.3)
            ax.legend(title="Config", fontsize=12)

            plt.tight_layout()

            safe_strategy = re.sub(r"[^a-zA-Z0-9._-]", "_", strategy)
            output_path = os.path.join(output_folder, f"strategy_{safe_strategy}_{packet_proto_key}.png")
            plt.savefig(output_path, dpi=300, bbox_inches='tight')
            print(f"Saved plot: {output_path}")
            plt.close()




if __name__ == "__main__":
    # Get input and output folders from command line arguments
    parser = argparse.ArgumentParser(description="Parse configuration evaluation results")
    parser.add_argument(
        "-i", "--input",
        type=str,
        required=True,
        help="Path to the input folder containing configuration evaluation results"
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

    # Generate plots grouped by speed
    plot_by_speed(parsed_results, args.output)
    plot_configs_by_strategy(parsed_results, args.output)
    
    print("Plotting complete!")
