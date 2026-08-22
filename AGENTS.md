# NatOS Debugging Rules

NatOS is a custom operating system. Preserve its architecture and existing
ABI unless the evidence demonstrates that they are the cause of the problem.

When debugging:

1. Reproduce the failure before changing code.
2. Record the exact working and failing conditions.
3. Form explicit hypotheses.
4. For each hypothesis, design the smallest experiment that can distinguish
it from competing hypotheses.
5. Do not repeat hypotheses or experiments that have already been disproven.
6. Prefer experiments that change one variable at a time.
7. Do not make speculative fixes before establishing a causal mechanism.
8. Preserve a known-good Git state before invasive experiments.
9. After each experiment, record:

   * what changed
   * expected result
   * actual result
   * what hypothesis was eliminated or strengthened
10. Treat linker maps, disassembly, register state, memory addresses,
crash logs, and hardware observations as primary evidence.
11. Do not infer causation merely because two values changed together.
12. When an experiment produces a surprising result, investigate the
implication rather than immediately reverting it.
13. Keep temporary experiments isolated and clearly marked.
14. Do not modify unrelated code while investigating a bug.
15. Before proposing a fix, explain the evidence connecting the suspected
mechanism to the observed failure.

For ESP32 targets:

* Treat each chip target as hardware-specific.
* Do not assume ESP32 and ESP32-S3 have identical memory maps, cache
behavior, MMU configuration, peripherals, or PHY implementations.
* Verify the target before making low-level architectural assumptions.



Maintain the debugging record in docs/debug/ for significant investigations.

After each meaningful experiment, update the record with:

\- hypothesis

\- experiment

\- expected result

\- actual result

\- conclusion

\- next recommended experiment



Do not erase disproven hypotheses. Mark them eliminated.

