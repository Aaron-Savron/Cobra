# Cobra Language Lab

This directory is a working evaluation suite for Cobra as a complete programming language.
It exercises the language from several developer viewpoints instead of measuring only kernel speed.

## Scenarios

- `01_workbench.cb` tests application syntax, structs, strings, lists, dictionaries, and assertions.
- `02_data_pipeline.cb` tests raw buffers, regions, parallel loops, and dynamic GEMM.
- `03_module_app.cb` tests source modules and qualified calls.
- `04_http_service.cb` checks the shape of a small native HTTP service.
- `05_status_flow.cb` tests postfix status propagation through region cleanup.
- `negative/` holds programs that should be rejected with useful diagnostics.

## Run

From the repository root:

```sh
python3 codex/run_lab.py
```

The runner builds Cobra, checks every scenario, runs the executable tests, and invokes the existing
cross-language benchmark harness when its toolchain is available. Network service code is checked
and built, but is not started by the lab.

## Evaluation rules

The benchmark is only one part of the result. The lab also records:

- whether common code reads naturally to a Python developer
- where explicit systems contracts improve correctness
- where the syntax requires unnecessary ceremony
- whether diagnostics identify the source of the problem
- whether a feature is documented as implemented and actually works

The findings are recorded in `findings.md` after a run.
