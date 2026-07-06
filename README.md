# WAITGC 🧹
> **"Good Blocks Come to Those Who Wait: Invalidation-Aware GC Victim Selection"**

An ultra-low-overhead, prediction-driven Garbage Collection (GC) victim selection framework for Modern Flash SSDs. Instead of relying on static time-based aging, **WAITGC** tracks group-level write frequencies to anticipate the exact volume of future page invalidations, maximizing physical space reclamation while minimizing block migration overhead.

---

## 💡 Core Philosophy: "Let It Ripen"
Traditional GC algorithms (like Cost-Benefit or CAT) use chronological **Age** as a proxy for deadness. However, *Chronological Age ≠ Mathematical Deadness*. 

**WAITGC** redefines the victim selection pipeline by waiting for the block to "ripen"—maximizing the absolute volume of self-invalidation before execution.

---

## 🛠️ Key Architecture

WAITGC splits the problem into a **Dynamic Prediction Layer** and a **Deterministic Decision Layer**, completely avoiding heavy Reinforcement Learning (RL) or neural network overheads.

### 1. Group-Level Invalidation Prediction (Dynamic Layer)
*   **Granularity-Matching:** Tracks write frequencies at a group level (e.g., matching mapping entries) rather than per-LPN, drastically reducing metadata overhead.
*   **Self-Feedback Loop:** Continuously updates the frequency counters upon every write/overwrite without any backpropagation or gradient descent.
*   **Absolute Volume Estimation ($PoI_{abs}$):** Calculates the *expected absolute number of pages* that will turn invalid shortly, rather than a mere percentage.

### 2. Deterministic Cost Function (Decision Layer)
The prediction is fed into a mathematical cost function, evaluating candidate blocks deterministically ($O(1)$ lookup):

$$\text{Victim} = \arg\max_{b \in \text{Blocks}} \left( \frac{\text{Expected Invalidated Pages}(b)}{\text{Migration Cost}(b)} \times \text{Wear Leveling Weight}(b) \right)$$

*   **Zero RL Overhead:** No trial-and-error, no exploration-exploitation dilemma at runtime. 
*   **True Objective Alignment:** Prioritizes blocks based on the *absolute quantity* of redundant data eliminated, directly optimizing WAF (Write Amplification Factor).

---

## 📈 Evaluation Matrix & Baseline Comparison

To validate the efficiency of WAITGC, evaluate the implementation against standard baselines (**CAT**, **Cost-Benefit**) across the following metrics:

1.  **Write Amplification Factor (WAF):** Lower WAF in multi-tenant/highly random workloads due to precise time-to-die estimation.
2.  **Blocks Copied (Migration Overhead):** Fewer valid page copies during GC invocation.
3.  **Wear Variance:** Ensuring the wear-leveling weight balances structural longevity without compromising hot/cold separation.

> ⚠️ **Pre-Implementation Check:** Before full integration, measure the correlation coefficient between chronological `Age` and predicted `PoI` under highly random workloads. A low correlation justifies that WAITGC captures hidden invalidation dynamics that traditional age-based metrics completely miss.

---

## 🚀 Getting Started

### Prerequisites
*   A trace-driven SSD Simulator (e.g., MQSim, SimpleSSD) or custom FTL framework.
*   C/C++ compiler with C++17 support.

### Installation & Integration
1. Clone the repository (and strip any old remote if necessary 😉):
   ```bash
   git clone [https://github.com/your-username/WAITGC.git](https://github.com/your-username/WAITGC.git)
   cd WAITGC