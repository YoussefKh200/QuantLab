# Alpha Factory Research Outputs

`AlphaFactory::run()` results persist to `research_db/alphas/*.csv`
(via `StorageEngine::FlatFileStorage`). Use this directory for Python
notebooks that consume those CSVs for deeper analysis:

- Distribution of composite scores across candidate types
- IC decay curves (`ic_half_life`) by factor family
- Capacity vs Sharpe tradeoff scatter plots
- Promotion funnel: candidates -> validated -> live
