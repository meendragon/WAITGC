#!/usr/bin/env python3
"""Fit a compact monotone Age step function for CONV_GC_POLICY=3.

Input CSV columns:
  age_ns,future_invalid_pages[,weight]

The response is the number of additional invalid pages observed over a fixed
future horizon.  It is fitted as a monotonically non-increasing function of
Age using PAVA.  Dynamic programming then reduces the PAVA fit to at most
seven steps.  The inverse fitted gain becomes the monotonically non-decreasing
victim-selection multiplier exported to conv_ftl.h.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Block:
    age_min: int
    age_max: int
    weight: float
    weighted_y: float
    weighted_y2: float
    samples: int

    @property
    def mean(self) -> float:
        return self.weighted_y / self.weight

    @property
    def sse(self) -> float:
        value = self.weighted_y2 - self.weighted_y * self.weighted_y / self.weight
        return max(0.0, value)


def merge_blocks(blocks: Iterable[Block]) -> Block:
    items = list(blocks)
    if not items:
        raise ValueError("cannot merge an empty block list")
    return Block(
        age_min=items[0].age_min,
        age_max=items[-1].age_max,
        weight=sum(item.weight for item in items),
        weighted_y=sum(item.weighted_y for item in items),
        weighted_y2=sum(item.weighted_y2 for item in items),
        samples=sum(item.samples for item in items),
    )


def load_points(path: Path) -> tuple[list[Block], int]:
    rows: list[tuple[int, float, float]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {"age_ns", "future_invalid_pages"}
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"missing CSV columns: {', '.join(sorted(missing))}")

        for line_no, row in enumerate(reader, start=2):
            try:
                age = int(row["age_ns"])
                future_invalid = float(row["future_invalid_pages"])
                weight = float(row.get("weight") or 1.0)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid numeric value at CSV line {line_no}") from exc
            if age < 0 or future_invalid < 0 or not math.isfinite(future_invalid):
                raise ValueError(f"negative or non-finite sample at CSV line {line_no}")
            if weight <= 0 or not math.isfinite(weight):
                raise ValueError(f"weight must be finite and positive at CSV line {line_no}")
            rows.append((age, future_invalid, weight))

    if not rows:
        raise ValueError("input CSV contains no samples")

    rows.sort(key=lambda item: item[0])
    points: list[Block] = []
    cursor = 0
    while cursor < len(rows):
        age = rows[cursor][0]
        same_age: list[tuple[int, float, float]] = []
        while cursor < len(rows) and rows[cursor][0] == age:
            same_age.append(rows[cursor])
            cursor += 1
        weight = sum(item[2] for item in same_age)
        weighted_y = sum(item[1] * item[2] for item in same_age)
        weighted_y2 = sum(item[1] * item[1] * item[2] for item in same_age)
        points.append(Block(age, age, weight, weighted_y, weighted_y2, len(same_age)))

    return points, len(rows)


def prebin(points: list[Block], max_bins: int) -> list[Block]:
    if len(points) <= max_bins:
        return points
    result: list[Block] = []
    for index in range(max_bins):
        start = index * len(points) // max_bins
        end = (index + 1) * len(points) // max_bins
        if start < end:
            result.append(merge_blocks(points[start:end]))
    return result


def pava_decreasing(points: list[Block]) -> list[Block]:
    """Weighted PAVA for a response constrained to be non-increasing."""
    fitted: list[Block] = []
    for point in points:
        fitted.append(point)
        while len(fitted) >= 2 and fitted[-2].mean < fitted[-1].mean:
            fitted[-2:] = [merge_blocks(fitted[-2:])]
    return fitted


def segment_cost(prefix_w: list[float], prefix_y: list[float],
                 prefix_y2: list[float], start: int, end: int) -> float:
    weight = prefix_w[end] - prefix_w[start]
    weighted_y = prefix_y[end] - prefix_y[start]
    weighted_y2 = prefix_y2[end] - prefix_y2[start]
    return max(0.0, weighted_y2 - weighted_y * weighted_y / weight)


def reduced_fits(blocks: list[Block], max_steps: int) -> dict[int, tuple[float, list[Block]]]:
    """Return the best adjacent coarsening of the PAVA blocks for every K."""
    count = len(blocks)
    max_steps = min(max_steps, count)
    prefix_w = [0.0]
    prefix_y = [0.0]
    prefix_y2 = [0.0]
    for block in blocks:
        prefix_w.append(prefix_w[-1] + block.weight)
        prefix_y.append(prefix_y[-1] + block.weighted_y)
        prefix_y2.append(prefix_y2[-1] + block.weighted_y2)

    inf = float("inf")
    dp = [[inf] * (count + 1) for _ in range(max_steps + 1)]
    parent = [[-1] * (count + 1) for _ in range(max_steps + 1)]
    dp[0][0] = 0.0

    for steps in range(1, max_steps + 1):
        for end in range(steps, count + 1):
            for start in range(steps - 1, end):
                candidate = dp[steps - 1][start] + segment_cost(
                    prefix_w, prefix_y, prefix_y2, start, end
                )
                if candidate < dp[steps][end]:
                    dp[steps][end] = candidate
                    parent[steps][end] = start

    results: dict[int, tuple[float, list[Block]]] = {}
    for steps in range(1, max_steps + 1):
        ranges: list[tuple[int, int]] = []
        end = count
        cursor = steps
        while cursor:
            start = parent[cursor][end]
            if start < 0:
                raise RuntimeError("failed to reconstruct reduced isotonic fit")
            ranges.append((start, end))
            end = start
            cursor -= 1
        ranges.reverse()
        groups = [merge_blocks(blocks[start:end]) for start, end in ranges]
        results[steps] = (dp[steps][count], groups)
    return results


def choose_fit(fits: dict[int, tuple[float, list[Block]]], total_weight: float,
               sample_count: int, penalty: float | None) -> tuple[int, list[Block], list[str]]:
    best_steps = -1
    best_objective = float("inf")
    report: list[str] = []
    for steps, (sse, groups) in sorted(fits.items()):
        mse = sse / total_weight
        if penalty is None:
            objective = sample_count * math.log(mse + 1e-12) + steps * math.log(sample_count)
            label = "BIC"
        else:
            objective = mse + penalty * steps
            label = "penalized-MSE"
        report.append(
            f"K={steps} mse={mse:.8g} {label}={objective:.8g}"
        )
        if objective < best_objective:
            best_objective = objective
            best_steps = steps
    return best_steps, fits[best_steps][1], report


def scores_from_future_gain(groups: list[Block], epsilon: float,
                            max_value: int) -> list[int]:
    baseline = max(0.0, groups[0].mean)
    values: list[int] = []
    previous = 1
    for group in groups:
        ratio = (baseline + epsilon) / (max(0.0, group.mean) + epsilon)
        value = min(max_value, max(1, int(round(ratio))))
        value = max(previous, value)
        values.append(value)
        previous = value
    values[0] = 1
    return values


def merge_equal_score_steps(groups: list[Block], values: list[int]) -> tuple[list[Block], list[int]]:
    """Remove boundaries that become meaningless after integer score export."""
    merged_groups: list[Block] = []
    merged_values: list[int] = []
    for group, value in zip(groups, values):
        if merged_values and value == merged_values[-1]:
            merged_groups[-1] = merge_blocks([merged_groups[-1], group])
        else:
            merged_groups.append(group)
            merged_values.append(value)
    return merged_groups, merged_values


def render_header(groups: list[Block], values: list[int], source: Path) -> str:
    boundaries = [group.age_min for group in groups[1:]]
    lines = [
        "/* Generated by fit_isotonic.py; do not hand-edit fitted values. */",
        f"/* Source: {source.name} */",
        "#ifndef _CONV_ISOTONIC_FIT_H",
        "#define _CONV_ISOTONIC_FIT_H",
        "",
        f"#define CONV_ISO_STEP_COUNT {len(groups)}",
    ]
    for index, boundary in enumerate(boundaries, start=1):
        lines.append(f"#define CONV_ISO_AGE_T{index}_NS {boundary}ULL")
    for index, value in enumerate(values, start=1):
        lines.append(f"#define CONV_ISO_AGE_V{index} {value}ULL")
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="CSV with age_ns and future_invalid_pages")
    parser.add_argument("-o", "--output", type=Path, help="write generated C header here")
    parser.add_argument("--max-steps", type=int, default=7, choices=range(1, 8))
    parser.add_argument(
        "--prebins", type=int, default=256,
        help="maximum ordered bins before PAVA/DP (default: 256)",
    )
    parser.add_argument(
        "--step-penalty", type=float,
        help="MSE penalty per step; omit to select K using BIC",
    )
    parser.add_argument(
        "--epsilon", type=float, default=1.0,
        help="future-invalid smoothing constant in pages (default: 1)",
    )
    parser.add_argument(
        "--max-value", type=int, default=64,
        help="maximum exported relative Age value (default: 64)",
    )
    args = parser.parse_args()
    if args.prebins < 1:
        parser.error("--prebins must be positive")
    if args.step_penalty is not None and args.step_penalty < 0:
        parser.error("--step-penalty must be non-negative")
    if args.epsilon <= 0:
        parser.error("--epsilon must be positive")
    if args.max_value < 1:
        parser.error("--max-value must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        points, sample_count = load_points(args.csv)
        binned = prebin(points, args.prebins)
        pava_blocks = pava_decreasing(binned)
        fits = reduced_fits(pava_blocks, args.max_steps)
        total_weight = sum(point.weight for point in binned)
        steps, groups, report = choose_fit(
            fits, total_weight, sample_count, args.step_penalty
        )
        values = scores_from_future_gain(groups, args.epsilon, args.max_value)
        groups, values = merge_equal_score_steps(groups, values)
        header = render_header(groups, values, args.csv)
    except (OSError, ValueError) as exc:
        print(f"fit_isotonic.py: {exc}", file=sys.stderr)
        return 2

    print(
        f"samples={sample_count} unique_ages={len(points)} prebins={len(binned)} "
        f"pava_blocks={len(pava_blocks)} selected_K={steps} exported_K={len(groups)}",
        file=sys.stderr,
    )
    for line in report:
        print(line, file=sys.stderr)
    for index, (group, value) in enumerate(zip(groups, values), start=1):
        print(
            f"step={index} age=[{group.age_min},{group.age_max}] "
            f"future_invalid={group.mean:.6g} value={value}",
            file=sys.stderr,
        )

    if args.output:
        args.output.write_text(header, encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(header)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
