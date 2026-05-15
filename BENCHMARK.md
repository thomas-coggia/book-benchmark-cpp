# Benchmark presets

The `benchmark` program drives the matcher with **synthetic order streams**. Each `--profile` stands for a different kind of market condition the stream is meant to evoke.

## `quiet` — calm markets

- deeper liquidity,
- very low aggression,
- persistent passive flow.

## `active` — liquid routine tape

- tight spread,
- high interaction,
- low diffusion.

## `volatile` — stressed liquidity

- asymmetric liquidity,
- cancel cascades,
- persistent directional flow.

## `sweep` — aggressive execution pressure

- liquidity collapse,
- extreme same-side persistence,
- widening spreads,
- shallow near-touch liquidity.

## `cancel` — quote-heavy maintenance

- large resting books,
- mostly cancels with little crossing,
- sustained quote churn.

---

Run `benchmark --help` for flags. `--profile all` runs: `quiet`, `active`, `cancel`, `volatile`, `sweep`. Example numbers: [`benchmark.txt`](benchmark.txt).
